#ifndef HARMONY_TABS_INTERNAL_H
#define HARMONY_TABS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "harmony_paths.h"
#include "harmony_tabs_embed.h"

// The registry's own state, shared between its translation units. Everything
// declared under "WebKit thread" is touched only by that thread and needs no
// lock; everything under "shared" is reached from the host's frame thread too
// and states how it is guarded.

namespace harmony_tabs {

// Every WK object is an opaque pointer in C, so one alias serves them all. The
// declarations below mirror the WebKit C API by hand rather than including its
// headers: the engine is resolved at run time out of WebKit2.dll, and a build
// of this browser must not need a WebKit checkout beside it to compile.
using WKTypeRef = const void*;
using WKContextConfigurationRef = const void*;
using WKContextRef = const void*;
using WKPageConfigurationRef = const void*;
using WKPageRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKViewRef = const void*;
using WKNavigationActionRef = const void*;
using WKWindowFeaturesRef = const void*;
using WKGeolocationPermissionRequestRef = const void*;
using WKFrameRef = const void*;
using WKSecurityOriginRef = const void*;
using WKNavigationResponseRef = const void*;
using WKFramePolicyListenerRef = const void*;
using WKDownloadRef = const void*;
using WKPageStateClientBase = void;

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
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CString)(WKStringRef, char*, size_t) { nullptr };
    size_t (*stringGetUTF8CStringNonStrict)(WKStringRef, char*, size_t) { nullptr };
    WKURLRef (*urlCreateWithUTF8CString)(const char*) { nullptr };
    WKStringRef (*urlCopyString)(WKURLRef) { nullptr };
    WKStringRef (*urlCopyHostName)(WKURLRef) { nullptr };
    void (*pageSetCustomUserAgent)(WKPageRef, WKStringRef) { nullptr };
    void (*pageSetCustomBackingScaleFactor)(WKPageRef, double) { nullptr };
    void (*pageSetPageUIClient)(WKPageRef, const void*) { nullptr };
    void (*pageSetPageStateClient)(WKPageRef, WKPageStateClientBase*) { nullptr };
    void (*pageSetPageNavigationClient)(WKPageRef, const void*) { nullptr };
    void (*framePolicyListenerUse)(WKFramePolicyListenerRef) { nullptr };
    void (*framePolicyListenerDownload)(WKFramePolicyListenerRef) { nullptr };
    WKStringRef (*pageCopyTitle)(WKPageRef) { nullptr };
    WKURLRef (*pageCopyActiveURL)(WKPageRef) { nullptr };
    WKURLRef (*pageCopyPendingAPIRequestURL)(WKPageRef) { nullptr };
    double (*pageGetEstimatedProgress)(WKPageRef) { nullptr };
    bool (*pageCanGoBack)(WKPageRef) { nullptr };
    bool (*pageCanGoForward)(WKPageRef) { nullptr };
    void (*pageForceRepaint)(WKPageRef, void*, void (*)(WKTypeRef, void*)) { nullptr };
    void (*pageLoadURL)(WKPageRef, WKURLRef) { nullptr };
    void (*pageGoBack)(WKPageRef) { nullptr };
    void (*pageGoForward)(WKPageRef) { nullptr };
    void (*pageReload)(WKPageRef) { nullptr };
    void (*pageStopLoading)(WKPageRef) { nullptr };
    void (*pageClose)(WKPageRef) { nullptr };
    void (*geolocationPermissionRequestDeny)(WKGeolocationPermissionRequestRef) { nullptr };
    WKTypeRef (*retain)(WKTypeRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };
};

// One open tab. Everything in it belongs to the WebKit thread.
struct Tab {
    int id { 0 };
    bool pinned { false };

    WKViewRef view { nullptr };
    WKPageRef page { nullptr };
    HWND child { nullptr };

    int width { 0 };
    int height { 0 };
    bool suspended { false };
    bool repaintInFlight { false };

    // The last frame this tab had on screen, kept so switching back to it can
    // show something immediately. A tab that has never been shown has none.
    HBITMAP snapshot { nullptr };
    int snapshotWidth { 0 };
    int snapshotHeight { 0 };

