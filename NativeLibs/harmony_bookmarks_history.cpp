#include "harmony_bookmarks_internal.h"

#include <algorithm>
#include <cmath>

// The history: one entry per address, carrying every visit to it.
//
// Visits are kept rather than counted because a range-scoped removal deletes
// visits and not addresses. An address read yesterday and again last year has to
// survive "clear the last 24 hours" with one visit fewer and its older visit
// intact, and a counter cannot be asked which of its increments to undo.
//
// Everything here runs under the model lock. The recording half is called from
// the WebKit thread and the query half from the host's frame thread, and the two
// touch the same vector.

namespace harmony::bookmarks {

namespace {

std::vector<HistoryEntry> g_entries;

// How much of a query's rank comes from where it matched rather than from how
// often the address is read. A host that begins with what was typed is what the
// person almost certainly means, and it outranks a page read twice as often
// whose title merely contains the same letters.
constexpr double kMatchHostPrefix = 120.0;
constexpr double kMatchURLPrefix = 90.0;
constexpr double kMatchTitlePrefix = 60.0;
constexpr double kMatchHostInside = 30.0;
constexpr double kMatchURLInside = 14.0;
constexpr double kMatchTitleInside = 10.0;

// How fast an address falls out of the ranking once it stops being read. A
// fortnight is the point at which a page read once is worth less than a page
// read twice: shorter and yesterday's tab buries a site of years, longer and a
// site of years buries what was read this morning.
constexpr double kRecencyHalfLifeDays = 14.0;

HistoryEntry* findLocked(const std::string& url)
{
    for (auto& entry : g_entries) {
        if (entry.url == url)
            return &entry;
    }
    return nullptr;
}

// Drops the addresses nobody has been back to. The cap is on entries rather than
// on visits because an entry is what a list draws and what a query walks.
void trimLocked()
{
    if (g_entries.size() <= kMaxHistoryEntries)
        return;

    std::sort(g_entries.begin(), g_entries.end(), [](const HistoryEntry& left, const HistoryEntry& right) {
        return left.lastVisit() > right.lastVisit();
    });
    g_entries.resize(kMaxHistoryEntries);
}

void dropEmptyLocked()
{
    g_entries.erase(
        std::remove_if(g_entries.begin(), g_entries.end(), [](const HistoryEntry& entry) {
            return entry.visits.empty();
        }),
        g_entries.end()
    );
}

HistoryRow rowOf(const HistoryEntry& entry, double score)
{
    HistoryRow row;
    row.url = entry.url;
    row.title = entry.title;
    row.host = entry.host;
    row.lastVisit = entry.lastVisit();
    row.visitCount = entry.visitCount();
    row.day = localDayOf(row.lastVisit);
    row.score = score;
    return row;
}

} // namespace

std::vector<HistoryEntry>& historyEntriesLocked()
{
    return g_entries;
}

// --- Recording --------------------------------------------------------------

void historyRecordLocked(const std::string& url, const std::string& title, double when)
{
    if (!isRecordableURL(url))
        return;

    const double at = when > 0.0 ? when : unixNow();

    HistoryEntry* entry = findLocked(url);
    if (!entry) {
        HistoryEntry created;
        created.url = url;
        created.host = hostOfURL(url);
        created.foldedURL = matchableURL(url);
        g_entries.push_back(std::move(created));
        entry = &g_entries.back();
    }

    if (!title.empty() && title != entry->title) {
        entry->title = title;
        entry->foldedTitle = foldCase(title);
    }

    entry->visits.push_back(at);
    // A page that reloads itself is one page, not one visit per second of it.
    // Anything inside the window is folded onto the visit already recorded.
    constexpr double kSameVisitSeconds = 2.0;
    if (entry->visits.size() > 1 && at - entry->visits[entry->visits.size() - 2] < kSameVisitSeconds) {
        entry->visits.pop_back();
        entry->visits.back() = at;
    }

    if (entry->visits.size() > kMaxVisitsPerEntry) {
        entry->visits.erase(
            entry->visits.begin(),
            entry->visits.begin() + static_cast<ptrdiff_t>(entry->visits.size() - kMaxVisitsPerEntry)
        );
    }

    trimLocked();
    markHistoryDirtyLocked();
    bumpRevisionLocked();
}

void historyRetitleLocked(const std::string& url, const std::string& title)
{
    if (title.empty())
        return;

    HistoryEntry* entry = findLocked(url);
    if (!entry || entry->title == title)
        return;

    entry->title = title;
    entry->foldedTitle = foldCase(title);
    markHistoryDirtyLocked();
    bumpRevisionLocked();
}

// --- Ranking ----------------------------------------------------------------

double historyScore(const HistoryEntry& entry, const std::string& folded, int match, double now)
{
    const double last = entry.lastVisit();
    if (last <= 0.0)
        return 0.0;

    // How often, on a curve rather than a line: the tenth visit says far less
    // about a page than the second did.
    const double weight = 1.0 + 3.0 * std::log(1.0 + static_cast<double>(entry.visitCount()));

    // How recently, halving every fortnight and never falling to nothing: a site
    // of years is worth less than this morning's tab and more than a page that
    // was never read at all.
    const double ageDays = std::max(0.0, (now - last) / 86400.0);
    const double recency = 0.15 + 0.85 * std::exp(-ageDays / kRecencyHalfLifeDays);

    double where = 0.0;
    if (!folded.empty()) {
        if (entry.host.rfind(folded, 0) == 0)
            where = kMatchHostPrefix;
        else if (entry.foldedURL.rfind(folded, 0) == 0)
            where = kMatchURLPrefix;
        else if (entry.foldedTitle.rfind(folded, 0) == 0)
            where = kMatchTitlePrefix;
        else if (match == HB_HISTORY_MATCH_SUBSTRING) {
            if (entry.host.find(folded) != std::string::npos)
                where = kMatchHostInside;
            else if (entry.foldedURL.find(folded) != std::string::npos)
                where = kMatchURLInside;
            else if (entry.foldedTitle.find(folded) != std::string::npos)
                where = kMatchTitleInside;
        }
    }

    return where + weight * recency * 10.0;
}

namespace {

// Whether an entry answers the query at all. A prefix query is answered by the
// host, the address or the title STARTING with what was typed; a substring query
// by any of the three containing it.
bool matchesLocked(const HistoryEntry& entry, const std::string& folded, int match)
{
    if (folded.empty())
        return true;

    if (entry.host.rfind(folded, 0) == 0)
        return true;
    if (entry.foldedURL.rfind(folded, 0) == 0)
        return true;
    if (entry.foldedTitle.rfind(folded, 0) == 0)
        return true;
    if (match != HB_HISTORY_MATCH_SUBSTRING)
        return false;

    if (entry.host.find(folded) != std::string::npos)
        return true;
    if (entry.foldedURL.find(folded) != std::string::npos)
        return true;
    return entry.foldedTitle.find(folded) != std::string::npos;
}

} // namespace

std::vector<HistoryRow> historyQueryLocked(const std::string& text, int match, int limit)
{
    const std::string folded = foldCase(text);
    const double now = unixNow();

    std::vector<HistoryRow> rows;
    rows.reserve(g_entries.size());
    for (const HistoryEntry& entry : g_entries) {
        if (!matchesLocked(entry, folded, match))
            continue;
        rows.push_back(rowOf(entry, historyScore(entry, folded, match, now)));
    }

    // Most recent first, weighted by how often the address is read. Two rows of
    // equal rank are ordered by their last visit, so a list never reorders
    // itself between two frames that saw the same history.
    std::sort(rows.begin(), rows.end(), [](const HistoryRow& left, const HistoryRow& right) {
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

// --- Forgetting -------------------------------------------------------------

void historyForgetURLLocked(const std::string& url)
{
    const size_t before = g_entries.size();
    g_entries.erase(
        std::remove_if(g_entries.begin(), g_entries.end(), [&url](const HistoryEntry& entry) {
            return entry.url == url;
        }),
        g_entries.end()
    );
    if (g_entries.size() == before)
        return;

    markHistoryDirtyLocked();
    bumpRevisionLocked();
}

void historyForgetHostLocked(const std::string& host)
{
    const std::string wanted = foldCase(host);
    const size_t before = g_entries.size();
    g_entries.erase(
        std::remove_if(g_entries.begin(), g_entries.end(), [&wanted](const HistoryEntry& entry) {
            return entry.host == wanted;
        }),
        g_entries.end()
    );
    if (g_entries.size() == before)
        return;

    markHistoryDirtyLocked();
    bumpRevisionLocked();
}

void historyForgetSinceLocked(double since)
{
    if (since <= 0.0) {
        if (g_entries.empty())
            return;
        g_entries.clear();
        markHistoryDirtyLocked();
        bumpRevisionLocked();
        return;
    }

    bool changed = false;
    for (HistoryEntry& entry : g_entries) {
        const auto first = std::lower_bound(entry.visits.begin(), entry.visits.end(), since);
        if (first == entry.visits.end())
            continue;
        entry.visits.erase(first, entry.visits.end());
        changed = true;
    }
    if (!changed)
        return;

    // An address every visit to which was inside the range is an address the
    // browser has not been to, and a row with no visit behind it would be a row
    // that cannot say when it was read.
    dropEmptyLocked();
    markHistoryDirtyLocked();
    bumpRevisionLocked();
}

} // namespace harmony::bookmarks
