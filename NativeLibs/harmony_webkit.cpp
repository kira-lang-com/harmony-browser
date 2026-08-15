#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>

#include "harmony_webkit.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace {

using WKTypeRef = const void*;
using WKContextConfigurationRef = const void*;
using WKContextRef = const void*;
using WKPageConfigurationRef = const void*;
using WKPageRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKViewRef = const void*;

struct WebKitApi {
    HMODULE module { nullptr };

    WKContextConfigurationRef (*contextConfigurationCreate)() { nullptr };
    void (*contextConfigurationSetProcessSwapsOnNavigation)(WKContextConfigurationRef, bool) { nullptr };
    WKContextRef (*contextCreateWithConfiguration)(WKContextConfigurationRef) { nullptr };
    void (*contextSetUsesSingleWebProcess)(WKContextRef, bool) { nullptr };
    WKPageConfigurationRef (*pageConfigurationCreate)() { nullptr };
    void (*pageConfigurationSetContext)(WKPageConfigurationRef, WKContextRef) { nullptr };
    WKViewRef (*viewCreate)(RECT, WKPageConfigurationRef, HWND) { nullptr };
    HWND (*viewGetWindow)(WKViewRef) { nullptr };
    WKPageRef (*viewGetPage)(WKViewRef) { nullptr };
    void (*viewSetParentWindow)(WKViewRef, HWND) { nullptr };
    void (*viewSetIsInWindow)(WKViewRef, bool) { nullptr };
    void (*runLoopCycle)() { nullptr };
    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    WKURLRef (*urlCreateWithUTF8CString)(const char*) { nullptr };
    void (*pageSetCustomUserAgent)(WKPageRef, WKStringRef) { nullptr };
    void (*pageSetCustomBackingScaleFactor)(WKPageRef, double) { nullptr };
    void (*pageForceRepaint)(WKPageRef, void*, void (*)(WKTypeRef, void*)) { nullptr };
    void (*pageLoadURL)(WKPageRef, WKURLRef) { nullptr };
    void (*pageGoBack)(WKPageRef) { nullptr };
    void (*pageGoForward)(WKPageRef) { nullptr };
    void (*pageReload)(WKPageRef) { nullptr };
    void (*pageClose)(WKPageRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };
};

// One open tab. Everything in it belongs to the WebKit thread.
//
// A slot is the host's name for a tab that has a fixed place in its UI, so a
// row can ask for "its" tab without tracking an id. A tab opened without one
// carries -1 and is reachable only by id.
struct Tab {
    int id { 0 };
    int slot { -1 };
    WKViewRef view { nullptr };
    WKPageRef page { nullptr };
    HWND child { nullptr };
    int width { 0 };
    int height { 0 };
    bool repaintInFlight { false };
    bool suspended { false };

    // The last frame this tab had on screen, kept so switching back to it can
    // show something immediately. A tab that has never been shown has none, and
    // shows white until its page renders.
    HBITMAP snapshot { nullptr };
    int snapshotWidth { 0 };
    int snapshotHeight { 0 };
};

struct Command {
    enum class Kind {
        OpenTab,
        ActivateSlot,
        CloseTab,
        CloseSlot,
        SelectTab,
        LoadURL,
        Back,
        Forward,
        Reload,
        ForceRepaint,
        Shutdown,
    };

    Kind kind { Kind::ForceRepaint };
    int tabId { 0 };
    int slot { -1 };
    std::string url;
};

// --- Owned by the WebKit thread ---------------------------------------------

WebKitApi g_api;
WKContextRef g_context { nullptr };
std::vector<Tab> g_tabs;
int g_activeTabId { 0 };
double g_backingScaleFactor { 0 };

// The window that shows a tab's last frame while its page catches up.
//
// A background tab is suspended: hidden, and told it is out of the window, so
// its process is not rendering anything. That is the only way a browser holds
// fifty tabs. The cost is that switching to one has nothing to show until the
// page renders again, so the frame it had when it was last on screen is kept
// and put up immediately, and taken down once the real view has rendered.
HWND g_snapshotWindow { nullptr };
HBITMAP g_snapshotBitmap { nullptr };
int g_snapshotBitmapWidth { 0 };
int g_snapshotBitmapHeight { 0 };
ULONGLONG g_snapshotDeadline { 0 };