    // What the host draws. Refreshed from the page's load state.
    std::string title;
    std::string url;
    std::string host;
    bool loading { false };
    bool canGoBack { false };
    bool canGoForward { false };
    double progress { 0.0 };
};

// A tab the user closed, kept so the close can be undone.
struct ClosedTab {
    std::string url;
    std::string title;
    int index { 0 };
    bool pinned { false };
};

// The host's copy of a tab. Read under g_publishedMutex.
struct PublishedTab {
    int id { 0 };
    std::string title;
    std::string url;
    std::string host;
    bool loading { false };
    bool pinned { false };
    bool canGoBack { false };
    bool canGoForward { false };
    double progress { 0.0 };
};

struct Command {
    enum class Kind {
        Open,
        Close,
        CloseOthers,
        CloseAll,
        ReopenClosed,
        Select,
        SelectIndex,
        SelectNext,
        SelectPrevious,
        Move,
        SetPinned,
        Load,
        Back,
        Forward,
        Reload,
        Stop,
        ApplyBounds,
        ApplyBackingScale,
        Invoke,
        Shutdown,
    };

    Kind kind { Kind::ApplyBounds };
    int tabId { 0 };
    int index { -1 };
    bool flag { false };
    std::string url;
    void (*invoke)(void*) { nullptr };
    void* invokeContext { nullptr };
};

struct PageObserver {
    hb_tabs_page_hook created { nullptr };
    hb_tabs_page_hook destroying { nullptr };
    void* userData { nullptr };
};

struct UiClientHook {
    hb_tabs_ui_client_hook hook { nullptr };
    void* userData { nullptr };
};

struct PageStateObserver {
    hb_tabs_page_state_hook hook { nullptr };
    void* userData { nullptr };
};

struct NavigationClientHook {
    hb_tabs_navigation_client_hook hook { nullptr };
    void* userData { nullptr };
};

struct DownloadClient {
    hb_tabs_download_policy_hook shouldDownloadAction { nullptr };
    hb_tabs_download_policy_hook shouldDownloadResponse { nullptr };
    hb_tabs_download_hook didBecomeDownload { nullptr };
    void* userData { nullptr };
};

struct CycleHook {
    hb_tabs_cycle_hook hook { nullptr };
    void* userData { nullptr };
};

struct TeardownHook {
    hb_tabs_teardown_hook hook { nullptr };
    void* userData { nullptr };
};

// A tab id crosses into a WK client as its `clientInfo`, which is how every
// callback below finds the tab it was called for. Both directions are here so
// the widening is written once.
inline const void* clientInfoForTab(int id)
{
    return reinterpret_cast<const void*>(static_cast<intptr_t>(id));
}

inline int tabIdFromClientInfo(const void* clientInfo)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(clientInfo));
}

// --- WebKit thread ----------------------------------------------------------

extern WebKitApi g_api;
extern WKContextRef g_context;
// Held by pointer so a tab's address survives both the vector growing and the
// list being reordered: a WebKit callback can create a tab from inside a call
// made on another one, and the caller must not be left holding a moved-from
// element.
extern std::vector<std::unique_ptr<Tab>> g_tabs;
extern std::vector<ClosedTab> g_closedTabs;
extern int g_activeTabId;

extern HWND g_snapshotWindow;
extern HBITMAP g_snapshotBitmap;
extern int g_snapshotBitmapWidth;
extern int g_snapshotBitmapHeight;
extern ULONGLONG g_snapshotDeadline;

// --- Shared -----------------------------------------------------------------

extern std::mutex g_commandMutex;
extern std::deque<Command> g_commands;
extern HANDLE g_commandEvent;

extern std::mutex g_publishedMutex;
extern std::vector<PublishedTab> g_published;
extern int g_publishedActiveId;
extern int g_publishedClosedCount;
extern std::atomic<int> g_revision;

