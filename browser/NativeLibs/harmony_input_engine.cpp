#include "harmony_input_internal.h"

#include <objbase.h>
#include <shlobj.h>

#include <cstdio>

// The module's spine: the state the two threads share, the entry points
// resolved out of the loaded engine, the event queue the host drains, and the
// attachment to the tab registry that gives this module a page to work on.

namespace harmony_input {

std::atomic<HWND> g_hostWindow { nullptr };
std::atomic<DWORD> g_frameThreadId { 0 };
std::atomic<DWORD> g_engineThreadId { 0 };
std::atomic<bool> g_hooksReady { false };
std::atomic<int> g_textTarget { HB_INPUT_TARGET_NONE };
std::atomic<bool> g_findOpen { false };
std::atomic<int> g_activeTabId { 0 };

namespace {

std::mutex g_errorMutex;
std::string g_error;

std::atomic<int> g_revision { 0 };

std::mutex g_eventMutex;
std::deque<Event> g_events;

std::mutex g_windowMutex;
std::vector<std::pair<int, HWND>> g_tabWindows;

std::atomic<bool> g_attached { false };

// Engine thread.
WebKitApi g_api;
bool g_apiResolved { false };
bool g_apiUsable { false };

// The known folder id for %LOCALAPPDATA%, spelled out so the link line needs no
// uuid.lib.
const KNOWNFOLDERID kFolderLocalAppData = {
    0xF1B32785, 0x6FBA, 0x4FCF, { 0x9D, 0x55, 0x7B, 0x8E, 0x7F, 0x15, 0x70, 0x91 }
};

constexpr wchar_t kProfileFolderName[] = L"HarmonyBrowser";

template<typename Function>
bool resolve(const char* name, Function& function, std::string& missing)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
    if (function)
        return true;

    if (!missing.empty())
        missing += ", ";
    missing += name;
    return false;
}

bool resolveApi()
{
    WebKitApi api;
    std::string missing;
    bool ok = true;
    ok &= resolve("WKViewGetWindow", api.viewGetWindow, missing);
    ok &= resolve("WKPageGetPageZoomFactor", api.pageGetPageZoomFactor, missing);
    ok &= resolve("WKPageSetPageZoomFactor", api.pageSetPageZoomFactor, missing);
    ok &= resolve("WKPageGetMainFrame", api.pageGetMainFrame, missing);
    ok &= resolve("WKPageFindString", api.pageFindString, missing);
    ok &= resolve("WKPageHideFindUI", api.pageHideFindUI, missing);
    ok &= resolve("WKPageCountStringMatches", api.pageCountStringMatches, missing);
    ok &= resolve("WKPageSetPageFindClient", api.pageSetPageFindClient, missing);
    ok &= resolve("WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString, missing);
    ok &= resolve("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize, missing);
    ok &= resolve("WKStringGetUTF8CStringNonStrict", api.stringGetUTF8CStringNonStrict, missing);
    ok &= resolve("WKRelease", api.release, missing);

    if (!ok) {
        setError("this WebKit build does not export " + missing);
        return false;
    }

    g_api = api;
    return true;
}

// Engine thread, from the registry, just after a page is created. The page's
// window is what the keyboard is handed to and taken from, and the find client
// is installed here because a page carries exactly one.
void pageCreated(int tabId, void* page, void*)
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return;

    if (WKViewRef view = hb_tabs_view(tabId))
        recordTabWindow(tabId, api->viewGetWindow(view));

    attachFindClient(tabId, page);
}

void pageDestroying(int tabId, void* page, void*)
{
    detachFindClient(tabId, page);
    forgetTabWindow(tabId);
}

void uiClientCreated(int tabId, hb_wk_page_ui_client_v19* client, void*)
{
    installFocusClient(tabId, client);
}

// Engine thread. Everything this module needs that only that thread may do.
void bootstrapEngineThread(void*)
{
    g_engineThreadId.store(GetCurrentThreadId());
    installEngineHook();

    // The two windows are one window's worth of keyboard: the page's child is
    // owned by this thread and the chrome by the frame thread, and until the
    // two input queues are one, neither thread can move the focus to the
    // other's window -- which is the whole of what a browser's Ctrl+L does.
    const DWORD frameThread = g_frameThreadId.load();
    if (frameThread && frameThread != GetCurrentThreadId()) {
        AttachThreadInput(frameThread, GetCurrentThreadId(), TRUE);
        AttachThreadInput(GetCurrentThreadId(), frameThread, TRUE);
    }

    g_hooksReady.store(true);
    bumpRevision();
}

void detachEngineThread(void*)
{
    removeEngineHook();

    const DWORD frameThread = g_frameThreadId.load();
    if (frameThread && frameThread != GetCurrentThreadId()) {
        AttachThreadInput(frameThread, GetCurrentThreadId(), FALSE);
        AttachThreadInput(GetCurrentThreadId(), frameThread, FALSE);
    }

    g_engineThreadId.store(0);
    g_hooksReady.store(false);
}

// Engine thread. Adopts every tab this module has not seen the window of.
//
// The page observer answers for every tab opened after this module attached,
// and the first tab of a run is opened by the frame that attaches it -- the two
// are queued onto the same thread in an order neither one decides. So the list
// is swept as well, and a tab that was already open when the hooks went in is
// picked up on the next frame rather than being a page the keyboard can never
// leave.
void sweepTabWindows(void*)
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return;

    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int tab = hb_tabs_id_at(index);
        if (tab <= 0 || tabWindow(tab))
            continue;

        WKViewRef view = hb_tabs_view(tab);
        if (!view)
            continue;

        recordTabWindow(tab, api->viewGetWindow(view));
        attachFindClient(tab, hb_tabs_page(tab));
    }
}