// How long a stale frame may stand in for a page that has not rendered yet.
constexpr ULONGLONG kSnapshotHoldMs = 400;

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// --- Shared ------------------------------------------------------------------

std::mutex g_errorMutex;
std::string g_error;

std::mutex g_commandMutex;
std::deque<Command> g_commands;
HANDLE g_commandEvent { nullptr };

std::mutex g_snapshotMutex;
std::vector<int> g_snapshotTabIds;
int g_snapshotActiveTabId { 0 };

std::mutex g_startMutex;
HANDLE g_thread { nullptr };

std::atomic<bool> g_ready { false };
std::atomic<bool> g_shutdownRequested { false };
std::atomic<HWND> g_parentWindow { nullptr };
std::atomic<HWND> g_activeChild { nullptr };
std::atomic<int> g_boundsX { 0 };
std::atomic<int> g_boundsY { 0 };
std::atomic<int> g_boundsWidth { 0 };
std::atomic<int> g_boundsHeight { 0 };
std::atomic<int> g_nextTabId { 1 };

// How long the WebKit thread sleeps when it has nothing to do. It is a ceiling
// rather than a period: a posted command wakes it immediately. WebKit's timers
// only fire from a run loop cycle, so the wait also bounds how late one can be.
constexpr DWORD kWorkerWaitMs = 8;

constexpr char kDesktopWebKitUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/18.5 Safari/605.1.15";

// The property the original window procedure of a subclassed tab is kept under.
constexpr wchar_t kOriginalProcProperty[] = L"HarmonyWebKitOriginalProc";

// WebKit's Windows view class is registered with no background brush, so a view
// with nothing to paint — one whose page has not rendered at this size yet — is
// left showing whatever was in the window, which reads as black. Erasing it
// white makes the same moment read as a page that has not loaded yet.
LRESULT CALLBACK tabChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kOriginalProcProperty));

    if (message == WM_ERASEBKGND) {
        RECT clientRect { };
        GetClientRect(window, &clientRect);
        FillRect(reinterpret_cast<HDC>(wparam), &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }

    if (message == WM_NCDESTROY) {
        RemovePropW(window, kOriginalProcProperty);
        if (original)
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    }

    if (!original)
        return DefWindowProcW(window, message, wparam, lparam);
    return CallWindowProcW(original, window, message, wparam, lparam);
}

void subclassTabChild(HWND child)
{
    if (!child || GetPropW(child, kOriginalProcProperty))
        return;

    auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
    if (!original)
        return;

    SetPropW(child, kOriginalProcProperty, reinterpret_cast<HANDLE>(original));
    SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(tabChildProc));
}

constexpr wchar_t kSnapshotWindowClassName[] = L"HarmonyWebKitSnapshotWindow";

