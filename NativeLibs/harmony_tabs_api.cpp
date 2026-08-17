#include "harmony_tabs.h"
#include "harmony_tabs_internal.h"

#include <cstdio>

// The boundary the host calls across. Everything here runs on the host's frame
// thread: it reads the published copy of the list, or queues a command, and
// never touches a WebKit object.

using namespace harmony_tabs;

namespace {

std::atomic<bool> g_firstTabQueued { false };

const PublishedTab* publishedTab(int tabId)
{
    for (const auto& entry : g_published) {
        if (entry.id == tabId)
            return &entry;
    }
    return nullptr;
}

// Hands back this thread's own copy so the pointer stays good for as long as
// the caller needs to read it, whichever thread asked.
const char* publishText(std::string&& value, std::string& storage)
{
    storage = std::move(value);
    return storage.c_str();
}

// The id is reserved here rather than on the WebKit thread so the caller holds
// the tab's id the moment it asks for it, and can select, move or close the tab
// before the thread has got round to creating it.
int postOpen(const char* url, int index, bool foreground)
{
    Command command;
    command.kind = Command::Kind::Open;
    command.tabId = reserveTabId();
    command.url = url ? url : "";
    command.index = index;
    command.flag = foreground;

    const int id = command.tabId;
    postCommand(std::move(command));
    return id;
}

void postIndexed(Command::Kind kind, int tabId, int index, bool flag = false)
{
    Command command;
    command.kind = kind;
    command.tabId = tabId;
    command.index = index;
    command.flag = flag;
    postCommand(std::move(command));
}

// Runs on the WebKit thread after the host has handed over a different window
// to live in. Every view has to follow, and the stand-in window belongs to the
// old parent and has to be rebuilt under the new one.
void reparentViews(void*)
{
    HWND parent = g_parentWindow.load();
    if (!parent)
        return;

    destroySnapshotWindow();
    for (auto& tab : g_tabs) {
        if (!tab->view)
            continue;
        g_api.viewSetParentWindow(tab->view, parent);
        if (tab->id == g_activeTabId)
            resumeTab(*tab);
        else
            suspendTab(*tab);
    }
}

} // namespace

// --- Engine lifetime --------------------------------------------------------

extern "C" int hb_tabs_start(void* parent_window, const char* home_url)
{
    if (!parent_window) {
        setError("invalid WebKit parent window");
        return 0;
    }

    HWND parent = static_cast<HWND>(parent_window);
    HWND previous = g_parentWindow.exchange(parent);

    if (home_url && *home_url) {
        std::lock_guard<std::mutex> lock(g_homeMutex);
        g_home = home_url;
    }

    if (!startWebKitThread())
        return 0;

    if (previous && previous != parent) {
        Command command;
        command.kind = Command::Kind::Invoke;
        command.invoke = reparentViews;
        postCommand(std::move(command));
    }

    // Queued, not dropped: the thread drains whatever arrived while it started.
    // Said once, on the call that opens the first tab, because the host calls
    // this every frame and a browser that reached its engine should say so
    // exactly as loudly as one that did not.
    bool expected = false;
    if (g_firstTabQueued.compare_exchange_strong(expected, true)) {
        postOpen(home_url, -1, true);
        std::fprintf(stderr, "harmony: tabs: webkit thread started for %s\n",
            home_url && *home_url ? home_url : "(home)");
    }

    return g_ready.load() ? 1 : 0;
}

extern "C" int hb_tabs_ready(void)
{
    return g_ready.load() ? 1 : 0;
}

extern "C" void hb_tabs_set_home(const char* url)
{
    if (!url || !*url)
        return;
    std::lock_guard<std::mutex> lock(g_homeMutex);
    g_home = url;
}

extern "C" void hb_tabs_shutdown(void)
{
    stopWebKitThread();
    g_firstTabQueued.store(false);
}

extern "C" const char* hb_tabs_error(void)
{
    static thread_local std::string storage;
    return publishText(currentError(), storage);
}

// --- Geometry ---------------------------------------------------------------

extern "C" void hb_tabs_set_bounds(int x, int y, int width, int height)
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
    if (child && IsWindow(child)) {
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

    postSimpleCommand(Command::Kind::ApplyBounds);
}

extern "C" void hb_tabs_set_backing_scale(double scale)
{
    if (!(scale > 0.0))
        return;

    const double previous = g_backingScale.load();
    if (previous == scale)
        return;

    g_backingScale.store(scale);
    postSimpleCommand(Command::Kind::ApplyBackingScale);
}

// --- Opening and closing ----------------------------------------------------

extern "C" int hb_tabs_open(const char* url, int foreground)
{
    return postOpen(url, -1, foreground != 0);
}

extern "C" int hb_tabs_open_at(const char* url, int index, int foreground)
{
    return postOpen(url, index, foreground != 0);
}

extern "C" void hb_tabs_close(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Close, tab_id);
}

extern "C" void hb_tabs_close_others(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::CloseOthers, tab_id);
}

extern "C" void hb_tabs_close_all(void)
{
    postSimpleCommand(Command::Kind::CloseAll);
}

extern "C" int hb_tabs_reopen_closed(void)
{
    if (hb_tabs_closed_count() <= 0)
        return 0;

    Command command;
    command.kind = Command::Kind::ReopenClosed;
    command.tabId = reserveTabId();

    const int id = command.tabId;
    postCommand(std::move(command));
    return id;
}

extern "C" int hb_tabs_closed_count(void)
{
    // Reading the WebKit thread's own reopen stack would be a race, so its
    // depth rides along with the published list instead.
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    return g_publishedClosedCount;
}

// --- Selection --------------------------------------------------------------