// Frame thread. Whether the registry holds a tab whose window is not known
// here yet.
bool tabWindowsAreMissing()
{
    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int tab = hb_tabs_id_at(index);
        if (tab > 0 && !tabWindow(tab))
            return true;
    }
    return false;
}

// Frame thread. Attaches to the tab registry once, the first time the host
// draws a frame with a window to draw it in.
void attachOnce(HWND host)
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    g_frameThreadId.store(GetCurrentThreadId());
    g_hostWindow.store(host);

    loadZoomLevels();

    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
    hb_tabs_add_ui_client_hook(uiClientCreated, nullptr);

    installHooks();
    hb_tabs_invoke_on_webkit_thread(bootstrapEngineThread, nullptr);
}

} // namespace

// --- Errors and revisions ---------------------------------------------------

void setError(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        g_error = message;
    }
    // A shortcut that silently does nothing is indistinguishable from a
    // keyboard that is not working.
    std::fprintf(stderr, "harmony: input: %s\n", message.c_str());
}

std::string currentError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

void bumpRevision()
{
    g_revision.fetch_add(1);
}

// --- The engine's entry points ----------------------------------------------

const WebKitApi* webKitApi()
{
    if (!g_apiResolved) {
        g_apiResolved = true;
        g_apiUsable = resolveApi();
    }
    return g_apiUsable ? &g_api : nullptr;
}

void releaseObject(WKTypeRef object)
{
    if (object && g_apiUsable && g_api.release)
        g_api.release(object);
}

WKStringRef createString(const std::string& text)
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return nullptr;
    return api->stringCreateWithUTF8CString(text.c_str());
}

// --- Events -----------------------------------------------------------------

void queueEvent(int kind, int argument, int target)
{
    if (kind == HB_INPUT_EVENT_NONE)
        return;

    {
        std::lock_guard<std::mutex> lock(g_eventMutex);
        g_events.push_back(Event { kind, argument, target });
    }
    bumpRevision();
}

bool popEvent(Event& event)
{
    std::lock_guard<std::mutex> lock(g_eventMutex);
    if (g_events.empty())
        return false;
    event = g_events.front();
    g_events.pop_front();
    return true;
}

// --- The page's windows -----------------------------------------------------

void recordTabWindow(int tabId, HWND window)
{
    if (tabId <= 0 || !window)
        return;

    std::lock_guard<std::mutex> lock(g_windowMutex);
    for (auto& entry : g_tabWindows) {
        if (entry.first == tabId) {
            entry.second = window;
            return;
        }
    }
    g_tabWindows.emplace_back(tabId, window);
}