LRESULT CALLBACK snapshotWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ERASEBKGND) {
        RECT clientRect { };
        GetClientRect(window, &clientRect);
        FillRect(reinterpret_cast<HDC>(wparam), &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT paint { };
        HDC dc = BeginPaint(window, &paint);
        RECT clientRect { };
        GetClientRect(window, &clientRect);

        if (g_snapshotBitmap && g_snapshotBitmapWidth > 0 && g_snapshotBitmapHeight > 0) {
            HDC memoryDC = CreateCompatibleDC(dc);
            if (memoryDC) {
                HGDIOBJ previous = SelectObject(memoryDC, g_snapshotBitmap);
                // Stretched rather than clipped: a window that has been resized
                // since the frame was taken should show the whole page scaled,
                // not a corner of it.
                StretchBlt(
                    dc,
                    0,
                    0,
                    clientRect.right,
                    clientRect.bottom,
                    memoryDC,
                    0,
                    0,
                    g_snapshotBitmapWidth,
                    g_snapshotBitmapHeight,
                    SRCCOPY
                );
                SelectObject(memoryDC, previous);
                DeleteDC(memoryDC);
            }
        } else
            FillRect(dc, &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        EndPaint(window, &paint);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

HWND snapshotWindow(HWND parent)
{
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        return g_snapshotWindow;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass { };
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = snapshotWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        // IDC_ARROW resolves to the narrow form unless UNICODE is defined, and
        // this translation unit does not define it.
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        windowClass.lpszClassName = kSnapshotWindowClassName;
        if (!RegisterClassExW(&windowClass))
            return nullptr;
        registered = true;
    }

    g_snapshotWindow = CreateWindowExW(
        0,
        kSnapshotWindowClassName,
        nullptr,
        WS_CHILD | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    return g_snapshotWindow;
}

void hideSnapshot()
{
    g_snapshotDeadline = 0;
    g_snapshotBitmap = nullptr;
    g_snapshotBitmapWidth = 0;
    g_snapshotBitmapHeight = 0;
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        ShowWindow(g_snapshotWindow, SW_HIDE);
}

void setError(const char* message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error = message ? message : "unknown WebKit error";
    // The caller discards the start result, and a browser whose engine never
    // started otherwise looks like a browser drawing an empty page.
    std::fprintf(stderr, "harmony: webkit: %s\n", g_error.c_str());
}

void clearError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error.clear();
}

void release(WKTypeRef object)
{
    if (object && g_api.release)
        g_api.release(object);
}

std::wstring executableDirectory()
{
    wchar_t buffer[32768] { };
    DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer))
        return { };

    std::wstring path(buffer, length);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return { };
    return path.substr(0, slash);
}

