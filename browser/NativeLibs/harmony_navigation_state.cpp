#include "harmony_navigation_state.h"

#include "harmony_navigation_host.h"
#include "harmony_navigation_model.h"
#include "harmony_navigation_webkit.h"

#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The WebKit half of the navigation model.
//
// The tabs registry owns the engine, the thread and every client a page carries,
// so nothing here installs one: this model registers with that registry, writes
// its own fields into the client the registry stamps, and is handed each page,
// each state report and one turn per run-loop cycle. Everything below runs on
// the WebKit thread except the command queue, which says so.

namespace harmony_navigation {
namespace {

// The error code the model reports when a page's web process ended under it.
// WebKit's own codes come from a navigation failing, and this did not.
constexpr int kWebProcessEndedCode = -1;

// One tab, as the WebKit thread knows it. Held behind a `unique_ptr` because a
// page created from inside a callback made on another tab must not move the
// record that callback is holding.
struct TabRecord {
    int tabId { 0 };
    int slot { -1 };
    WKPageRef page { nullptr };

    // Whether a navigation is in flight. WebKit has no C accessor for this, so
    // it is kept from the navigation callbacks that bracket a load.
    bool loading { false };

    int errorCode { 0 };
    std::string errorText;
};

// --- Owned by the WebKit thread ----------------------------------------------

std::vector<std::unique_ptr<TabRecord>> g_records;
int g_activeTabId { 0 };

// What the registry's list looked like when the slots were last read off it.
int g_tabsRevision { -1 };

// --- Shared -------------------------------------------------------------------

struct Command {
    enum class Kind {
        Navigate,
        Reload,
        ReloadIgnoringCache,
        Stop,
        Back,
        Forward,
        GoToHistoryEntry,
    };

    Kind kind { Kind::Reload };
    int tabId { 0 };
    int index { 0 };
    std::string url;
};

std::mutex g_commandMutex;
std::deque<Command> g_commands;

std::atomic<bool> g_attached { false };

// --- Records ------------------------------------------------------------------

TabRecord* findRecord(int tabId)
{
    const int wanted = tabId > 0 ? tabId : g_activeTabId;
    if (!wanted)
        return nullptr;

    for (const auto& record : g_records) {
        if (record->tabId == wanted)
            return record.get();
    }
    return nullptr;
}

// The tab a shared client callback was called for.
//
// Every client on a page is the tab registry's, and the registry states the tab
// by widening its id into `clientInfo`. So this model finds its record by that
// id rather than by an address of its own: a client whose clientInfo is a
// pointer into one module is a client no other module can share.
TabRecord* recordOf(const void* clientInfo)
{
    const int tabId = static_cast<int>(reinterpret_cast<intptr_t>(clientInfo));
    if (tabId <= 0)
        return nullptr;

    for (const auto& record : g_records) {
        if (record->tabId == tabId)
            return record.get();
    }
    return nullptr;
}

// --- Publishing ---------------------------------------------------------------

// The back/forward list as one flat list, oldest first.
//
// An entry is written for every offset the list reports, including one WebKit
// answers nothing for, so an entry's index in this list is always its offset
// from the current entry plus the number of entries behind it. That is the
// mapping `hb_nav_go_to_history_entry` inverts.
void readHistory(const WebKitApi& api, WKPageRef page, PageSnapshot& snapshot)
{
    WKBackForwardListRef list = api.pageGetBackForwardList(page);
    if (!list)
        return;

    // A page with nothing committed has no current entry, and so no history to
    // count offsets from.
    if (!api.backForwardListGetItemAtIndex(list, 0))
        return;

    const int back = static_cast<int>(api.backForwardListGetBackListCount(list));
    const int forward = static_cast<int>(api.backForwardListGetForwardListCount(list));
    snapshot.history.reserve(static_cast<size_t>(back + forward + 1));

    for (int offset = -back; offset <= forward; ++offset) {
        HistoryEntry entry;
        if (WKBackForwardListItemRef item = api.backForwardListGetItemAtIndex(list, offset)) {
            WKURLRef url = api.backForwardListItemCopyURL(item);
            entry.url = textOfURL(url);
            releaseWebKitObject(url);

            WKStringRef title = api.backForwardListItemCopyTitle(item);
            entry.title = textOfString(title);
            releaseWebKitObject(title);
        }
        if (offset == 0)
            snapshot.historyCurrent = static_cast<int>(snapshot.history.size());
        snapshot.history.push_back(std::move(entry));
    }
}

void publishRecord(const TabRecord& record)
{
    PageSnapshot snapshot;
    snapshot.tabId = record.tabId;
    snapshot.slot = record.slot;
    snapshot.loading = record.loading;
    snapshot.errorCode = record.errorCode;
    snapshot.errorText = record.errorText;

    const WebKitApi* api = webKitApi();
    if (api && record.page) {
        WKStringRef title = api->pageCopyTitle(record.page);
        snapshot.title = textOfString(title);
        releaseWebKitObject(title);

        // WebKit's active URL is already the one to show: the request in
        // flight while there is one, and the committed document otherwise.
        WKURLRef active = api->pageCopyActiveURL(record.page);
        snapshot.url = textOfURL(active);
        releaseWebKitObject(active);

        WKURLRef committed = api->pageCopyCommittedURL(record.page);
        snapshot.committedURL = textOfURL(committed);
        releaseWebKitObject(committed);

        WKURLRef provisional = api->pageCopyProvisionalURL(record.page);
        snapshot.provisionalURL = textOfURL(provisional);
        releaseWebKitObject(provisional);

        snapshot.progress = api->pageGetEstimatedProgress(record.page);
        snapshot.canGoBack = api->pageCanGoBack(record.page);
        snapshot.canGoForward = api->pageCanGoForward(record.page);
        readHistory(*api, record.page, snapshot);
    }

    publish(snapshot);
}

void recordFailure(TabRecord& record, WKErrorRef error)
{
    record.loading = false;
    record.errorCode = 0;
    record.errorText.clear();

    const WebKitApi* api = webKitApi();
    if (!api || !error)
        return;

    record.errorCode = api->errorGetErrorCode(error);
    WKStringRef description = api->errorCopyLocalizedDescription(error);
    record.errorText = textOfString(description);
    releaseWebKitObject(description);
}

// --- The navigation client's reports ------------------------------------------

void didStartProvisionalNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    TabRecord* record = recordOf(clientInfo);
    if (!record)
        return;