extern std::mutex g_hookMutex;
extern std::vector<PageObserver> g_pageObservers;
extern std::vector<PageStateObserver> g_pageStateObservers;
extern std::vector<UiClientHook> g_uiClientHooks;
extern std::vector<NavigationClientHook> g_navigationClientHooks;
extern std::vector<DownloadClient> g_downloadClients;
extern std::vector<CycleHook> g_cycleHooks;
extern std::vector<TeardownHook> g_teardownHooks;

extern std::mutex g_homeMutex;
extern std::string g_home;

extern std::atomic<bool> g_ready;
extern std::atomic<bool> g_shutdownRequested;
extern std::atomic<HWND> g_parentWindow;
extern std::atomic<HWND> g_activeChild;
extern std::atomic<int> g_boundsX;
extern std::atomic<int> g_boundsY;
extern std::atomic<int> g_boundsWidth;
extern std::atomic<int> g_boundsHeight;
extern std::atomic<double> g_backingScale;
extern std::atomic<int> g_nextTabId;
extern std::atomic<DWORD> g_webKitThreadId;

// How long a stale frame may stand in for a page that has not rendered yet.
constexpr ULONGLONG kSnapshotHoldMs = 400;

// How long the WebKit thread sleeps when it has nothing to do. It is a ceiling
// rather than a period: a posted command wakes it immediately.
constexpr DWORD kWorkerWaitMs = 8;

// How many closed tabs stay reopenable.
constexpr size_t kClosedTabHistory = 24;

// --- engine.cpp -------------------------------------------------------------

void setError(const char* message);
void clearError();
std::string currentError();

bool loadApi();
bool createContext();

void postCommand(Command&& command);
void postSimpleCommand(Command::Kind kind, int tabId = 0);
bool startWebKitThread();
void stopWebKitThread();

std::string normalizedURL(const char* input);
std::string homeURL();
const char* desktopUserAgent();

void release(WKTypeRef object);
WKTypeRef retain(WKTypeRef object);
std::string copyWKString(WKStringRef string);
std::string copyWKURL(WKURLRef url);

// --- view.cpp ---------------------------------------------------------------

RECT currentBoundsRect();
void subclassTabChild(HWND child);
HWND snapshotWindow(HWND parent);
void hideSnapshot();
void moveSnapshotToBounds();
void releaseSnapshot(Tab& tab);
void captureSnapshot(Tab& tab);
void showSnapshot(Tab& tab);
void suspendTab(Tab& tab);
void resumeTab(Tab& tab);
void requestRepaint(Tab& tab);
void repaintCompleted(WKTypeRef result, void* context);
void destroySnapshotWindow();

// --- model.cpp --------------------------------------------------------------

Tab* findTab(int id);
Tab* activeTab();
Tab* findTabByPage(WKPageRef page);
int indexOfTab(int id);
size_t pinnedCount();

void publishTabs();

// Creates a view on `configuration` and registers it as a tab under `id`. The
// caller keeps ownership of the configuration. The id is chosen by whoever asks
// for the tab rather than here, so a host that queued the open across threads
// already holds the id the tab will answer to.
Tab* createTab(WKPageConfigurationRef configuration, int id, int index, bool pinned, bool foreground);

// The id a caller reserves for a tab it is about to ask for.
int reserveTabId();

int openTab(int id, const std::string& url, int index, bool foreground);
void closeTab(int id);
void closeOtherTabs(int id);
void closeAllTabs();
int reopenClosedTab(int id);
void selectTab(int id);
void selectTabAtIndex(int index);
void selectAdjacentTab(int delta);
void moveTab(int id, int toIndex);
void setTabPinned(int id, bool pinned);
void loadInTab(int id, const std::string& url);
void loadInTabDirect(Tab& tab, const std::string& url);

// --- clients.cpp ------------------------------------------------------------

double hostBackingScale();
void applyBackingScaleToTabs();
void configurePage(Tab& tab);
void refreshTabState(Tab& tab);
void installPageClients(Tab& tab);
void notifyPageDestroying(const Tab& tab);

// --- embed.cpp --------------------------------------------------------------

void installNavigationClient(Tab& tab);
void notifyPageState(int tabId, WKPageRef page, int field);
void runCycleHooks();
void runTeardownHooks();

} // namespace harmony_tabs

#endif
