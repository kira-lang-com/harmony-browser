#include "harmony_bookmarks_internal.h"

// The history's Kira-facing surface, and the module's lifetime.
//
// Every read here is a lock, a copy and an unlock. A query is answered into this
// thread's own latch and the accessors read that latch, so the six fields a row
// costs all belong to one answer even while the WebKit thread records a visit
// between two of them.
//
// The bookmark tree's entry points are with the tree, and the suggestion model's
// are with the ranking, because both of those are one call per field against a
// model that is already in memory. What is left here is the history and the
// module itself.

using namespace harmony::bookmarks;

namespace {

thread_local std::vector<HistoryRow> t_rows;

const HistoryRow* rowAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= t_rows.size())
        return nullptr;
    return &t_rows[static_cast<size_t>(index)];
}

} // namespace

// --- Lifecycle --------------------------------------------------------------

extern "C" int hb_bookmarks_ready(void)
{
    return filesReady() ? 1 : 0;
}

extern "C" const char* hb_bookmarks_error(void)
{
    return answer(currentError());
}

extern "C" double hb_bookmarks_now(void)
{
    return unixNow();
}

extern "C" int hb_bookmarks_revision(void)
{
    return currentRevision();
}

// --- Querying ---------------------------------------------------------------

extern "C" int hb_history_query(const char* text, int match, int limit)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    t_rows = historyQueryLocked(text ? text : "", match, limit);
    return static_cast<int>(t_rows.size());
}

extern "C" const char* hb_history_url(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? answer(row->url) : "";
}

extern "C" const char* hb_history_title(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? answer(row->title) : "";
}

extern "C" const char* hb_history_host(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? answer(row->host) : "";
}

extern "C" double hb_history_last_visit(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? row->lastVisit : 0.0;
}

extern "C" int hb_history_visit_count(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? row->visitCount : 0;
}

extern "C" int hb_history_day(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? row->day : 0;
}

extern "C" double hb_history_score(int index)
{
    const HistoryRow* row = rowAt(index);
    return row ? row->score : 0.0;
}

extern "C" int hb_history_count(void)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return static_cast<int>(historyEntriesLocked().size());
}

extern "C" int hb_history_visit_total(void)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    size_t total = 0;
    for (const HistoryEntry& entry : historyEntriesLocked())
        total += entry.visits.size();
    return static_cast<int>(total);
}

// --- The calendar -----------------------------------------------------------

extern "C" int hb_history_today(void)
{
    return localDayOf(unixNow());
}

extern "C" int hb_history_day_year(int day)
{
    int year = 0;
    int month = 0;
    int dayOfMonth = 0;
    int weekday = 0;
    return calendarOfDay(day, year, month, dayOfMonth, weekday) ? year : 0;
}

extern "C" int hb_history_day_month(int day)
{
    int year = 0;
    int month = 0;
    int dayOfMonth = 0;
    int weekday = 0;
    return calendarOfDay(day, year, month, dayOfMonth, weekday) ? month : 0;
}

extern "C" int hb_history_day_of_month(int day)
{
    int year = 0;
    int month = 0;
    int dayOfMonth = 0;
    int weekday = 0;
    return calendarOfDay(day, year, month, dayOfMonth, weekday) ? dayOfMonth : 0;
}

extern "C" int hb_history_day_weekday(int day)
{
    int year = 0;
    int month = 0;
    int dayOfMonth = 0;
    int weekday = 0;
    return calendarOfDay(day, year, month, dayOfMonth, weekday) ? weekday : 0;
}

extern "C" int hb_history_minute_of_day(double unix_seconds)
{
    return localMinuteOfDay(unix_seconds);
}

// --- Forgetting -------------------------------------------------------------

extern "C" void hb_history_forget_url(const char* url)
{
    if (!url || !*url)
        return;

    std::lock_guard<std::mutex> lock(modelMutex());
    historyForgetURLLocked(url);
}

extern "C" void hb_history_forget_host(const char* host)
{
    if (!host || !*host)
        return;

    std::lock_guard<std::mutex> lock(modelMutex());
    historyForgetHostLocked(host);
}

extern "C" void hb_history_forget_since(double since)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    historyForgetSinceLocked(since);
}
