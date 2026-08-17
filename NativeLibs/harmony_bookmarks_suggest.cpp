#include "harmony_bookmarks_internal.h"

#include <algorithm>

// What the address bar offers while a person types.
//
// One ranking over both models, because they are one question: a page that is
// both bookmarked and read every day should appear once, at the rank the
// stronger of the two answers gives it, and not twice with the weaker copy
// underneath. So bookmarks are scored, history is scored, and an address in both
// keeps the higher score and says it is a bookmark -- which is the more useful
// thing to know about a row that is about to be pressed.
//
// What the TYPED text itself means is not decided here. Turning a phrase into a
// search and a bare host into an address is the navigation system's decision,
// made once, where its result is what the bar then shows.

namespace harmony::bookmarks {

namespace {

// What being bookmarked is worth against being read. A bookmark is a page the
// person asked to keep, which outranks a page they merely passed through, and
// this is the margin by which: roughly two weeks of daily reading.
constexpr double kBookmarkWeight = 45.0;

// The match weights a bookmark is ranked by. They mirror the history's, so a
// bookmarked host and a visited host answering the same prefix are separated by
// what is known about them rather than by which list they came from.
constexpr double kBookmarkHostPrefix = 120.0;
constexpr double kBookmarkURLPrefix = 90.0;
constexpr double kBookmarkTitlePrefix = 60.0;
constexpr double kBookmarkHostInside = 30.0;
constexpr double kBookmarkURLInside = 14.0;
constexpr double kBookmarkTitleInside = 10.0;

double bookmarkScore(const BookmarkNode& node, const std::string& folded)
{
    double where = 0.0;
    if (node.host.rfind(folded, 0) == 0)
        where = kBookmarkHostPrefix;
    else if (node.foldedURL.rfind(folded, 0) == 0)
        where = kBookmarkURLPrefix;
    else if (node.foldedTitle.rfind(folded, 0) == 0)
        where = kBookmarkTitlePrefix;
    else if (node.host.find(folded) != std::string::npos)
        where = kBookmarkHostInside;
    else if (node.foldedURL.find(folded) != std::string::npos)
        where = kBookmarkURLInside;
    else if (node.foldedTitle.find(folded) != std::string::npos)
        where = kBookmarkTitleInside;
    else
        return 0.0;

    return where + kBookmarkWeight;
}

SuggestionRow* findRow(std::vector<SuggestionRow>& rows, const std::string& url)
{
    for (auto& row : rows) {
        if (row.url == url)
            return &row;
    }
    return nullptr;
}

} // namespace

std::vector<SuggestionRow> suggestionsLocked(const std::string& text, int limit)
{
    std::vector<SuggestionRow> rows;
    const std::string folded = foldCase(text);
    if (folded.empty())
        return rows;

    bookmarksEnsureRootsLocked();
    for (const BookmarkNode& node : bookmarkNodesLocked()) {
        if (node.folder || node.url.empty())
            continue;

        const double score = bookmarkScore(node, folded);
        if (score <= 0.0)
            continue;

        SuggestionRow row;
        row.kind = HB_SUGGESTION_BOOKMARK;
        row.node = node.id;
        row.url = node.url;
        row.title = node.title;
        row.host = node.host;
        row.score = score;
        rows.push_back(std::move(row));
    }

    const double now = unixNow();
    for (const HistoryEntry& entry : historyEntriesLocked()) {
        // An entry that answers the typed text nowhere is left out, rather than
        // ranked on how often it is read: a dropdown that fell back on that
        // would offer the browser's favourite sites whatever was typed.
        if (entry.host.find(folded) == std::string::npos
            && entry.foldedURL.find(folded) == std::string::npos
            && entry.foldedTitle.find(folded) == std::string::npos)
            continue;

        const double score = historyScore(entry, folded, HB_HISTORY_MATCH_SUBSTRING, now);
        if (SuggestionRow* existing = findRow(rows, entry.url)) {
            // The same address in both models is one row. It keeps the higher
            // rank and stays a bookmark: that is the more useful thing to know
            // about a row that is about to be pressed.
            existing->score = std::max(existing->score, score);
            existing->visitCount = entry.visitCount();
            existing->lastVisit = entry.lastVisit();
            if (existing->title.empty())
                existing->title = entry.title;
            continue;
        }

        SuggestionRow row;
        row.kind = HB_SUGGESTION_HISTORY;
        row.url = entry.url;
        row.title = entry.title;
        row.host = entry.host;
        row.visitCount = entry.visitCount();
        row.lastVisit = entry.lastVisit();
        row.score = score;
        rows.push_back(std::move(row));
    }

    std::sort(rows.begin(), rows.end(), [](const SuggestionRow& left, const SuggestionRow& right) {
        if (left.score != right.score)
            return left.score > right.score;
        if (left.lastVisit != right.lastVisit)
            return left.lastVisit > right.lastVisit;
        return left.url < right.url;
    });

    if (limit > 0 && rows.size() > static_cast<size_t>(limit))
        rows.resize(static_cast<size_t>(limit));
    return rows;
}

} // namespace harmony::bookmarks

// --- The host's frame thread asks from here -----------------------------------

using namespace harmony::bookmarks;

namespace {

// The answer to the last query this thread ran. Latched rather than recomputed
// per field so the six reads a row costs all belong to one ranking.
thread_local std::vector<SuggestionRow> t_rows;

const SuggestionRow* rowAt(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= t_rows.size())
        return nullptr;
    return &t_rows[static_cast<size_t>(index)];
}

} // namespace

extern "C" int hb_suggestions_query(const char* text, int limit)
{
    const std::string typed = text ? text : "";
    if (typed.empty()) {
        t_rows.clear();
        return 0;
    }

    std::lock_guard<std::mutex> lock(modelMutex());
    t_rows = suggestionsLocked(typed, limit);
    return static_cast<int>(t_rows.size());
}

extern "C" int hb_suggestion_kind(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? row->kind : HB_SUGGESTION_HISTORY;
}

extern "C" const char* hb_suggestion_url(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? answer(row->url) : "";
}

extern "C" const char* hb_suggestion_title(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? answer(row->title) : "";
}

extern "C" const char* hb_suggestion_host(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? answer(row->host) : "";
}

extern "C" int hb_suggestion_node(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? row->node : 0;
}

extern "C" int hb_suggestion_visit_count(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? row->visitCount : 0;
}

extern "C" double hb_suggestion_last_visit(int index)
{
    const SuggestionRow* row = rowAt(index);
    return row ? row->lastVisit : 0.0;
}
