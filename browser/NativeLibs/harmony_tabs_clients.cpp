#include "harmony_tabs_internal.h"

#include "harmony_data_store.h"

#include <algorithm>

// What a tab's page is configured with, and what it tells the registry back.
//
// The two client structs below mirror WKPageUIClientV19 and WKPageStateClientV0
// field for field. WebKit copies a client struct by the size recorded for the
// version in its base, so the version and the struct have to agree exactly: a
// version that names a longer struct than the one handed over reads past it.
// Every member of both is pointer-sized, so mirroring them is a matter of the
// field COUNT, which the assertions below pin.

namespace harmony_tabs {

namespace {

// The version each client is installed at. Both are the latest their header in
// the checkout this browser's engine is built from declares, which is what puts
// every field a module writes inside the bytes WebKit copies.
constexpr int kUiClientVersion = 19;
constexpr int kStateClientVersion = 0;

static_assert(sizeof(hb_wk_page_ui_client_v19) == sizeof(hb_wk_page_ui_client_base) + 80 * sizeof(void*),
    "WKPageUIClientV19 carries 80 pointer-sized members after its base");

using WKPageStateClientCallback = void (*)(const void* clientInfo);

// Every WK client struct opens with the same two members.
struct WKClientBase {
    int version;
    const void* clientInfo;
};

struct WKPageStateClientV0 {
    WKClientBase base;

    WKPageStateClientCallback willChangeIsLoading;
    WKPageStateClientCallback didChangeIsLoading;
    WKPageStateClientCallback willChangeTitle;
    WKPageStateClientCallback didChangeTitle;
    WKPageStateClientCallback willChangeActiveURL;
    WKPageStateClientCallback didChangeActiveURL;
    WKPageStateClientCallback willChangeHasOnlySecureContent;
    WKPageStateClientCallback didChangeHasOnlySecureContent;
    WKPageStateClientCallback willChangeEstimatedProgress;
    WKPageStateClientCallback didChangeEstimatedProgress;
    WKPageStateClientCallback willChangeCanGoBack;
    WKPageStateClientCallback didChangeCanGoBack;
    WKPageStateClientCallback willChangeCanGoForward;
    WKPageStateClientCallback didChangeCanGoForward;
    WKPageStateClientCallback willChangeNetworkRequestsInProgress;
    WKPageStateClientCallback didChangeNetworkRequestsInProgress;
    WKPageStateClientCallback willChangeCertificateInfo;
    WKPageStateClientCallback didChangeCertificateInfo;
    WKPageStateClientCallback willChangeWebProcessIsResponsive;
    WKPageStateClientCallback didChangeWebProcessIsResponsive;
    WKPageStateClientCallback didSwapWebProcesses;
};

static_assert(sizeof(WKPageStateClientV0) == sizeof(WKClientBase) + 21 * sizeof(void*),
    "WKPageStateClientV0 carries 21 pointer-sized members after its base");
static_assert(sizeof(hb_wk_page_ui_client_base) == sizeof(WKClientBase),
    "the published UI-client base must match the WK client base");

// The host a tab is labelled and badged by. The `www.` a site answers on is
// not part of what the site IS, and a strip of tabs reading "www.github.com"
// spends its width saying so.
std::string copyWKHost(WKURLRef url)
{
    if (!url || !g_api.urlCopyHostName)
        return { };

    WKStringRef host = g_api.urlCopyHostName(url);
    std::string out = copyWKString(host);
    release(host);

    if (out.rfind("www.", 0) == 0)
        out.erase(0, 4);
    return out;
}

// Any of the page's load-state changes: the whole record is read back rather
// than each callback carrying the one field it is named after, because the C
// state client reports THAT something changed and never what it changed to.
//
// The field the callback was named after is still forwarded to the observers,
// which is what lets one of them republish a single number for progress instead
// of a whole page's worth of strings sixty times a second.
void tabStateDidChange(const void* clientInfo, int field)
{
    const int id = tabIdFromClientInfo(clientInfo);
    Tab* tab = findTab(id);
    if (!tab)
        return;
    refreshTabState(*tab);
    publishTabs();
    notifyPageState(id, tab->page, field);
}

void tabStateWillChange(const void*)
{
}

void tabDidChangeIsLoading(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_LOADING);
}

void tabDidChangeTitle(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_TITLE);
}

void tabDidChangeActiveURL(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_URL);
}

void tabDidChangeEstimatedProgress(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_PROGRESS);
}

void tabDidChangeCanGoBack(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_CAN_GO_BACK);
}

