#include "harmony_tabs_internal.h"

// The seam the rest of the browser's native code reaches WebKit through.
//
// Three things live here, and all three exist because WebKit gives a page ONE
// of something the browser has several owners for:
//
//   - the navigation client, whose two policy callbacks must be answered and
//     therefore belong to the registry rather than to whichever module
//     installed its client last;
//   - the download seam, which is those two answers plus the three callbacks
//     that hand a WKDownloadRef over;
//   - the WebKit thread's own schedule, which a module needs a place in when it
//     has work that can only run against a WebKit object.

namespace harmony_tabs {

namespace {

static_assert(
    sizeof(hb_wk_page_navigation_client_v3)
        == sizeof(hb_wk_page_navigation_client_base) + 27 * sizeof(void*),
    "WKPageNavigationClientV3 carries 27 pointer-sized members after its base"
);

using WKPageNavigationDecidePolicyForNavigationActionCallback =
    void (*)(WKPageRef, WKNavigationActionRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
using WKPageNavigationDecidePolicyForNavigationResponseCallback =
    void (*)(WKPageRef, WKNavigationResponseRef, WKFramePolicyListenerRef, WKTypeRef, const void*);
using WKPageNavigationActionDidBecomeDownloadCallback =
    void (*)(WKPageRef, WKNavigationActionRef, WKDownloadRef, const void*);
using WKPageNavigationResponseDidBecomeDownloadCallback =
    void (*)(WKPageRef, WKNavigationResponseRef, WKDownloadRef, const void*);
using WKPageNavigationContextMenuDidCreateDownloadCallback =
    void (*)(WKPageRef, WKDownloadRef, const void*);

std::vector<DownloadClient> downloadClients()
{
    std::lock_guard<std::mutex> lock(g_hookMutex);
    return g_downloadClients;
}

// Whether any module claims this navigation as a file to be saved. Asked at the
// moment of the decision rather than baked into the page's client, so a module
// that attached after the tab was created still gets to claim its downloads.
bool claimedAsDownload(int tabId, const void* subject, bool response)
{
    for (const DownloadClient& client : downloadClients()) {
        hb_tabs_download_policy_hook hook = response
            ? client.shouldDownloadResponse
            : client.shouldDownloadAction;
        if (hook && hook(tabId, subject, client.userData))
            return true;
    }
    return false;
}

void deliverDownload(int tabId, WKDownloadRef download)
{
    if (!download)
        return;
    for (const DownloadClient& client : downloadClients()) {
        if (client.didBecomeDownload)
            client.didBecomeDownload(tabId, download, client.userData);
    }
}

// Every load's first decision. Answering is not optional: a listener left
// unanswered is a load that neither starts nor fails, so both paths below end
// in a call.
void decidePolicyForNavigationAction(
    WKPageRef,
    WKNavigationActionRef action,
    WKFramePolicyListenerRef listener,
    WKTypeRef,
    const void* clientInfo
) {
    if (claimedAsDownload(tabIdFromClientInfo(clientInfo), action, false)) {
        g_api.framePolicyListenerDownload(listener);
        return;
    }
    g_api.framePolicyListenerUse(listener);
}

// The second decision, once the server has answered and the type of what it
// sent is known.
void decidePolicyForNavigationResponse(
    WKPageRef,
    WKNavigationResponseRef response,
    WKFramePolicyListenerRef listener,
    WKTypeRef,
    const void* clientInfo
) {
    if (claimedAsDownload(tabIdFromClientInfo(clientInfo), response, true)) {
        g_api.framePolicyListenerDownload(listener);
        return;
    }
    g_api.framePolicyListenerUse(listener);
}

void navigationActionDidBecomeDownload(
    WKPageRef,
    WKNavigationActionRef,
    WKDownloadRef download,
    const void* clientInfo
) {
    deliverDownload(tabIdFromClientInfo(clientInfo), download);
}

void navigationResponseDidBecomeDownload(
    WKPageRef,
    WKNavigationResponseRef,
    WKDownloadRef download,
    const void* clientInfo
) {
    deliverDownload(tabIdFromClientInfo(clientInfo), download);
}

// "Save link as" from WebKit's own context menu, which creates the download
// without a navigation to decide about first.
void contextMenuDidCreateDownload(WKPageRef, WKDownloadRef download, const void* clientInfo)
{
    deliverDownload(tabIdFromClientInfo(clientInfo), download);
}

} // namespace

// --- The page's navigation client -------------------------------------------

void installNavigationClient(Tab& tab)
{
    if (!tab.page || !g_api.pageSetPageNavigationClient)
        return;

    hb_wk_page_navigation_client_v3 client { };
    client.base.version = 3;
    client.base.clientInfo = clientInfoForTab(tab.id);
    client.decidePolicyForNavigationAction = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDecidePolicyForNavigationActionCallback>(decidePolicyForNavigationAction)
    );
    client.decidePolicyForNavigationResponse = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDecidePolicyForNavigationResponseCallback>(decidePolicyForNavigationResponse)
    );
    client.navigationActionDidBecomeDownload = reinterpret_cast<void*>(
        static_cast<WKPageNavigationActionDidBecomeDownloadCallback>(navigationActionDidBecomeDownload)
    );
    client.navigationResponseDidBecomeDownload = reinterpret_cast<void*>(
        static_cast<WKPageNavigationResponseDidBecomeDownloadCallback>(navigationResponseDidBecomeDownload)
    );
    client.contextMenuDidCreateDownload = reinterpret_cast<void*>(
        static_cast<WKPageNavigationContextMenuDidCreateDownloadCallback>(contextMenuDidCreateDownload)
    );

