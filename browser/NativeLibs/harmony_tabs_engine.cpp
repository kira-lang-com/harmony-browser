#include "harmony_tabs_internal.h"

#include "harmony_data_store.h"

#include <objbase.h>

#include <cstdio>
#include <iterator>

// The engine half of the registry: finding WebKit2.dll, resolving it, creating
// the one process context every tab shares, and running the one thread every
// WebKit object lives on.

namespace harmony_tabs {

WebKitApi g_api;
WKContextRef g_context { nullptr };
std::vector<std::unique_ptr<Tab>> g_tabs;
std::vector<ClosedTab> g_closedTabs;
int g_activeTabId { 0 };

HWND g_snapshotWindow { nullptr };
HBITMAP g_snapshotBitmap { nullptr };
int g_snapshotBitmapWidth { 0 };
int g_snapshotBitmapHeight { 0 };
ULONGLONG g_snapshotDeadline { 0 };

std::mutex g_commandMutex;
std::deque<Command> g_commands;
HANDLE g_commandEvent { nullptr };

std::mutex g_publishedMutex;
std::vector<PublishedTab> g_published;
int g_publishedActiveId { 0 };
int g_publishedClosedCount { 0 };
std::atomic<int> g_revision { 0 };

std::mutex g_hookMutex;
std::vector<PageObserver> g_pageObservers;
std::vector<PageStateObserver> g_pageStateObservers;
std::vector<UiClientHook> g_uiClientHooks;
std::vector<NavigationClientHook> g_navigationClientHooks;
std::vector<DownloadClient> g_downloadClients;
std::vector<CycleHook> g_cycleHooks;
std::vector<TeardownHook> g_teardownHooks;

std::mutex g_homeMutex;
std::string g_home { "https://www.google.com/" };

std::atomic<bool> g_ready { false };
std::atomic<bool> g_shutdownRequested { false };
std::atomic<HWND> g_parentWindow { nullptr };
std::atomic<HWND> g_activeChild { nullptr };
std::atomic<int> g_boundsX { 0 };
std::atomic<int> g_boundsY { 0 };
std::atomic<int> g_boundsWidth { 0 };
std::atomic<int> g_boundsHeight { 0 };
std::atomic<double> g_backingScale { 0.0 };
std::atomic<int> g_nextTabId { 1 };
std::atomic<DWORD> g_webKitThreadId { 0 };

namespace {

std::mutex g_errorMutex;
std::string g_error;

std::mutex g_startMutex;
HANDLE g_thread { nullptr };

constexpr char kDesktopWebKitUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/18.5 Safari/605.1.15";

using harmony::paths::executableDirectory;
using harmony::paths::fileExists;

// The engine this checkout builds, found by walking up from the running
// executable. A build lands in `.kira-build` inside the package, so the tree
// that holds `third_party` is always above it.
std::wstring checkoutWebKitDLL()
{
    std::wstring directory = executableDirectory();
    while (!directory.empty()) {
        const std::wstring candidate = directory
            + L"\\third_party\\WebKit\\WebKitBuild\\Release\\bin\\WebKit2.dll";
        if (fileExists(candidate))
            return candidate;
        const auto slash = directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos)
            break;
        directory = directory.substr(0, slash);
    }
    return { };
}

std::wstring webKitDLLPath()
{
    wchar_t buffer[32768] { };
    DWORD length = GetEnvironmentVariableW(L"HARMONY_WEBKIT_ROOT", buffer, static_cast<DWORD>(std::size(buffer)));
    if (length && length < std::size(buffer)) {
        std::wstring root(buffer, length);
        if (root.size() >= 4 && root.substr(root.size() - 4) == L".dll")
            return root;
        if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
            root += L'\\';
        return root + L"WebKit2.dll";
    }

    const auto directory = executableDirectory();
    if (!directory.empty() && fileExists(directory + L"\\WebKit2.dll"))
        return directory + L"\\WebKit2.dll";
    if (auto checkout = checkoutWebKitDLL(); !checkout.empty())
        return checkout;
    if (!directory.empty())
        return directory + L"\\WebKit2.dll";
    return L"WebKit2.dll";
}

template<typename Function>
bool loadFunction(HMODULE module, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    return function != nullptr;
}

// A symbol the engine cannot run without. The names are collected rather than
// reported one at a time, because "this build exports none of these six" is
// what identifies a WebKit built from the wrong branch, and six separate
// complaints is not.
template<typename Function>
bool loadRequired(HMODULE module, const char* name, Function& function, std::string& missing)
{
    if (loadFunction(module, name, function))
        return true;
    if (!missing.empty())
        missing += ", ";
    missing += name;
    return false;
}

bool isAsciiWhitespace(char value)
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

bool isUrlSafe(unsigned char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '-' || value == '_' || value == '.' || value == '~';
}

std::string percentEncode(const std::string& value)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (unsigned char character : value) {
        if (isUrlSafe(character)) {
            encoded += static_cast<char>(character);
            continue;
        }
        encoded += '%';
        encoded += hex[(character >> 4) & 0x0F];
        encoded += hex[character & 0x0F];
    }
    return encoded;
}