    record->loading = true;
    record->errorCode = 0;
    record->errorText.clear();
    publishRecord(*record);
}

void didReceiveServerRedirectForProvisionalNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    if (TabRecord* record = recordOf(clientInfo))
        publishRecord(*record);
}

void didFailProvisionalNavigation(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* clientInfo)
{
    TabRecord* record = recordOf(clientInfo);
    if (!record)
        return;

    recordFailure(*record, error);
    publishRecord(*record);
}

void didCommitNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    if (TabRecord* record = recordOf(clientInfo))
        publishRecord(*record);
}

void didFinishDocumentLoad(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    if (TabRecord* record = recordOf(clientInfo))
        publishRecord(*record);
}

void didFinishNavigation(WKPageRef, WKNavigationRef, WKTypeRef, const void* clientInfo)
{
    TabRecord* record = recordOf(clientInfo);
    if (!record)
        return;

    record->loading = false;
    record->errorCode = 0;
    record->errorText.clear();
    publishRecord(*record);
}

void didFailNavigation(WKPageRef, WKNavigationRef, WKErrorRef error, WKTypeRef, const void* clientInfo)
{
    TabRecord* record = recordOf(clientInfo);
    if (!record)
        return;

    recordFailure(*record, error);
    publishRecord(*record);
}

void didSameDocumentNavigation(WKPageRef, WKNavigationRef, uint32_t, WKTypeRef, const void* clientInfo)
{
    if (TabRecord* record = recordOf(clientInfo))
        publishRecord(*record);
}

void webProcessDidCrash(WKPageRef, const void* clientInfo)
{
    TabRecord* record = recordOf(clientInfo);
    if (!record)
        return;

    record->loading = false;
    record->errorCode = kWebProcessEndedCode;
    record->errorText = "the page's web process ended";
    publishRecord(*record);
}

// --- Commands -----------------------------------------------------------------

void loadURL(const TabRecord& record, const WebKitApi& api, const std::string& url)
{
    if (url.empty())
        return;

    WKURLRef target = api.urlCreateWithUTF8CString(url.c_str());
    if (!target)
        return;

    api.pageLoadURL(record.page, target);
    releaseWebKitObject(target);
}

void goToHistoryEntry(const TabRecord& record, const WebKitApi& api, int index)
{
    WKBackForwardListRef list = api.pageGetBackForwardList(record.page);
    if (!list || index < 0)
        return;

    // The published list runs oldest first, so its index counts from the
    // oldest entry behind the current one, which is where the offset WebKit
    // wants counts from zero.
    const int back = static_cast<int>(api.backForwardListGetBackListCount(list));
    const int forward = static_cast<int>(api.backForwardListGetForwardListCount(list));
    if (index > back + forward)
        return;

    const int offset = index - back;
    if (!offset)
        return;

    WKBackForwardListItemRef item = api.backForwardListGetItemAtIndex(list, offset);
    if (item)
        api.pageGoToBackForwardListItem(record.page, item);
}