void tabDidChangeCanGoForward(const void* clientInfo)
{
    tabStateDidChange(clientInfo, HB_TABS_PAGE_STATE_CAN_GO_FORWARD);
}

// window.close(). Queued rather than acted on here: WebKit is inside this
// page's own callback, and the tab's page cannot be destroyed under it.
void pageDidClose(WKPageRef, const void* clientInfo)
{
    postSimpleCommand(Command::Kind::Close, tabIdFromClientInfo(clientInfo));
}

// WebKit denies an unanswered camera or notification request, but drops an
// unanswered geolocation one and leaves the page's promise pending forever. A
// browser that has not asked the user yet still has to answer.
void denyGeolocation(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKGeolocationPermissionRequestRef request, const void*)
{
    if (g_api.geolocationPermissionRequestDeny)
        g_api.geolocationPermissionRequestDeny(request);
}

using WKPageCreateNewPageCallback = WKPageRef (*)(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef, WKWindowFeaturesRef, const void*);
using WKPageUIClientCallback = void (*)(WKPageRef, const void*);
using WKPageDecidePolicyForGeolocationPermissionRequestCallback = void (*)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKGeolocationPermissionRequestRef, const void*);

// target=_blank, window.open, and anything else that asks for a second page.
// The returned page carries a reference for the caller, which is why it is
// retained on the way out: WebKit adopts it rather than retaining it itself.
WKPageRef createNewPageForTab(WKPageRef, WKPageConfigurationRef configuration, WKNavigationActionRef, WKWindowFeaturesRef, const void*)
{
    // A page that opens a window opens it beside the tab list's end, in the
    // foreground, which is what every browser does with a link that asks for a
    // new window and what makes the click visibly do something.
    //
    // The id is reserved BEFORE the configuration is settled, because the data
    // store has to be told which tab this configuration belongs to: window.open
    // from a private tab carries the ephemeral store WebKit already put on the
    // configuration, and the profile leaves it there when it finds it.
    const int id = reserveTabId();
    hb_data_store_apply_to_page_configuration(const_cast<void*>(configuration), id);
    Tab* tab = createTab(configuration, id, -1, false, true);
    if (!tab || !tab->page)
        return nullptr;

    refreshTabState(*tab);
    publishTabs();
    return static_cast<WKPageRef>(retain(tab->page));
}

} // namespace

// --- Page configuration -----------------------------------------------------

double hostBackingScale()
{
    const double hostScale = g_backingScale.load();
    if (hostScale > 0.0)
        return std::clamp(hostScale, 0.5, 8.0);

    // Before the host has said, the window it gave us knows.
    HWND window = g_parentWindow.load();
    UINT dpi = window ? GetDpiForWindow(window) : 0;
    if (!dpi)
        dpi = GetDpiForSystem();
    if (!dpi)
        return 1.0;
    return std::clamp(static_cast<double>(dpi) / 96.0, 0.5, 8.0);
}

void configurePage(Tab& tab)
{
    if (!tab.page)
        return;

    if (g_api.stringCreateWithUTF8CString && g_api.pageSetCustomUserAgent) {
        WKStringRef userAgent = g_api.stringCreateWithUTF8CString(desktopUserAgent());
        if (userAgent) {
            g_api.pageSetCustomUserAgent(tab.page, userAgent);
            release(userAgent);
        }
    }

    // The page is laid out at the SAME scale the chrome around it is drawn at.
    // Rounding one of the two and not the other is what makes a page read a
    // third too large beside its own toolbar on a 150% display.
    if (g_api.pageSetCustomBackingScaleFactor)
        g_api.pageSetCustomBackingScaleFactor(tab.page, hostBackingScale());
}

void applyBackingScaleToTabs()
{
    if (!g_api.pageSetCustomBackingScaleFactor)
        return;

    const double scale = hostBackingScale();
    for (auto& tab : g_tabs) {
        if (tab->page)
            g_api.pageSetCustomBackingScaleFactor(tab->page, scale);
    }
}

// --- Load state -------------------------------------------------------------