void applyBounds()
{
    if (Tab* tab = activeTab()) {
        const RECT rect = currentBoundsRect();
        tab->width = rect.right - rect.left;
        tab->height = rect.bottom - rect.top;
        requestRepaint(*tab);
    }
    // A snapshot still standing in for a page has to follow the window, or the
    // resize uncovers the very blank it was put up to hide.
    moveSnapshotToBounds();
}

void runCommand(Command& command)
{
    switch (command.kind) {
    case Command::Kind::Open:
        openTab(command.tabId, command.url, command.index, command.flag);
        break;
    case Command::Kind::Close:
        closeTab(command.tabId);
        break;
    case Command::Kind::CloseOthers:
        closeOtherTabs(command.tabId);
        break;
    case Command::Kind::CloseAll:
        closeAllTabs();
        break;
    case Command::Kind::ReopenClosed:
        reopenClosedTab(command.tabId);
        break;
    case Command::Kind::Select:
        selectTab(command.tabId);
        break;
    case Command::Kind::SelectIndex:
        selectTabAtIndex(command.index);
        break;
    case Command::Kind::SelectNext:
        selectAdjacentTab(1);
        break;
    case Command::Kind::SelectPrevious:
        selectAdjacentTab(-1);
        break;
    case Command::Kind::Move:
        moveTab(command.tabId, command.index);
        break;
    case Command::Kind::SetPinned:
        setTabPinned(command.tabId, command.flag);
        break;
    case Command::Kind::Load:
        loadInTab(command.tabId, command.url);
        break;
    case Command::Kind::Back:
        if (Tab* tab = findTab(command.tabId); tab && tab->page)
            g_api.pageGoBack(tab->page);
        break;
    case Command::Kind::Forward:
        if (Tab* tab = findTab(command.tabId); tab && tab->page)
            g_api.pageGoForward(tab->page);
        break;
    case Command::Kind::Reload:
        if (Tab* tab = findTab(command.tabId); tab && tab->page)
            g_api.pageReload(tab->page);
        break;
    case Command::Kind::Stop:
        if (Tab* tab = findTab(command.tabId); tab && tab->page && g_api.pageStopLoading)
            g_api.pageStopLoading(tab->page);
        break;
    case Command::Kind::ApplyBounds:
        applyBounds();
        break;
    case Command::Kind::ApplyBackingScale:
        applyBackingScaleToTabs();
        break;
    case Command::Kind::Invoke:
        if (command.invoke)
            command.invoke(command.invokeContext);
        break;
    case Command::Kind::Shutdown:
        g_shutdownRequested.store(true);
        break;
    }
}

void drainCommands()
{
    for (;;) {
        Command command;
        {
            std::lock_guard<std::mutex> lock(g_commandMutex);
            if (g_commands.empty())
                return;
            command = std::move(g_commands.front());
            g_commands.pop_front();
        }
        runCommand(command);
    }
}