bool fileExists(const std::wstring& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// The engine this checkout builds, found by walking up from the running
// executable. A build lands in `.kira-build` inside the package, so the tree
// that holds `third_party` is always above it — and a browser that carries its
// own engine should not have to be told where it put it.
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

double backingScaleFactorForWindow(HWND window)
{
    UINT dpi = window ? GetDpiForWindow(window) : 0;
    if (!dpi)
        dpi = GetDpiForSystem();
    if (!dpi)
        return 1;

    // WebKit's Windows port rounds fractional scale factors to avoid noisy
    // backing-store updates. Keep the same policy as MiniBrowser.
    return std::max(1.0, std::round(static_cast<double>(dpi) / 96.0));
}

template<typename Function>
bool loadFunction(HMODULE module, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    if (!function) {
        setError(name);
        return false;
    }
    return true;
}

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
    bool ok = true;
    ok &= loadFunction(module, "WKContextConfigurationCreate", api.contextConfigurationCreate);
    ok &= loadFunction(module, "WKContextCreateWithConfiguration", api.contextCreateWithConfiguration);
    ok &= loadFunction(module, "WKPageConfigurationCreate", api.pageConfigurationCreate);
    ok &= loadFunction(module, "WKPageConfigurationSetContext", api.pageConfigurationSetContext);
    ok &= loadFunction(module, "WKViewCreate", api.viewCreate);
    ok &= loadFunction(module, "WKViewGetWindow", api.viewGetWindow);
    ok &= loadFunction(module, "WKViewGetPage", api.viewGetPage);
    ok &= loadFunction(module, "WKViewSetParentWindow", api.viewSetParentWindow);
    ok &= loadFunction(module, "WKViewSetIsInWindow", api.viewSetIsInWindow);
    ok &= loadFunction(module, "WKRunLoopCycle", api.runLoopCycle);
    ok &= loadFunction(module, "WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString);
    ok &= loadFunction(module, "WKURLCreateWithUTF8CString", api.urlCreateWithUTF8CString);
    ok &= loadFunction(module, "WKPageSetCustomUserAgent", api.pageSetCustomUserAgent);
    ok &= loadFunction(module, "WKPageSetCustomBackingScaleFactor", api.pageSetCustomBackingScaleFactor);
    ok &= loadFunction(module, "WKPageLoadURL", api.pageLoadURL);
    ok &= loadFunction(module, "WKPageGoBack", api.pageGoBack);
    ok &= loadFunction(module, "WKPageGoForward", api.pageGoForward);
    ok &= loadFunction(module, "WKPageReload", api.pageReload);
    ok &= loadFunction(module, "WKPageClose", api.pageClose);
    ok &= loadFunction(module, "WKRelease", api.release);

    if (!ok) {
        FreeLibrary(module);
        return false;
    }

    // Optional. A build without these still browses; it just does so with
    // weaker repainting during a host resize, or with a weaker guarantee that
    // two tabs are two processes.
    if (!loadFunction(module, "WKPageForceRepaint", api.pageForceRepaint))
        api.pageForceRepaint = nullptr;
    if (!loadFunction(module, "WKContextConfigurationSetProcessSwapsOnNavigation", api.contextConfigurationSetProcessSwapsOnNavigation))
        api.contextConfigurationSetProcessSwapsOnNavigation = nullptr;
    if (!loadFunction(module, "WKContextSetUsesSingleWebProcess", api.contextSetUsesSingleWebProcess))
        api.contextSetUsesSingleWebProcess = nullptr;

    g_api = api;
    clearError();
    return true;
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

void postCommand(Command&& command)
{
    {
        std::lock_guard<std::mutex> lock(g_commandMutex);
        g_commands.push_back(std::move(command));
    }
    if (g_commandEvent)
        SetEvent(g_commandEvent);
}

void postSimpleCommand(Command::Kind kind, int tabId = 0)
{
    Command command;
    command.kind = kind;
    command.tabId = tabId;
    postCommand(std::move(command));
}

// --- WebKit thread -----------------------------------------------------------

void closeTab(int id);

Tab* findTab(int id)
{
    for (auto& tab : g_tabs) {
        if (tab.id == id)
            return &tab;
    }
    return nullptr;
}

Tab* activeTab()
{
    return g_activeTabId ? findTab(g_activeTabId) : nullptr;
}

Tab* findTabBySlot(int slot)
{
    if (slot < 0)
        return nullptr;
    for (auto& tab : g_tabs) {
        if (tab.slot == slot)
            return &tab;
    }
    return nullptr;
}

void publishSnapshot()
{
    std::vector<int> ids;
    ids.reserve(g_tabs.size());
    for (const auto& tab : g_tabs)
        ids.push_back(tab.id);

    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    g_snapshotTabIds = std::move(ids);
    g_snapshotActiveTabId = g_activeTabId;
}

void configurePage(WKPageRef page, HWND parent)
{
    if (!page)
        return;

    auto userAgent = g_api.stringCreateWithUTF8CString(kDesktopWebKitUserAgent);
    if (userAgent) {
        g_api.pageSetCustomUserAgent(page, userAgent);
        release(userAgent);
    }

    const double scale = backingScaleFactorForWindow(parent);
    g_api.pageSetCustomBackingScaleFactor(page, scale);
    g_backingScaleFactor = scale;
}

void loadInTab(const Tab& tab, const std::string& url)
{
    if (!tab.page || url.empty())
        return;

    const auto target = normalizedURL(url.c_str());
    if (target.empty())
        return;

    auto webURL = g_api.urlCreateWithUTF8CString(target.c_str());
    if (!webURL)
        return;

    g_api.pageLoadURL(tab.page, webURL);
    release(webURL);
}

// Runs on the WebKit thread, from the run loop, once the page has rendered.
void repaintCompleted(WKTypeRef, void* context)
{
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(context));
    if (Tab* tab = findTab(id))
        tab->repaintInFlight = false;

    // The view underneath has pixels now, so the frame standing in for it can
    // come down.
    if (id == g_activeTabId)
        hideSnapshot();
}

// Asks a page for a frame. In accelerated compositing mode the UI process holds
// no pixels at all — the view's content is a surface the GPU process presents
// when the page renders — so this is the only way to put pixels back after the
// host's swapchain reconfigure, or a resize, has dropped them.
//
// The in-flight flag is per tab: a repaint owed to the tab being switched away
// from must not swallow the one the incoming tab needs.
void requestRepaint(Tab& tab)
{
    if (!g_api.pageForceRepaint || !tab.page || tab.repaintInFlight)
        return;

    tab.repaintInFlight = true;
    g_api.pageForceRepaint(tab.page, reinterpret_cast<void*>(static_cast<intptr_t>(tab.id)), repaintCompleted);
}

RECT currentBoundsRect()
{
    const int x = g_boundsX.load();
    const int y = g_boundsY.load();
    const int width = std::max(1, g_boundsWidth.load());
    const int height = std::max(1, g_boundsHeight.load());
    return RECT { x, y, x + width, y + height };
}

void releaseSnapshot(Tab& tab)
{
    if (g_snapshotBitmap == tab.snapshot)
        hideSnapshot();
    if (tab.snapshot)
        DeleteObject(tab.snapshot);
    tab.snapshot = nullptr;
    tab.snapshotWidth = 0;
    tab.snapshotHeight = 0;
}

// Keeps the frame a tab has on screen right now, so switching back to it has
// something to show while its page renders again.
//
// `PW_RENDERFULLCONTENT` is what asks for a window's composed content rather
// than the result of sending it a paint message, which is what a view whose
// pixels belong to the GPU process needs. A capture that comes back with
// nothing leaves the white underneath, which reads as a page still loading.
void captureSnapshot(Tab& tab)
{
    if (!tab.child || !IsWindow(tab.child))
        return;

    RECT clientRect { };
    GetClientRect(tab.child, &clientRect);
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0)
        return;

    HDC windowDC = GetDC(tab.child);
    if (!windowDC)
        return;

    HDC memoryDC = CreateCompatibleDC(windowDC);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDC, width, height);
    if (memoryDC && bitmap) {
        HGDIOBJ previous = SelectObject(memoryDC, bitmap);
        RECT fill { 0, 0, width, height };
        FillRect(memoryDC, &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        PrintWindow(tab.child, memoryDC, PW_RENDERFULLCONTENT);
        SelectObject(memoryDC, previous);

        releaseSnapshot(tab);
        tab.snapshot = bitmap;
        tab.snapshotWidth = width;
        tab.snapshotHeight = height;
        bitmap = nullptr;
    }

    if (bitmap)
        DeleteObject(bitmap);
    if (memoryDC)
        DeleteDC(memoryDC);
    ReleaseDC(tab.child, windowDC);
}

