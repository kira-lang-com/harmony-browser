#include "harmony_navigation_model.h"
#include "harmony_navigation_state.h"

#include <algorithm>
#include <mutex>

namespace harmony_navigation {

namespace {

std::mutex g_mutex;
std::vector<PageSnapshot> g_pages;
int g_activeTabId { 0 };
std::string g_diagnostic;

// Reading a tab's model is one lock, one copy, one unlock. The copy is what
// lets a reader answer without holding the lock across the return into Kira,
// where the seam copies the bytes again.
//
// A returned pointer stays good until this thread has asked eight more
// questions: long enough for a caller to read it, short enough that nothing
// accumulates.
const char* answer(const std::string& value)
{
    constexpr size_t slotCount = 8;
    static thread_local std::string slots[slotCount];
    static thread_local size_t next = 0;

    std::string& slot = slots[next];
    next = (next + 1) % slotCount;
    slot = value;
    return slot.c_str();
}

// The caller's tab, with zero and anything negative naming the active one.
const PageSnapshot* lookupLocked(int tabId)
{
    const int wanted = tabId > 0 ? tabId : g_activeTabId;
    if (!wanted)
        return nullptr;

    const auto position = std::find_if(g_pages.begin(), g_pages.end(), [wanted](const PageSnapshot& page) {
        return page.tabId == wanted;
    });
    return position == g_pages.end() ? nullptr : &*position;
}

// One tab's model, held under the lock for as long as the reader needs it.
// The lock is declared first so it is taken before the lookup runs and
// released after the answer is copied.
class Reader {
public:
    explicit Reader(int tabId)
        : m_lock(g_mutex)
        , m_page(lookupLocked(tabId))
    {
    }

    const PageSnapshot* page() const { return m_page; }

    const HistoryEntry* entry(int index) const
    {
        if (!m_page || index < 0 || static_cast<size_t>(index) >= m_page->history.size())
            return nullptr;
        return &m_page->history[static_cast<size_t>(index)];
    }

private:
    std::lock_guard<std::mutex> m_lock;
    const PageSnapshot* m_page;
};

} // namespace

void publish(const PageSnapshot& snapshot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto position = std::find_if(g_pages.begin(), g_pages.end(), [&snapshot](const PageSnapshot& page) {
        return page.tabId == snapshot.tabId;
    });
    if (position == g_pages.end())
        g_pages.push_back(snapshot);
    else
        *position = snapshot;
}

void publishSlot(int tabId, int slot)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto position = std::find_if(g_pages.begin(), g_pages.end(), [tabId](const PageSnapshot& page) {
        return page.tabId == tabId;
    });
    if (position != g_pages.end())
        position->slot = slot;
}

void publishProgress(int tabId, double progress)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto position = std::find_if(g_pages.begin(), g_pages.end(), [tabId](const PageSnapshot& page) {
        return page.tabId == tabId;
    });
    if (position != g_pages.end())
        position->progress = progress;
}

void unpublish(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pages.erase(
        std::remove_if(g_pages.begin(), g_pages.end(), [tabId](const PageSnapshot& page) {
            return page.tabId == tabId;
        }),
        g_pages.end()
    );
    if (g_activeTabId == tabId)
        g_activeTabId = 0;
}

void publishActiveTab(int tabId)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_activeTabId = tabId;
}

void publishNothing()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pages.clear();
    g_activeTabId = 0;
}

void setDiagnostic(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_diagnostic = message;
}

} // namespace harmony_navigation

// --- The host's frame thread reads from here ---------------------------------

using namespace harmony_navigation;

extern "C" int hb_nav_active_tab(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_activeTabId;
}

extern "C" int hb_nav_tab_for_slot(int slot)
{
    if (slot < 0)
        return 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& page : g_pages) {
        if (page.slot == slot)
            return page.tabId;
    }
    return 0;
}

extern "C" int hb_nav_tab_count(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_pages.size());
}

extern "C" int hb_nav_tab_id_at(int index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || static_cast<size_t>(index) >= g_pages.size())
        return 0;
    return g_pages[static_cast<size_t>(index)].tabId;
}

extern "C" int hb_nav_tab_slot(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? reader.page()->slot : -1;
}

extern "C" const char* hb_nav_url(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? answer(reader.page()->url) : "";
}

extern "C" const char* hb_nav_committed_url(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? answer(reader.page()->committedURL) : "";
}

extern "C" const char* hb_nav_provisional_url(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? answer(reader.page()->provisionalURL) : "";
}

extern "C" const char* hb_nav_title(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? answer(reader.page()->title) : "";
}

extern "C" double hb_nav_progress(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? reader.page()->progress : 0.0;
}

extern "C" int hb_nav_is_loading(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() && reader.page()->loading ? 1 : 0;
}

extern "C" int hb_nav_can_go_back(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() && reader.page()->canGoBack ? 1 : 0;
}

extern "C" int hb_nav_can_go_forward(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() && reader.page()->canGoForward ? 1 : 0;
}

extern "C" int hb_nav_error_code(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? reader.page()->errorCode : 0;
}

extern "C" const char* hb_nav_error_text(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? answer(reader.page()->errorText) : "";
}

extern "C" int hb_nav_history_count(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? static_cast<int>(reader.page()->history.size()) : 0;
}

extern "C" int hb_nav_history_current(int tab_id)
{
    const Reader reader(tab_id);
    return reader.page() ? reader.page()->historyCurrent : -1;
}

extern "C" const char* hb_nav_history_url(int tab_id, int index)
{
    const Reader reader(tab_id);
    const HistoryEntry* entry = reader.entry(index);
    return entry ? answer(entry->url) : "";
}

extern "C" const char* hb_nav_history_title(int tab_id, int index)
{
    const Reader reader(tab_id);
    const HistoryEntry* entry = reader.entry(index);
    return entry ? answer(entry->title) : "";
}

extern "C" const char* hb_nav_diagnostic(void)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    return answer(g_diagnostic);
}