DWORD WINAPI webKitThreadMain(LPVOID)
{
    g_webKitThreadId.store(GetCurrentThreadId());

    const HRESULT comResult = OleInitialize(nullptr);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized && comResult != RPC_E_CHANGED_MODE) {
        setError("COM/OLE initialization failed");
        g_webKitThreadId.store(0);
        return 0;
    }

    if (!loadApi() || !createContext()) {
        if (comInitialized)
            OleUninitialize();
        g_webKitThreadId.store(0);
        return 0;
    }

    g_ready.store(true);

    while (!g_shutdownRequested.load()) {
        drainCommands();
        if (g_shutdownRequested.load())
            break;

        // A page that never answers must not leave a stale frame up for good.
        if (g_snapshotDeadline && GetTickCount64() >= g_snapshotDeadline)
            hideSnapshot();

        // Before WebKit's own cycle, so work another module queued against a
        // WebKit object is issued in the same cycle it is picked up in.
        runCycleHooks();

        g_api.runLoopCycle();

        HANDLE handles[1] = { g_commandEvent };
        MsgWaitForMultipleObjectsEx(1, handles, kWorkerWaitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    g_ready.store(false);
    closeAllTabs();
    // After the pages are gone and before the context is: the last moment a
    // module that holds a WebKit object of its own can release it.
    runTeardownHooks();
    destroySnapshotWindow();
    release(g_context);
    g_context = nullptr;

    if (comInitialized)
        OleUninitialize();
    g_webKitThreadId.store(0);
    return 0;
}

} // namespace

// --- Errors -----------------------------------------------------------------

void setError(const char* message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error = message ? message : "unknown WebKit error";
    // A browser whose engine never started otherwise looks like a browser
    // drawing an empty page.
    std::fprintf(stderr, "harmony: tabs: %s\n", g_error.c_str());
}

void clearError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error.clear();
}

std::string currentError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

// --- WK memory and text -----------------------------------------------------

void release(WKTypeRef object)
{
    if (object && g_api.release)
        g_api.release(object);
}

WKTypeRef retain(WKTypeRef object)
{
    if (object && g_api.retain)
        return g_api.retain(object);
    return object;
}

std::string copyWKString(WKStringRef string)
{
    if (!string || !g_api.stringGetMaximumUTF8CStringSize)
        return { };

    const size_t capacity = g_api.stringGetMaximumUTF8CStringSize(string);
    if (capacity <= 1)
        return { };

    std::string out(capacity, '\0');
    // Strict conversion refuses a title carrying a lone surrogate outright;
    // the non-strict form substitutes the replacement character, which is what
    // a tab label should show.
    size_t written = 0;
    if (g_api.stringGetUTF8CStringNonStrict)
        written = g_api.stringGetUTF8CStringNonStrict(string, &out[0], capacity);
    else if (g_api.stringGetUTF8CString)
        written = g_api.stringGetUTF8CString(string, &out[0], capacity);
    if (written <= 1)
        return { };

    out.resize(written - 1);
    return out;
}

std::string copyWKURL(WKURLRef url)
{
    if (!url || !g_api.urlCopyString)
        return { };
    WKStringRef text = g_api.urlCopyString(url);
    std::string out = copyWKString(text);
    release(text);
    return out;
}

// --- Address normalization --------------------------------------------------

std::string normalizedURL(const char* input)
{
    if (!input)
        return { };

    std::string value(input);
    while (!value.empty() && isAsciiWhitespace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && isAsciiWhitespace(value.back()))
        value.pop_back();
    if (value.empty())
        return { };

    if (value.rfind("about:", 0) == 0 || value.find("://") != std::string::npos)
        return value;

    bool hasWhitespace = false;
    for (char character : value) {
        if (isAsciiWhitespace(character)) {
            hasWhitespace = true;
            break;
        }
    }

    if (!hasWhitespace && value.find('.') != std::string::npos)
        return "https://" + value;

    return "https://www.google.com/search?q=" + percentEncode(value);
}

std::string homeURL()
{
    std::lock_guard<std::mutex> lock(g_homeMutex);
    return g_home;
}

// --- Runtime ----------------------------------------------------------------