    std::vector<NavigationClientHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        hooks = g_navigationClientHooks;
    }
    for (const NavigationClientHook& entry : hooks) {
        if (entry.hook)
            entry.hook(tab.id, &client, entry.userData);
    }

    // WebKit copies the struct by the size its version names, so the stack copy
    // is all it ever needs.
    g_api.pageSetPageNavigationClient(tab.page, &client);
}

// --- The page's load state --------------------------------------------------

void notifyPageState(int tabId, WKPageRef page, int field)
{
    std::vector<PageStateObserver> observers;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_pageStateObservers.empty())
            return;
        observers = g_pageStateObservers;
    }
    for (const PageStateObserver& entry : observers) {
        if (entry.hook)
            entry.hook(tabId, const_cast<void*>(page), field, entry.userData);
    }
}

// --- The WebKit thread's schedule -------------------------------------------

void runCycleHooks()
{
    std::vector<CycleHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        if (g_cycleHooks.empty())
            return;
        hooks = g_cycleHooks;
    }
    for (const CycleHook& entry : hooks) {
        if (entry.hook)
            entry.hook(entry.userData);
    }
}

void runTeardownHooks()
{
    std::vector<TeardownHook> hooks;
    {
        std::lock_guard<std::mutex> lock(g_hookMutex);
        hooks = g_teardownHooks;
    }
    for (const TeardownHook& entry : hooks) {
        if (entry.hook)
            entry.hook(entry.userData);
    }
}

} // namespace harmony_tabs

// --- Registration -----------------------------------------------------------
//
// Safe from any thread and at any time. Each list is read under the same lock
// wherever it is used, and none of them is consulted with a WebKit object in
// hand that the registration could invalidate.

using namespace harmony_tabs;

extern "C" void hb_tabs_add_navigation_client_hook(hb_tabs_navigation_client_hook hook, void* user_data)
{
    if (!hook)
        return;

    NavigationClientHook entry;
    entry.hook = hook;
    entry.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_navigationClientHooks.push_back(entry);
}

extern "C" void hb_tabs_add_page_state_observer(hb_tabs_page_state_hook hook, void* user_data)
{
    if (!hook)
        return;

    PageStateObserver entry;
    entry.hook = hook;
    entry.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_pageStateObservers.push_back(entry);
}

extern "C" void hb_tabs_add_download_client(
    hb_tabs_download_policy_hook should_download_action,
    hb_tabs_download_policy_hook should_download_response,
    hb_tabs_download_hook did_become_download,
    void* user_data
) {
    if (!should_download_action && !should_download_response && !did_become_download)
        return;

    DownloadClient client;
    client.shouldDownloadAction = should_download_action;
    client.shouldDownloadResponse = should_download_response;
    client.didBecomeDownload = did_become_download;
    client.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_downloadClients.push_back(client);
}

extern "C" void hb_tabs_add_cycle_hook(hb_tabs_cycle_hook hook, void* user_data)
{
    if (!hook)
        return;

    CycleHook entry;
    entry.hook = hook;
    entry.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_cycleHooks.push_back(entry);
}

extern "C" void hb_tabs_add_teardown_hook(hb_tabs_teardown_hook hook, void* user_data)
{
    if (!hook)
        return;

    TeardownHook entry;
    entry.hook = hook;
    entry.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_teardownHooks.push_back(entry);
}