// Puts a tab's kept frame on screen at the size the view is about to occupy.
void showSnapshot(Tab& tab)
{
    HWND parent = g_parentWindow.load();
    HWND window = snapshotWindow(parent);
    if (!window)
        return;

    const RECT rect = currentBoundsRect();
    g_snapshotBitmap = tab.snapshot;
    g_snapshotBitmapWidth = tab.snapshotWidth;
    g_snapshotBitmapHeight = tab.snapshotHeight;

    SetWindowPos(
        window,
        HWND_TOP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    g_snapshotDeadline = GetTickCount64() + kSnapshotHoldMs;
}

// Stops a tab rendering. Hidden alone is not enough: the view has to be told it
// is out of the window, or its process carries on as though it were visible.
void suspendTab(Tab& tab)
{
    if (!tab.child || tab.suspended)
        return;

    tab.suspended = true;
    ShowWindow(tab.child, SW_HIDE);
    if (g_api.viewSetIsInWindow && tab.view)
        g_api.viewSetIsInWindow(tab.view, false);
}

void resumeTab(Tab& tab)
{
    if (!tab.child)
        return;

    const RECT rect = currentBoundsRect();
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW;
    if (tab.width == width && tab.height == height)
        flags |= SWP_NOSIZE;
    else {
        tab.width = width;
        tab.height = height;
    }

    // Below the snapshot, which is holding this tab's last frame until the page
    // has rendered into the view underneath it.
    SetWindowPos(tab.child, HWND_TOP, rect.left, rect.top, width, height, flags);

    if (tab.suspended) {
        tab.suspended = false;
        if (g_api.viewSetIsInWindow && tab.view)
            g_api.viewSetIsInWindow(tab.view, true);
    }
}

// The switch itself is immediate and costs no rendering: the outgoing tab's
// frame is kept, the incoming tab's kept frame goes up straight away, and only
// then is the page behind it woken to render. The snapshot comes down when the
// page has, so the moment the view is blank is never on screen.
void selectTab(int id)
{
    Tab* target = findTab(id);
    if (!target)
        return;

    const int previousId = g_activeTabId;
    if (previousId == id) {
        resumeTab(*target);
        return;
    }

    Tab* previous = previousId ? findTab(previousId) : nullptr;
    if (previous)
        captureSnapshot(*previous);

    g_activeTabId = id;
    g_activeChild.store(target->child);

    showSnapshot(*target);
    resumeTab(*target);
    requestRepaint(*target);

    if (previous)
        suspendTab(*previous);

    publishSnapshot();
}

void createTab(int id, int slot, const std::string& url, bool select)
{
    HWND parent = g_parentWindow.load();
    if (!parent || !g_context)
        return;

    auto configuration = g_api.pageConfigurationCreate();
    if (!configuration) {
        setError("WKPageConfigurationCreate failed");
        return;
    }
    g_api.pageConfigurationSetContext(configuration, g_context);
    // No related page is set, deliberately: pages created as related to another
    // share its web process, and two tabs should not share one.

    const RECT rect = currentBoundsRect();
    auto view = g_api.viewCreate(rect, configuration, parent);
    release(configuration);
    if (!view) {
        setError("WKViewCreate failed");
        return;
    }

    Tab tab;
    tab.id = id;
    tab.slot = slot;
    tab.view = view;
    tab.child = g_api.viewGetWindow(view);
    tab.page = g_api.viewGetPage(view);
    if (!tab.child) {
        setError("WKViewGetWindow failed");
        release(view);
        return;
    }

    g_api.viewSetParentWindow(view, parent);
    g_api.viewSetIsInWindow(view, true);
    configurePage(tab.page, parent);
    subclassTabChild(tab.child);

    const RECT bounds = currentBoundsRect();
    tab.width = bounds.right - bounds.left;
    tab.height = bounds.bottom - bounds.top;
    SetWindowPos(
        tab.child,
        HWND_BOTTOM,
        bounds.left,
        bounds.top,
        tab.width,
        tab.height,
        SWP_NOACTIVATE | SWP_NOREDRAW | SWP_HIDEWINDOW
    );

    // A new tab starts suspended and is woken by being selected. Loading is not
    // rendering: the page still fetches and parses while it is out of the
    // window, it just does not draw.
    tab.suspended = true;
    g_api.viewSetIsInWindow(view, false);

    g_tabs.push_back(tab);
    if (!url.empty())
        loadInTab(tab, url);

    if (select || !g_activeTabId)
        selectTab(id);
    else
        publishSnapshot();
}

// Shows the tab that belongs to a host UI slot, opening it on first use. The
// url is the slot's home: it is loaded when the tab is created and ignored
// afterwards, so returning to a slot returns to where that tab had got to.
void activateSlot(int slot, const std::string& url)
{
    if (Tab* existing = findTabBySlot(slot)) {
        selectTab(existing->id);
        return;
    }
    createTab(g_nextTabId.fetch_add(1), slot, url, true);
}

void closeSlot(int slot)
{
    if (Tab* existing = findTabBySlot(slot))
        closeTab(existing->id);
}

void destroyTab(Tab& tab)
{
    releaseSnapshot(tab);

    // WebKit's Windows view closes its page from the child window's WM_DESTROY
    // path. Destroy the child first so the parent cannot later deliver a
    // WM_DESTROY to a freed object.
    if (tab.child && IsWindow(tab.child))
        DestroyWindow(tab.child);
    else if (tab.page)
        g_api.pageClose(tab.page);
    release(tab.view);
    tab.view = nullptr;
    tab.page = nullptr;
    tab.child = nullptr;
}

void closeTab(int id)
{
    const auto position = std::find_if(g_tabs.begin(), g_tabs.end(), [id](const Tab& tab) {
        return tab.id == id;
    });
    if (position == g_tabs.end())
        return;

    const bool wasActive = g_activeTabId == id;
    destroyTab(*position);
    g_tabs.erase(position);

    if (wasActive) {
        g_activeTabId = 0;
        g_activeChild.store(nullptr);
        if (!g_tabs.empty())
            selectTab(g_tabs.front().id);
    }
    publishSnapshot();
}

void destroyAllTabs()
{
    for (auto& tab : g_tabs)
        destroyTab(tab);
    g_tabs.clear();
    g_activeTabId = 0;
    g_activeChild.store(nullptr);
    hideSnapshot();
    publishSnapshot();
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

void runCommand(Command& command)
{
    switch (command.kind) {
    case Command::Kind::OpenTab:
        createTab(command.tabId, command.slot, command.url, false);
        break;
    case Command::Kind::ActivateSlot:
        activateSlot(command.slot, command.url);
        break;
    case Command::Kind::CloseTab:
        closeTab(command.tabId);
        break;
    case Command::Kind::CloseSlot:
        closeSlot(command.slot);
        break;
    case Command::Kind::SelectTab:
        selectTab(command.tabId);
        break;
    case Command::Kind::LoadURL:
        if (Tab* tab = activeTab())
            loadInTab(*tab, command.url);
        break;
    case Command::Kind::Back:
        if (Tab* tab = activeTab(); tab && tab->page)
            g_api.pageGoBack(tab->page);
        break;
    case Command::Kind::Forward:
        if (Tab* tab = activeTab(); tab && tab->page)
            g_api.pageGoForward(tab->page);
        break;
    case Command::Kind::Reload:
        if (Tab* tab = activeTab(); tab && tab->page)
            g_api.pageReload(tab->page);
        break;
    case Command::Kind::ForceRepaint:
        // Posted by the frame thread when the bounds changed. It has already
        // moved the active tab's window itself, so the size is recorded here
        // rather than re-applied, and the tabs behind it are put on the clock.
        if (Tab* tab = activeTab()) {
            const RECT rect = currentBoundsRect();
            tab->width = rect.right - rect.left;
            tab->height = rect.bottom - rect.top;
            requestRepaint(*tab);
        }
        // A snapshot still standing in for the page has to follow the window,
        // or the resize uncovers the very blank it was put up to hide.
        if (g_snapshotDeadline && g_snapshotWindow && IsWindow(g_snapshotWindow)) {
            const RECT rect = currentBoundsRect();
            SetWindowPos(
                g_snapshotWindow,
                HWND_TOP,
                rect.left,
                rect.top,
                rect.right - rect.left,
                rect.bottom - rect.top,
                SWP_NOACTIVATE
            );
        }
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
    const HRESULT comResult = OleInitialize(nullptr);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized && comResult != RPC_E_CHANGED_MODE) {
        setError("COM/OLE initialization failed");
        return 0;
    }

    if (!loadApi() || !createContext()) {
        if (comInitialized)
            OleUninitialize();
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

        g_api.runLoopCycle();

        HANDLE handles[1] = { g_commandEvent };
        MsgWaitForMultipleObjectsEx(1, handles, kWorkerWaitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    g_ready.store(false);
    destroyAllTabs();
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        DestroyWindow(g_snapshotWindow);
    g_snapshotWindow = nullptr;
    release(g_context);
    g_context = nullptr;

    if (comInitialized)
        OleUninitialize();
    return 0;
}

} // namespace

extern "C" int hb_webkit_start(void* parent_window, const char* initial_url)
{
    if (!parent_window) {
        setError("invalid WebKit parent window");
        return 0;
    }

    g_parentWindow.store(static_cast<HWND>(parent_window));

    std::lock_guard<std::mutex> lock(g_startMutex);
    if (g_thread)
        return g_ready.load() ? 1 : 0;

    if (!g_commandEvent) {
        g_commandEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_commandEvent) {
            setError("could not create the WebKit command event");
            return 0;
        }
    }

    g_shutdownRequested.store(false);
    g_thread = CreateThread(nullptr, 0, webKitThreadMain, nullptr, 0, nullptr);
    if (!g_thread) {
        setError("could not start the WebKit thread");
        return 0;
    }

    // Queued, not dropped: the thread drains whatever arrived while it started.
    // Slot zero is the host's first tab row, so the tab the browser opens with
    // is the same tab that row selects rather than a second one beside it.
    Command command;
    command.kind = Command::Kind::ActivateSlot;
    command.slot = 0;
    command.url = initial_url ? initial_url : "";
    postCommand(std::move(command));

    // Said once, on the call that creates the thread: the frame handler calls
    // this every frame, and a browser that reached its engine should say so
    // exactly as loudly as one that did not.
    std::fprintf(stderr, "harmony: webkit thread started for %s\n",
        initial_url ? initial_url : "(no url)");
    return g_ready.load() ? 1 : 0;
}

extern "C" int hb_webkit_supported(void)
{
    return g_ready.load() ? 1 : 0;
}

extern "C" void hb_webkit_set_bounds(int x, int y, int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    const bool changed = g_boundsX.load() != x
        || g_boundsY.load() != y
        || g_boundsWidth.load() != width
        || g_boundsHeight.load() != height;

    g_boundsX.store(x);
    g_boundsY.store(y);
    g_boundsWidth.store(width);
    g_boundsHeight.store(height);

    if (!changed)
        return;

    // Asynchronous so the frame thread never waits on the WebKit thread to
    // process the size, and without a redraw because the exposed strips have
    // nothing to paint until the page has rendered at the new size.
    HWND child = g_activeChild.load();
    if (child) {
        SetWindowPos(
            child,
            HWND_TOP,
            x,
            y,
            width,
            height,
            SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW | SWP_ASYNCWINDOWPOS
        );
    }

    postSimpleCommand(Command::Kind::ForceRepaint);
}

extern "C" int hb_webkit_tab_open(const char* url)
{
    const int id = g_nextTabId.fetch_add(1);
    if (id <= 0)
        return 0;

    Command command;
    command.kind = Command::Kind::OpenTab;
    command.tabId = id;
    command.url = url ? url : "";
    postCommand(std::move(command));
    return id;
}

extern "C" void hb_webkit_tab_activate_slot(int slot, const char* url)
{
    if (slot < 0)
        return;

    Command command;
    command.kind = Command::Kind::ActivateSlot;
    command.slot = slot;
    command.url = url ? url : "";
    postCommand(std::move(command));
}

extern "C" void hb_webkit_tab_close_slot(int slot)
{
    if (slot < 0)
        return;

    Command command;
    command.kind = Command::Kind::CloseSlot;
    command.slot = slot;
    postCommand(std::move(command));
}

extern "C" void hb_webkit_tab_close(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::CloseTab, tab_id);
}

extern "C" void hb_webkit_tab_select(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::SelectTab, tab_id);
}

extern "C" int hb_webkit_tab_count(void)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    return static_cast<int>(g_snapshotTabIds.size());
}

extern "C" int hb_webkit_tab_id_at(int index)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    if (index < 0 || index >= static_cast<int>(g_snapshotTabIds.size()))
        return 0;
    return g_snapshotTabIds[static_cast<size_t>(index)];
}

extern "C" int hb_webkit_tab_active(void)
{
    std::lock_guard<std::mutex> lock(g_snapshotMutex);
    return g_snapshotActiveTabId;
}

extern "C" void hb_webkit_go_back(void)
{
    postSimpleCommand(Command::Kind::Back);
}

extern "C" void hb_webkit_go_forward(void)
{
    postSimpleCommand(Command::Kind::Forward);
}

extern "C" void hb_webkit_reload(void)
{
    postSimpleCommand(Command::Kind::Reload);
}

extern "C" void hb_webkit_load_url(const char* url)
{
    Command command;
    command.kind = Command::Kind::LoadURL;
    command.url = url ? url : "";
    postCommand(std::move(command));
}

extern "C" void hb_webkit_shutdown(void)
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

extern "C" const char* hb_webkit_error(void)
{
    static thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        copy = g_error;
    }
    return copy.c_str();
}
