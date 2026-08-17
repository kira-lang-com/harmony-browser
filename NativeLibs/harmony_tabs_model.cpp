#include "harmony_tabs_internal.h"

#include "harmony_data_store.h"

// The registry itself: which tabs exist, in what order, which one is showing,
// and what happens to that order when one is opened, closed, reopened, moved or
// pinned. Everything here runs on the WebKit thread.
//
// Pinned tabs are a PREFIX of the list rather than a flag scattered through it.
// That single invariant is what makes reordering, insertion and the two select
// directions agree with each other without any of them re-sorting the list.

namespace harmony_tabs {

namespace {

// Where a tab may be inserted, given whether it is pinned. A negative index
// means the end of its own group.
size_t clampInsertIndex(int index, bool pinned)
{
    const size_t pinned_ = pinnedCount();
    const size_t low = pinned ? 0 : pinned_;
    const size_t high = pinned ? pinned_ : g_tabs.size();
    if (index < 0)
        return high;

    size_t at = static_cast<size_t>(index);
    if (at < low)
        at = low;
    if (at > high)
        at = high;
    return at;
}

void rememberClosedTab(const Tab& tab, int index)
{
    if (tab.url.empty())
        return;

    ClosedTab closed;
    closed.url = tab.url;
    closed.title = tab.title;
    closed.index = index;
    closed.pinned = tab.pinned;
    g_closedTabs.push_back(std::move(closed));
    if (g_closedTabs.size() > kClosedTabHistory)
        g_closedTabs.erase(g_closedTabs.begin());
}

// WebKit's Windows view closes its page from the child window's WM_DESTROY
// path. Destroy the child first so the parent cannot later deliver a
// WM_DESTROY to a freed object.
void destroyTab(Tab& tab)
{
    notifyPageDestroying(tab);
    releaseSnapshot(tab);

    if (tab.child && IsWindow(tab.child))
        DestroyWindow(tab.child);
    else if (tab.page && g_api.pageClose)
        g_api.pageClose(tab.page);

    release(tab.view);
    tab.view = nullptr;
    tab.page = nullptr;
    tab.child = nullptr;
}

// Selects whatever should show once the tab at `index` has gone: its right-hand
// neighbour, or its left-hand one when it was last.
void selectAfterRemoval(size_t index)
{
    g_activeTabId = 0;
    g_activeChild.store(nullptr);
    if (g_tabs.empty()) {
        hideSnapshot();
        publishTabs();
        return;
    }

    size_t next = index;
    if (next >= g_tabs.size())
        next = g_tabs.size() - 1;
    selectTab(g_tabs[next]->id);
}

} // namespace

// --- Lookup -----------------------------------------------------------------

Tab* findTab(int id)
{
    if (id <= 0)
        return nullptr;
    for (auto& tab : g_tabs) {
        if (tab->id == id)
            return tab.get();
    }
    return nullptr;
}

Tab* findTabByPage(WKPageRef page)
{
    if (!page)
        return nullptr;
    for (auto& tab : g_tabs) {
        if (tab->page == page)
            return tab.get();
    }
    return nullptr;
}

Tab* activeTab()
{
    return findTab(g_activeTabId);
}

int indexOfTab(int id)
{
    for (size_t i = 0; i < g_tabs.size(); ++i) {
        if (g_tabs[i]->id == id)
            return static_cast<int>(i);
    }
    return -1;
}

size_t pinnedCount()
{
    size_t count = 0;
    for (const auto& tab : g_tabs) {
        if (tab->pinned)
            ++count;
    }
    return count;
}

// --- Publication ------------------------------------------------------------

void publishTabs()
{
    std::vector<PublishedTab> snapshot;
    snapshot.reserve(g_tabs.size());
    for (const auto& tab : g_tabs) {
        PublishedTab entry;
        entry.id = tab->id;
        entry.title = tab->title;
        entry.url = tab->url;
        entry.host = tab->host;
        entry.loading = tab->loading;
        entry.pinned = tab->pinned;
        entry.canGoBack = tab->canGoBack;
        entry.canGoForward = tab->canGoForward;
        entry.progress = tab->progress;
        snapshot.push_back(std::move(entry));
    }

    const int closedCount = static_cast<int>(g_closedTabs.size());
    {
        std::lock_guard<std::mutex> lock(g_publishedMutex);
        g_published = std::move(snapshot);
        g_publishedActiveId = g_activeTabId;
        g_publishedClosedCount = closedCount;
    }
    g_revision.fetch_add(1);
}

// --- Creation ---------------------------------------------------------------

int reserveTabId()
{
    return g_nextTabId.fetch_add(1);
}

Tab* createTab(WKPageConfigurationRef configuration, int id, int index, bool pinned, bool foreground)
{
    HWND parent = g_parentWindow.load();
    if (!parent || !configuration || !g_api.viewCreate || id <= 0 || findTab(id))
        return nullptr;

    const RECT rect = currentBoundsRect();
    WKViewRef view = g_api.viewCreate(rect, configuration, parent);
    if (!view) {
        setError("WKViewCreate failed");
        return nullptr;
    }

    auto owned = std::make_unique<Tab>();
    owned->id = id;
    owned->pinned = pinned;
    owned->view = view;
    owned->child = g_api.viewGetWindow(view);
    owned->page = g_api.viewGetPage(view);
    if (!owned->child || !owned->page) {
        setError("WKViewGetWindow failed");
        release(view);
        return nullptr;
    }

    g_api.viewSetParentWindow(view, parent);
    g_api.viewSetIsInWindow(view, true);
    subclassTabChild(owned->child);

    owned->width = rect.right - rect.left;
    owned->height = rect.bottom - rect.top;
    SetWindowPos(
        owned->child,
        HWND_BOTTOM,
        rect.left,
        rect.top,
        owned->width,
        owned->height,
        SWP_NOACTIVATE | SWP_NOREDRAW | SWP_HIDEWINDOW
    );

    // A new tab starts suspended and is woken by being selected. Loading is not
    // rendering: the page still fetches and parses while it is out of the
    // window, it just does not draw.
    owned->suspended = true;
    g_api.viewSetIsInWindow(view, false);

    Tab* tab = owned.get();
    const size_t at = clampInsertIndex(index, pinned);
    g_tabs.insert(g_tabs.begin() + static_cast<ptrdiff_t>(at), std::move(owned));

    configurePage(*tab);
    installPageClients(*tab);
    refreshTabState(*tab);

    if (foreground || !g_activeTabId)
        selectTab(tab->id);
    else
        publishTabs();
    return tab;
}

int openTab(int id, const std::string& url, int index, bool foreground)
{
    if (!g_context || !g_api.pageConfigurationCreate)
        return 0;

    WKPageConfigurationRef configuration = g_api.pageConfigurationCreate();
    if (!configuration) {
        setError("WKPageConfigurationCreate failed");
        return 0;
    }
    g_api.pageConfigurationSetContext(configuration, g_context);
    // No related page is set, deliberately: pages created as related to another
    // share its web process, and two tabs should not share one.
    //
    // Which website data store the page gets is the profile's decision, and it
    // is made per tab: a tab opened privately gets the ephemeral one.
    hb_data_store_apply_to_page_configuration(const_cast<void*>(configuration), id);

    Tab* tab = createTab(configuration, id, index, false, foreground);
    release(configuration);
    if (!tab)
        return 0;

    loadInTabDirect(*tab, url.empty() ? homeURL() : url);
    return tab->id;
}

// --- Closing ----------------------------------------------------------------

void closeTab(int id)
{
    const int at = indexOfTab(id);
    if (at < 0)
        return;

    const size_t index = static_cast<size_t>(at);
    Tab& tab = *g_tabs[index];
    const bool wasActive = g_activeTabId == id;

    rememberClosedTab(tab, at);
    destroyTab(tab);
    g_tabs.erase(g_tabs.begin() + at);

    if (wasActive) {
        selectAfterRemoval(index);
        return;
    }
    publishTabs();
}

void closeOtherTabs(int id)
{
    if (indexOfTab(id) < 0)
        return;

    // The survivor is shown first so that closing the rest never has to pick a
    // replacement, which would put a tab on screen only to close it next.
    selectTab(id);

    std::vector<int> doomed;
    doomed.reserve(g_tabs.size());
    for (const auto& tab : g_tabs) {
        if (tab->id != id)
            doomed.push_back(tab->id);
    }
    for (int victim : doomed)
        closeTab(victim);
}

void closeAllTabs()
{
    if (g_tabs.empty())
        return;

    // Nothing is selected first, so no tab is brought forward on its way out.
    g_activeTabId = 0;
    g_activeChild.store(nullptr);
    hideSnapshot();

    // Recorded left to right, so the reopen stack gives the rightmost tab back
    // first and the list rebuilds itself from the end inwards.
    for (size_t at = 0; at < g_tabs.size(); ++at)
        rememberClosedTab(*g_tabs[at], static_cast<int>(at));

    // Destroying a tab runs the page observers and WebKit's own teardown, and a
    // page is entitled to open another from inside either. So each tab is taken
    // OUT of the list before it is destroyed and the list is re-read every turn:
    // an iterator walking the vector would not survive the insertion, and a
    // clear afterwards would free the new tab without telling the observers it
    // was going.
    while (!g_tabs.empty()) {
        std::unique_ptr<Tab> tab = std::move(g_tabs.back());
        g_tabs.pop_back();
        destroyTab(*tab);
    }

    // A tab created and then destroyed by the loop above may have selected
    // itself on the way in, so the selection is cleared again after it.
    g_activeTabId = 0;
    g_activeChild.store(nullptr);
    publishTabs();
}

int reopenClosedTab(int id)
{
    if (g_closedTabs.empty() || !g_context || !g_api.pageConfigurationCreate)
        return 0;

    ClosedTab closed = std::move(g_closedTabs.back());
    g_closedTabs.pop_back();

    WKPageConfigurationRef configuration = g_api.pageConfigurationCreate();
    if (!configuration) {
        setError("WKPageConfigurationCreate failed");
        return 0;
    }
    g_api.pageConfigurationSetContext(configuration, g_context);
    hb_data_store_apply_to_page_configuration(const_cast<void*>(configuration), id);

    Tab* tab = createTab(configuration, id, closed.index, closed.pinned, true);
    release(configuration);
    if (!tab)
        return 0;

    // The label the tab had is worth showing while its page loads again, so the
    // reopened tab is recognisable before its title arrives.
    if (!closed.title.empty()) {
        tab->title = closed.title;
        publishTabs();
    }
    loadInTabDirect(*tab, closed.url);
    return tab->id;
}

// --- Selection --------------------------------------------------------------

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