void runCommand(const Command& command)
{
    TabRecord* record = findRecord(command.tabId);
    const WebKitApi* api = webKitApi();
    if (!record || !api || !record->page)
        return;

    switch (command.kind) {
    case Command::Kind::Navigate:
        loadURL(*record, *api, command.url);
        break;
    case Command::Kind::Reload:
        api->pageReload(record->page);
        break;
    case Command::Kind::ReloadIgnoringCache:
        api->pageReloadFromOrigin(record->page);
        break;
    case Command::Kind::Stop:
        api->pageStopLoading(record->page);
        // Stopping ends the load without a navigation callback to say so.
        record->loading = false;
        break;
    case Command::Kind::Back:
        api->pageGoBack(record->page);
        break;
    case Command::Kind::Forward:
        api->pageGoForward(record->page);
        break;
    case Command::Kind::GoToHistoryEntry:
        goToHistoryEntry(*record, *api, command.index);
        break;
    }

    publishRecord(*record);
}

void drainCommands(void*)
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

// Any thread. The registry is asked to run the drain rather than left to reach
// it on its next idle turn: a navigation a person just asked for should not wait
// out the engine thread's sleep.
void post(Command&& command)
{
    {
        std::lock_guard<std::mutex> lock(g_commandMutex);
        g_commands.push_back(std::move(command));
    }
    hb_tabs_invoke_on_webkit_thread(drainCommands, nullptr);
}

// The row of the host's tab strip each tab occupies, which is its position in
// the registry's list. It changes when a tab is opened, closed, moved or pinned,
// and none of those is a report a page makes, so the caller reads it off the
// registry's revision rather than waiting for one.
void followTabStrip()
{
    for (const auto& record : g_records) {
        const int slot = hb_tabs_index_of(record->tabId);
        if (slot == record->slot)
            continue;
        record->slot = slot;
        publishSlot(record->tabId, slot);
    }
}

// --- The registry's seams -------------------------------------------------------

void writeNavigationClient(int, hb_wk_page_navigation_client_v3* navigation, void*)
{
    if (!navigation)
        return;

    // Only the fields this model owns are written. The two decidePolicy fields
    // and the three download fields are the registry's: a policy listener nobody
    // answers is a load that neither starts nor fails.
    //
    // The registry's struct mirrors WKPageNavigationClient version 3 field for
    // field, and every field in it is pointer-sized, so the callbacks are cast
    // into it rather than the layout being transcribed a second time here.
    navigation->didStartProvisionalNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidStartProvisionalNavigationCallback>(didStartProvisionalNavigation)
    );
    navigation->didReceiveServerRedirectForProvisionalNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidReceiveServerRedirectForProvisionalNavigationCallback>(didReceiveServerRedirectForProvisionalNavigation)
    );
    navigation->didFailProvisionalNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidFailProvisionalNavigationCallback>(didFailProvisionalNavigation)
    );
    navigation->didCommitNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidCommitNavigationCallback>(didCommitNavigation)
    );
    navigation->didFinishDocumentLoad = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidFinishDocumentLoadCallback>(didFinishDocumentLoad)
    );
    navigation->didFinishNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidFinishNavigationCallback>(didFinishNavigation)
    );
    navigation->didFailNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidFailNavigationCallback>(didFailNavigation)
    );
    navigation->didSameDocumentNavigation = reinterpret_cast<void*>(
        static_cast<WKPageNavigationDidSameDocumentNavigationCallback>(didSameDocumentNavigation)
    );
    navigation->webProcessDidCrash = reinterpret_cast<void*>(
        static_cast<WKPageNavigationWebProcessDidCrashCallback>(webProcessDidCrash)
    );
}

void pageStateChanged(int tabId, void* page, int field, void*)
{
    TabRecord* record = findRecord(tabId);
    if (!record || record->page != page)
        return;

    // Progress moves many times a second and carries one number. Republishing a
    // page's title, URL, history list and error text at that rate would spend
    // the whole of a load copying strings nothing read.
    if (field == HB_TABS_PAGE_STATE_PROGRESS) {
        const WebKitApi* api = webKitApi();
        if (api && record->page)
            publishProgress(record->tabId, api->pageGetEstimatedProgress(record->page));
        return;
    }
    publishRecord(*record);
}