void forgetTabWindow(int tabId)
{
    std::lock_guard<std::mutex> lock(g_windowMutex);
    for (size_t index = 0; index < g_tabWindows.size(); ++index) {
        if (g_tabWindows[index].first == tabId) {
            g_tabWindows.erase(g_tabWindows.begin() + static_cast<ptrdiff_t>(index));
            return;
        }
    }
}

HWND tabWindow(int tabId)
{
    std::lock_guard<std::mutex> lock(g_windowMutex);
    for (const auto& entry : g_tabWindows) {
        if (entry.first == tabId)
            return entry.second;
    }
    return nullptr;
}

bool isTabWindow(HWND window)
{
    if (!window)
        return false;

    std::lock_guard<std::mutex> lock(g_windowMutex);
    for (const auto& entry : g_tabWindows) {
        if (entry.second == window)
            return true;
    }
    return false;
}

// --- The profile ------------------------------------------------------------

std::wstring profileDirectory()
{
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(kFolderLocalAppData, KF_FLAG_CREATE, nullptr, &raw);
    if (FAILED(result) || !raw) {
        if (raw)
            CoTaskMemFree(raw);
        return { };
    }

    std::wstring path(raw);
    CoTaskMemFree(raw);
    if (!path.empty() && path.back() != L'\\')
        path += L'\\';
    path += kProfileFolderName;

    if (!CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        return { };
    return path;
}

std::string originOfURL(const std::string& url)
{
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        // about: and data: pages have no site to remember a level for.
        return { };
    }

    const size_t authority = scheme + 3;
    size_t end = url.size();
    for (size_t index = authority; index < url.size(); ++index) {
        const char character = url[index];
        if (character == '/' || character == '?' || character == '#') {
            end = index;
            break;
        }
    }
    if (end == authority)
        return { };

    std::string origin = url.substr(0, end);
    // Credentials belong to a request rather than to a site.
    const size_t at = origin.find('@', authority);
    if (at != std::string::npos)
        origin = origin.substr(0, authority) + origin.substr(at + 1);
    return origin;
}

} // namespace harmony_input

// --- The host's surface -----------------------------------------------------

using namespace harmony_input;

extern "C" void hb_input_attach(void* host_window)
{
    if (!host_window)
        return;
    attachOnce(static_cast<HWND>(host_window));
}

extern "C" void hb_input_frame(void* host_window)
{
    if (!host_window)
        return;

    HWND host = static_cast<HWND>(host_window);
    attachOnce(host);
    g_hostWindow.store(host);
    g_activeTabId.store(hb_tabs_active_id());

    if (tabWindowsAreMissing())
        hb_tabs_invoke_on_webkit_thread(sweepTabWindows, nullptr);

    serviceFocus();
    serviceZoom();
}

extern "C" void hb_input_shutdown(void)
{
    if (!g_attached.load())
        return;

    stopPrinting();
    saveZoomLevels();
    removeHooks();

    if (g_engineThreadId.load())
        hb_tabs_invoke_on_webkit_thread(detachEngineThread, nullptr);
}

extern "C" int hb_input_ready(void)
{
    return g_hooksReady.load() ? 1 : 0;
}

extern "C" const char* hb_input_error(void)
{
    static thread_local std::string storage;
    storage = currentError();
    return storage.c_str();
}

extern "C" int hb_input_revision(void)
{
    return g_revision.load();
}

// The event the caller's thread last popped. Held per thread so the two
// accessors describe that thread's own event and never another's.
static thread_local Event t_currentEvent;

extern "C" int hb_input_next_event(void)
{
    Event event;
    if (!popEvent(event)) {
        t_currentEvent = Event { };
        return HB_INPUT_EVENT_NONE;
    }
    t_currentEvent = event;
    return event.kind;
}

extern "C" int hb_input_event_argument(void)
{
    return t_currentEvent.argument;
}

extern "C" int hb_input_event_target(void)
{
    return t_currentEvent.target;
}