    Tab* previous = findTab(previousId);
    if (previous)
        captureSnapshot(*previous);

    g_activeTabId = id;
    g_activeChild.store(target->child);

    showSnapshot(*target);
    resumeTab(*target);
    requestRepaint(*target);

    if (previous)
        suspendTab(*previous);

    publishTabs();
}

void selectTabAtIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(g_tabs.size()))
        return;
    selectTab(g_tabs[static_cast<size_t>(index)]->id);
}

void selectAdjacentTab(int delta)
{
    if (g_tabs.empty())
        return;

    const int count = static_cast<int>(g_tabs.size());
    int at = indexOfTab(g_activeTabId);
    if (at < 0)
        at = 0;

    // Wrapping, so the shortcut a browser binds to this cycles rather than
    // stopping at the ends of the strip.
    int next = (at + delta) % count;
    if (next < 0)
        next += count;
    selectTab(g_tabs[static_cast<size_t>(next)]->id);
}

// --- Ordering ---------------------------------------------------------------

void moveTab(int id, int toIndex)
{
    const int from = indexOfTab(id);
    if (from < 0)
        return;

    auto moved = std::move(g_tabs[static_cast<size_t>(from)]);
    const bool pinned = moved->pinned;
    g_tabs.erase(g_tabs.begin() + from);

    const size_t at = clampInsertIndex(toIndex, pinned);
    g_tabs.insert(g_tabs.begin() + static_cast<ptrdiff_t>(at), std::move(moved));
    publishTabs();
}