extern "C" void hb_tabs_select(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Select, tab_id);
}

extern "C" void hb_tabs_select_index(int index)
{
    if (index >= 0)
        postIndexed(Command::Kind::SelectIndex, 0, index);
}

extern "C" void hb_tabs_select_next(void)
{
    postSimpleCommand(Command::Kind::SelectNext);
}

extern "C" void hb_tabs_select_previous(void)
{
    postSimpleCommand(Command::Kind::SelectPrevious);
}

// --- Ordering ---------------------------------------------------------------

extern "C" void hb_tabs_move(int tab_id, int to_index)
{
    if (tab_id > 0)
        postIndexed(Command::Kind::Move, tab_id, to_index);
}

extern "C" void hb_tabs_set_pinned(int tab_id, int pinned)
{
    if (tab_id > 0)
        postIndexed(Command::Kind::SetPinned, tab_id, -1, pinned != 0);
}

// --- Navigation -------------------------------------------------------------

extern "C" void hb_tabs_load(int tab_id, const char* url)
{
    if (tab_id <= 0)
        return;

    Command command;
    command.kind = Command::Kind::Load;
    command.tabId = tab_id;
    command.url = url ? url : "";
    postCommand(std::move(command));
}

extern "C" void hb_tabs_go_back(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Back, tab_id);
}

extern "C" void hb_tabs_go_forward(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Forward, tab_id);
}

extern "C" void hb_tabs_reload(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Reload, tab_id);
}

extern "C" void hb_tabs_stop(int tab_id)
{
    if (tab_id > 0)
        postSimpleCommand(Command::Kind::Stop, tab_id);
}

// --- The published list -----------------------------------------------------

extern "C" int hb_tabs_count(void)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    return static_cast<int>(g_published.size());
}

extern "C" int hb_tabs_revision(void)
{
    return g_revision.load();
}

extern "C" int hb_tabs_id_at(int index)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    if (index < 0 || index >= static_cast<int>(g_published.size()))
        return 0;
    return g_published[static_cast<size_t>(index)].id;
}

extern "C" int hb_tabs_index_of(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    for (size_t i = 0; i < g_published.size(); ++i) {
        if (g_published[i].id == tab_id)
            return static_cast<int>(i);
    }
    return -1;
}

extern "C" int hb_tabs_active_id(void)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    return g_publishedActiveId;
}

extern "C" int hb_tabs_active_index(void)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    for (size_t i = 0; i < g_published.size(); ++i) {
        if (g_published[i].id == g_publishedActiveId)
            return static_cast<int>(i);
    }
    return -1;
}

extern "C" const char* hb_tabs_title(int tab_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return publishText(entry ? std::string(entry->title) : std::string(), storage);
}

extern "C" const char* hb_tabs_url(int tab_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return publishText(entry ? std::string(entry->url) : std::string(), storage);
}

extern "C" const char* hb_tabs_host(int tab_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return publishText(entry ? std::string(entry->host) : std::string(), storage);
}

extern "C" int hb_tabs_is_loading(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return entry && entry->loading ? 1 : 0;
}

extern "C" int hb_tabs_is_pinned(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return entry && entry->pinned ? 1 : 0;
}

extern "C" int hb_tabs_can_go_back(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return entry && entry->canGoBack ? 1 : 0;
}

extern "C" int hb_tabs_can_go_forward(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return entry && entry->canGoForward ? 1 : 0;
}

extern "C" double hb_tabs_progress(int tab_id)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    const PublishedTab* entry = publishedTab(tab_id);
    return entry ? entry->progress : 0.0;
}

// --- The seam other native modules reach WebKit through ---------------------

extern "C" void* hb_tabs_webkit_symbol(const char* name)
{
    if (!name || !g_api.module)
        return nullptr;
    return reinterpret_cast<void*>(GetProcAddress(g_api.module, name));
}

extern "C" void* hb_tabs_context(void)
{
    return const_cast<void*>(g_context);
}

extern "C" void* hb_tabs_page(int tab_id)
{
    Tab* tab = findTab(tab_id);
    return tab ? const_cast<void*>(tab->page) : nullptr;
}

extern "C" void* hb_tabs_view(int tab_id)
{
    Tab* tab = findTab(tab_id);
    return tab ? const_cast<void*>(tab->view) : nullptr;
}

extern "C" int hb_tabs_id_for_page(void* page)
{
    Tab* tab = findTabByPage(page);
    return tab ? tab->id : 0;
}

extern "C" int hb_tabs_on_webkit_thread(void)
{
    const DWORD owner = g_webKitThreadId.load();
    return owner && owner == GetCurrentThreadId() ? 1 : 0;
}

extern "C" void hb_tabs_invoke_on_webkit_thread(void (*fn)(void*), void* context)
{
    if (!fn)
        return;

    Command command;
    command.kind = Command::Kind::Invoke;
    command.invoke = fn;
    command.invokeContext = context;
    postCommand(std::move(command));
}

extern "C" void hb_tabs_add_page_observer(hb_tabs_page_hook on_created, hb_tabs_page_hook on_destroying, void* user_data)
{
    if (!on_created && !on_destroying)
        return;

    PageObserver observer;
    observer.created = on_created;
    observer.destroying = on_destroying;
    observer.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_pageObservers.push_back(observer);
}

extern "C" void hb_tabs_add_ui_client_hook(hb_tabs_ui_client_hook hook, void* user_data)
{
    if (!hook)
        return;

    UiClientHook entry;
    entry.hook = hook;
    entry.userData = user_data;

    std::lock_guard<std::mutex> lock(g_hookMutex);
    g_uiClientHooks.push_back(entry);
}