bool loadApi()
{
    if (g_api.module)
        return g_api.contextConfigurationCreate != nullptr;

    const auto path = webKitDLLPath();
    HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        module = LoadLibraryW(L"WebKit2.dll");
    if (!module) {
        setError("WebKit2.dll was not found beside this executable, in this "
                 "checkout's WebKitBuild\\Release\\bin, or on the system path; "
                 "build it with scripts\\build-webkit-windows.ps1 or set "
                 "HARMONY_WEBKIT_ROOT");
        return false;
    }

    WebKitApi api;
    api.module = module;
    std::string missing;
    bool ok = true;
    ok &= loadRequired(module, "WKContextConfigurationCreate", api.contextConfigurationCreate, missing);
    ok &= loadRequired(module, "WKContextCreateWithConfiguration", api.contextCreateWithConfiguration, missing);
    ok &= loadRequired(module, "WKPageConfigurationCreate", api.pageConfigurationCreate, missing);
    ok &= loadRequired(module, "WKPageConfigurationSetContext", api.pageConfigurationSetContext, missing);
    ok &= loadRequired(module, "WKViewCreate", api.viewCreate, missing);
    ok &= loadRequired(module, "WKViewGetWindow", api.viewGetWindow, missing);
    ok &= loadRequired(module, "WKViewGetPage", api.viewGetPage, missing);
    ok &= loadRequired(module, "WKViewSetParentWindow", api.viewSetParentWindow, missing);
    ok &= loadRequired(module, "WKViewSetIsInWindow", api.viewSetIsInWindow, missing);
    ok &= loadRequired(module, "WKRunLoopCycle", api.runLoopCycle, missing);
    ok &= loadRequired(module, "WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString, missing);
    ok &= loadRequired(module, "WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize, missing);
    ok &= loadRequired(module, "WKStringGetUTF8CString", api.stringGetUTF8CString, missing);
    ok &= loadRequired(module, "WKURLCreateWithUTF8CString", api.urlCreateWithUTF8CString, missing);
    ok &= loadRequired(module, "WKURLCopyString", api.urlCopyString, missing);
    ok &= loadRequired(module, "WKURLCopyHostName", api.urlCopyHostName, missing);
    ok &= loadRequired(module, "WKPageSetCustomUserAgent", api.pageSetCustomUserAgent, missing);
    ok &= loadRequired(module, "WKPageSetCustomBackingScaleFactor", api.pageSetCustomBackingScaleFactor, missing);
    ok &= loadRequired(module, "WKPageSetPageUIClient", api.pageSetPageUIClient, missing);
    ok &= loadRequired(module, "WKPageSetPageStateClient", api.pageSetPageStateClient, missing);
    // The navigation client is not optional: it is what answers a policy
    // listener, and a listener nobody answers is a load that never starts.
    ok &= loadRequired(module, "WKPageSetPageNavigationClient", api.pageSetPageNavigationClient, missing);
    ok &= loadRequired(module, "WKFramePolicyListenerUse", api.framePolicyListenerUse, missing);
    ok &= loadRequired(module, "WKFramePolicyListenerDownload", api.framePolicyListenerDownload, missing);
    ok &= loadRequired(module, "WKPageCopyTitle", api.pageCopyTitle, missing);
    ok &= loadRequired(module, "WKPageCopyActiveURL", api.pageCopyActiveURL, missing);
    ok &= loadRequired(module, "WKPageCopyPendingAPIRequestURL", api.pageCopyPendingAPIRequestURL, missing);
    ok &= loadRequired(module, "WKPageGetEstimatedProgress", api.pageGetEstimatedProgress, missing);
    ok &= loadRequired(module, "WKPageCanGoBack", api.pageCanGoBack, missing);
    ok &= loadRequired(module, "WKPageCanGoForward", api.pageCanGoForward, missing);
    ok &= loadRequired(module, "WKPageLoadURL", api.pageLoadURL, missing);
    ok &= loadRequired(module, "WKPageGoBack", api.pageGoBack, missing);
    ok &= loadRequired(module, "WKPageGoForward", api.pageGoForward, missing);
    ok &= loadRequired(module, "WKPageReload", api.pageReload, missing);
    ok &= loadRequired(module, "WKPageStopLoading", api.pageStopLoading, missing);
    ok &= loadRequired(module, "WKPageClose", api.pageClose, missing);
    ok &= loadRequired(module, "WKRetain", api.retain, missing);
    ok &= loadRequired(module, "WKRelease", api.release, missing);

    if (!ok) {
        setError(("this WebKit build does not export " + missing).c_str());
        FreeLibrary(module);
        return false;
    }

    // Optional, and left null when absent: a build without these still browses;
    // it just does so with weaker repainting during a host resize, with a weaker
    // guarantee that two tabs are two processes, with strict UTF-8 conversion of
    // page titles, or by leaving a geolocation request to WebKit's own refusal
    // path. Every call site checks the pointer first.
    loadFunction(module, "WKStringGetUTF8CStringNonStrict", api.stringGetUTF8CStringNonStrict);
    loadFunction(module, "WKPageForceRepaint", api.pageForceRepaint);
    loadFunction(module, "WKContextConfigurationSetProcessSwapsOnNavigation", api.contextConfigurationSetProcessSwapsOnNavigation);
    loadFunction(module, "WKContextSetUsesSingleWebProcess", api.contextSetUsesSingleWebProcess);
    loadFunction(module, "WKGeolocationPermissionRequestDeny", api.geolocationPermissionRequestDeny);

    g_api = api;
    clearError();
    return true;
}

bool createContext()
{
    auto configuration = g_api.contextConfigurationCreate();
    if (!configuration) {
        setError("WKContextConfigurationCreate failed");
        return false;
    }

    // A navigation that crosses sites should land in a different process, the
    // same as it would in any other browser.
    if (g_api.contextConfigurationSetProcessSwapsOnNavigation)
        g_api.contextConfigurationSetProcessSwapsOnNavigation(configuration, true);

    // The profile has to exist before the first page configuration is built,
    // and it is prepare() that registers the profile's page observer, cycle
    // hook and teardown hook on this registry. Both run here, on the WebKit
    // thread, before the context they configure is created.
    hb_data_store_prepare();
    hb_data_store_apply_to_context_configuration(const_cast<void*>(configuration));

    g_context = g_api.contextCreateWithConfiguration(configuration);
    release(configuration);
    if (!g_context) {
        setError("WKContextCreateWithConfiguration failed");
        return false;
    }

    if (g_api.contextSetUsesSingleWebProcess)
        g_api.contextSetUsesSingleWebProcess(g_context, false);
    return true;
}

const char* desktopUserAgent()
{
    return kDesktopWebKitUserAgent;
}

// --- Command queue and thread ----------------------------------------------

void postCommand(Command&& command)
{
    {
        std::lock_guard<std::mutex> lock(g_commandMutex);
        g_commands.push_back(std::move(command));
    }
    if (g_commandEvent)
        SetEvent(g_commandEvent);
}

void postSimpleCommand(Command::Kind kind, int tabId)
{
    Command command;
    command.kind = kind;
    command.tabId = tabId;
    postCommand(std::move(command));
}

bool startWebKitThread()
{
    std::lock_guard<std::mutex> lock(g_startMutex);
    if (g_thread)
        return true;

    if (!g_commandEvent) {
        g_commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_commandEvent) {
            setError("could not create the WebKit command event");
            return false;
        }
    }

    g_shutdownRequested.store(false);
    g_thread = CreateThread(nullptr, 0, webKitThreadMain, nullptr, 0, nullptr);
    if (!g_thread) {
        setError("could not start the WebKit thread");
        return false;
    }
    return true;
}

void stopWebKitThread()
{
    HANDLE thread = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_startMutex);
        thread = g_thread;
        g_thread = nullptr;
    }
    if (!thread)
        return;

    postSimpleCommand(Command::Kind::Shutdown);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);

    if (g_commandEvent) {
        CloseHandle(g_commandEvent);
        g_commandEvent = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_commandMutex);
    g_commands.clear();
}

} // namespace harmony_tabs