void setTabPinned(int id, bool pinned)
{
    const int from = indexOfTab(id);
    if (from < 0 || g_tabs[static_cast<size_t>(from)]->pinned == pinned)
        return;

    auto moved = std::move(g_tabs[static_cast<size_t>(from)]);
    g_tabs.erase(g_tabs.begin() + from);
    moved->pinned = pinned;

    // Pinning lands at the end of the pinned prefix and unpinning at the front
    // of the unpinned suffix, which are the same position in the list the tab
    // has just been taken out of.
    const size_t at = pinnedCount();
    g_tabs.insert(g_tabs.begin() + static_cast<ptrdiff_t>(at), std::move(moved));
    publishTabs();
}

// --- Navigation -------------------------------------------------------------

void loadInTabDirect(Tab& tab, const std::string& url)
{
    if (!tab.page || url.empty() || !g_api.urlCreateWithUTF8CString)
        return;

    const std::string target = normalizedURL(url.c_str());
    if (target.empty())
        return;

    WKURLRef webURL = g_api.urlCreateWithUTF8CString(target.c_str());
    if (!webURL)
        return;

    g_api.pageLoadURL(tab.page, webURL);
    release(webURL);

    refreshTabState(tab);
    publishTabs();
}

void loadInTab(int id, const std::string& url)
{
    if (Tab* tab = findTab(id))
        loadInTabDirect(*tab, url);
}

} // namespace harmony_tabs