void refreshTabState(Tab& tab)
{
    if (!tab.page)
        return;

    if (g_api.pageCopyTitle) {
        WKStringRef title = g_api.pageCopyTitle(tab.page);
        tab.title = copyWKString(title);
        release(title);
    }

    if (g_api.pageCopyActiveURL) {
        WKURLRef active = g_api.pageCopyActiveURL(tab.page);
        tab.url = copyWKURL(active);
        tab.host = copyWKHost(active);
        release(active);
    }

    bool requestPending = false;
    if (g_api.pageCopyPendingAPIRequestURL) {
        WKURLRef pending = g_api.pageCopyPendingAPIRequestURL(tab.page);
        requestPending = pending != nullptr;
        // A load that has been asked for but not committed is the address the
        // tab is going to, and is the one worth showing while it gets there.
        if (pending && tab.url.empty()) {
            tab.url = copyWKURL(pending);
            tab.host = copyWKHost(pending);
        }
        release(pending);
    }

    tab.progress = g_api.pageGetEstimatedProgress ? g_api.pageGetEstimatedProgress(tab.page) : 0.0;
    tab.canGoBack = g_api.pageCanGoBack && g_api.pageCanGoBack(tab.page);
    tab.canGoForward = g_api.pageCanGoForward && g_api.pageCanGoForward(tab.page);

    // The C API reports that the loading flag changed and never what it changed
    // to, so the flag is read back off the two things that do have accessors: a
    // load asked for and not yet committed, or one running between the value
    // WebKit starts progress at and the 1 it finishes it at.
    tab.loading = requestPending || (tab.progress > 0.0 && tab.progress < 1.0);

    if (tab.title.empty())
        tab.title = tab.host.empty() ? std::string("New Tab") : tab.host;
}

// --- Clients ----------------------------------------------------------------

void installPageClients(Tab& tab)
{
    if (!tab.page)
        return;

    hb_wk_page_ui_client_v19 uiClient { };
    uiClient.base.version = kUiClientVersion;
    uiClient.base.clientInfo = clientInfoForTab(tab.id);
    uiClient.close = reinterpret_cast<void*>(static_cast<WKPageUIClientCallback>(pageDidClose));
    uiClient.createNewPage = reinterpret_cast<void*>(static_cast<WKPageCreateNewPageCallback>(createNewPageForTab));
    if (g_api.geolocationPermissionRequestDeny) {
        uiClient.decidePolicyForGeolocationPermissionRequest =
            reinterpret_cast<void*>(static_cast<WKPageDecidePolicyForGeolocationPermissionRequestCallback>(denyGeolocation));
    }

    {
        std::vector<UiClientHook> hooks;
        {
            std::lock_guard<std::mutex> lock(g_hookMutex);
            hooks = g_uiClientHooks;
        }
        for (const auto& entry : hooks) {
            if (entry.hook)
                entry.hook(tab.id, &uiClient, entry.userData);
        }
    }

    if (g_api.pageSetPageUIClient)
        g_api.pageSetPageUIClient(tab.page, &uiClient);

    WKPageStateClientV0 stateClient { };
    stateClient.base.version = kStateClientVersion;
    stateClient.base.clientInfo = clientInfoForTab(tab.id);
    stateClient.willChangeIsLoading = tabStateWillChange;
    stateClient.didChangeIsLoading = tabDidChangeIsLoading;
    stateClient.willChangeTitle = tabStateWillChange;
    stateClient.didChangeTitle = tabDidChangeTitle;
    stateClient.willChangeActiveURL = tabStateWillChange;
    stateClient.didChangeActiveURL = tabDidChangeActiveURL;
    stateClient.willChangeEstimatedProgress = tabStateWillChange;
    stateClient.didChangeEstimatedProgress = tabDidChangeEstimatedProgress;
    stateClient.willChangeCanGoBack = tabStateWillChange;
    stateClient.didChangeCanGoBack = tabDidChangeCanGoBack;
    stateClient.willChangeCanGoForward = tabStateWillChange;
    stateClient.didChangeCanGoForward = tabDidChangeCanGoForward;

    if (g_api.pageSetPageStateClient)
        g_api.pageSetPageStateClient(tab.page, &stateClient);

    // The navigation client is the registry's too, for the same reason the UI
    // client is: its two policy callbacks have to be answered, and a page whose
    // policy listener nobody answers never loads anything at all.
    installNavigationClient(tab);

    // Last, so a module that installs a find client of its own finds the page
    // fully configured and cannot be overwritten by this.
    std::vector<PageObserver> observers;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        observers = g_pageObservers;
    }
    for (const auto& observer : observers) {
        if (observer.created)
            observer.created(tab.id, const_cast<void*>(tab.page), observer.userData);
    }
}

void notifyPageDestroying(const Tab& tab)
{
    if (!tab.page)
        return;

    std::vector<PageObserver> observers;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        observers = g_pageObservers;
    }
    for (const auto& observer : observers) {
        if (observer.destroying)
            observer.destroying(tab.id, const_cast<void*>(tab.page), observer.userData);
    }
}

} // namespace harmony_tabs