void detachRecord(int tabId)
{
    const auto position = std::find_if(g_records.begin(), g_records.end(), [tabId](const std::unique_ptr<TabRecord>& record) {
        return record->tabId == tabId;
    });
    if (position == g_records.end())
        return;

    // Nothing is uninstalled: the clients on the page are the registry's, and
    // it destroys the page itself. What this model drops is its own record, and
    // every callback that could still arrive looks a record up by tab id and
    // finds nothing.
    g_records.erase(position);
    if (g_activeTabId == tabId)
        g_activeTabId = 0;
    unpublish(tabId);
}

void attachRecord(int tabId, WKPageRef page)
{
    if (tabId <= 0 || !page)
        return;
    if (!webKitApi())
        return;

    detachRecord(tabId);

    auto record = std::make_unique<TabRecord>();
    record->tabId = tabId;
    record->page = page;
    record->slot = hb_tabs_index_of(tabId);

    publishRecord(*record);
    g_records.push_back(std::move(record));

    // The registry publishes the tab after it has built the page, so the strip
    // has no row for this one yet. Forgetting the revision is what makes the
    // next cycle read the row it is about to be given.
    g_tabsRevision = -1;
}

void pageCreated(int tabId, void* page, void*)
{
    attachRecord(tabId, page);
}

void pageDestroying(int tabId, void*, void*)
{
    detachRecord(tabId);
}

void setActiveTab(int tabId)
{
    g_activeTabId = tabId > 0 ? tabId : 0;
    publishActiveTab(g_activeTabId);

    // The tab the host just showed answers for itself now, and a tab that was
    // suspended while it navigated has changes nobody has read yet.
    if (TabRecord* record = findRecord(g_activeTabId))
        publishRecord(*record);
}

// Runs on the WebKit thread, once per run-loop cycle, before WebKit's own.
//
// The registry publishes its selection and its order rather than announcing
// either, so both are read here: one revision compared per cycle, and work done
// only when it moved.
void engineCycle(void*)
{
    const int revision = hb_tabs_revision();
    if (revision != g_tabsRevision) {
        g_tabsRevision = revision;
        followTabStrip();

        const int active = hb_tabs_active_id();
        if (active != g_activeTabId)
            setActiveTab(active);
    }

    drainCommands(nullptr);
}

void engineTeardown(void*)
{
    g_records.clear();
    g_activeTabId = 0;
    g_tabsRevision = -1;

    {
        std::lock_guard<std::mutex> lock(g_commandMutex);
        g_commands.clear();
    }
    publishNothing();
}

} // namespace
} // namespace harmony_navigation

using namespace harmony_navigation;

// --- The engine seam --------------------------------------------------------------

extern "C" void hb_nav_attach(void)
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
    hb_tabs_add_navigation_client_hook(writeNavigationClient, nullptr);
    hb_tabs_add_page_state_observer(pageStateChanged, nullptr);
    hb_tabs_add_cycle_hook(engineCycle, nullptr);
    hb_tabs_add_teardown_hook(engineTeardown, nullptr);
}

extern "C" void hb_nav_shutdown(void)
{
    hb_tabs_invoke_on_webkit_thread(engineTeardown, nullptr);
}

// --- The host's frame thread navigates from here -------------------------------

extern "C" void hb_nav_navigate(int tab_id, const char* url)
{
    if (!url || !*url)
        return;

    Command command;
    command.kind = Command::Kind::Navigate;
    command.tabId = tab_id;
    command.url = url;
    post(std::move(command));
}

extern "C" void hb_nav_reload(int tab_id)
{
    Command command;
    command.kind = Command::Kind::Reload;
    command.tabId = tab_id;
    post(std::move(command));
}

extern "C" void hb_nav_reload_ignoring_cache(int tab_id)
{
    Command command;
    command.kind = Command::Kind::ReloadIgnoringCache;
    command.tabId = tab_id;
    post(std::move(command));
}

extern "C" void hb_nav_stop(int tab_id)
{
    Command command;
    command.kind = Command::Kind::Stop;
    command.tabId = tab_id;
    post(std::move(command));
}

extern "C" void hb_nav_go_back(int tab_id)
{
    Command command;
    command.kind = Command::Kind::Back;
    command.tabId = tab_id;
    post(std::move(command));
}

extern "C" void hb_nav_go_forward(int tab_id)
{
    Command command;
    command.kind = Command::Kind::Forward;
    command.tabId = tab_id;
    post(std::move(command));
}

extern "C" void hb_nav_go_to_history_entry(int tab_id, int index)
{
    Command command;
    command.kind = Command::Kind::GoToHistoryEntry;
    command.tabId = tab_id;
    command.index = index;
    post(std::move(command));
}
