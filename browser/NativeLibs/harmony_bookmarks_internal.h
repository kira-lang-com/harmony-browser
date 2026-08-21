#ifndef HARMONY_BOOKMARKS_INTERNAL_H
#define HARMONY_BOOKMARKS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "harmony_bookmarks.h"
#include "harmony_paths.h"
#include "harmony_text.h"

// The model the four translation units of this module share.
//
// One mutex covers both the history and the bookmark tree, because the two are
// read together: a suggestion list ranks them against one another, and two locks
// taken in one query is two locks that can be taken in the other order somewhere
// else. Every function whose name ends in `Locked` expects it held; every C
// entry point takes it.
namespace harmony::bookmarks {

using harmony::text::narrow;
using harmony::text::widen;

using harmony::paths::makeDirectories;

// --- The models -------------------------------------------------------------

// One address the browser has been to, with every visit to it.
struct HistoryEntry {
    std::string url;
    std::string title;
    std::string host;

    // Ascending. The last is the most recent, which is what every read wants.
    std::vector<double> visits;

    // The address and the title case-folded once, when they are written. A
    // query runs on every keystroke and would otherwise fold the whole history
    // per letter typed.
    std::string foldedURL;
    std::string foldedTitle;

    double lastVisit() const { return visits.empty() ? 0.0 : visits.back(); }
    int visitCount() const { return static_cast<int>(visits.size()); }
};

// One node of the bookmark tree. A folder carries `children` in the order the
// user put them in; a bookmark carries a URL and no children.
struct BookmarkNode {
    int id = 0;
    int parent = 0;
    bool folder = false;
    std::string title;
    std::string url;
    std::string host;
    std::string foldedURL;
    std::string foldedTitle;
    double added = 0.0;
    std::vector<int> children;
};

// A row of a query's answer, copied out from under the lock so a reader never
// holds it across a return into Kira.
struct HistoryRow {
    std::string url;
    std::string title;
    std::string host;
    double lastVisit = 0.0;
    int visitCount = 0;
    int day = 0;
    double score = 0.0;
};

struct SuggestionRow {
    int kind = HB_SUGGESTION_HISTORY;
    int node = 0;
    std::string url;
    std::string title;
    std::string host;
    double lastVisit = 0.0;
    int visitCount = 0;
    double score = 0.0;
};

// --- Shared state -----------------------------------------------------------

std::mutex& modelMutex();

// Says that what is published has moved. Called with the lock held.
void bumpRevisionLocked();
int currentRevision();

void setError(const std::string& message);
void clearError();
std::string currentError();

// A string handed back to Kira. The pointer stays good until this thread has
// asked eight more questions: long enough for a caller to read it, short enough
// that nothing accumulates. Copying out is what lets a reader answer without
// holding the model lock across the return.
const char* answer(const std::string& value);

// --- Text -------------------------------------------------------------------

// ASCII case folding. A URL and a host are ASCII by the time WebKit hands them
// over, and folding a UTF-8 title beyond ASCII would need a case table this
// browser has no other use for; a title matches on its ASCII letters and on the
// exact bytes of everything else.
std::string foldCase(const std::string& text);

// The host an address is on, without the `www.` a site answers on: the tab strip
// spells hosts that way, and a history list beside it must not spell them
// differently.
std::string hostOfURL(const std::string& url);

// The address as a query matches it: case folded, without the scheme every page
// carries and without the `www.` a site answers on, so typing "git" reaches
// github.com at the front of its address rather than in the middle of
// "https://github.com". Every folded address in either model is made here, so a
// bookmark and a history entry for one page can never match differently.
std::string matchableURL(const std::string& url);

// Whether an address is one a history is allowed to remember. `about:`, `data:`,
// `blob:` and `javascript:` name a document that cannot be returned to, and a
// list of them is a list of rows that do nothing when pressed.
bool isRecordableURL(const std::string& url);

// Seconds since the Unix epoch.
double unixNow();
double unixTimeOf(const FILETIME& time);

// The local day a Unix time falls in, counted from 1970-01-01, and the calendar
// that day lands on. Local rather than UTC because a history groups its rows by
// the day the person had, not the day Greenwich had.
int localDayOf(double unixSeconds);
int localMinuteOfDay(double unixSeconds);
bool calendarOfDay(int day, int& year, int& month, int& dayOfMonth, int& weekday);

// --- History ----------------------------------------------------------------

// The most visits one address keeps. A page opened every minute by a script
// would otherwise grow one entry without bound, and the thousandth visit to it
// says nothing the hundredth did not.
constexpr size_t kMaxVisitsPerEntry = 128;
// How many addresses the history holds. At the cap the least recently visited
// go first, which is the only order in which a history is worth forgetting.
constexpr size_t kMaxHistoryEntries = 8192;

std::vector<HistoryEntry>& historyEntriesLocked();

// Records a visit, creating the address's entry when it is new. A title arriving
// empty leaves whatever title the entry already had: WebKit reports the commit
// before the document names itself, and an empty title written over a good one
// is a row that loses its name every time it is read again.
void historyRecordLocked(const std::string& url, const std::string& title, double when);

// Replaces the title of the most recent entry for an address. This is the report
// that arrives seconds after the commit.
void historyRetitleLocked(const std::string& url, const std::string& title);

std::vector<HistoryRow> historyQueryLocked(const std::string& text, int match, int limit);

void historyForgetURLLocked(const std::string& url);
void historyForgetHostLocked(const std::string& host);
void historyForgetSinceLocked(double since);

// How strongly an entry answers a query: its weight from how often and how
// recently it was read, times what the match itself is worth.
double historyScore(const HistoryEntry& entry, const std::string& folded, int match, double now);

// --- Bookmarks --------------------------------------------------------------

std::vector<BookmarkNode>& bookmarkNodesLocked();

BookmarkNode* bookmarkNodeLocked(int id);
// Creates the two roots when the tree is empty, so every caller below can
// assume they are there.
void bookmarksEnsureRootsLocked();

int bookmarksAddLocked(int parent, int index, const std::string& url, const std::string& title, bool folder);
void bookmarksRemoveLocked(int id);
void bookmarksMoveLocked(int id, int parent, int index);
int bookmarksNodeForURLLocked(const std::string& url);
int bookmarksDepthLocked(int id);
int bookmarksLeafCountLocked(int id);

// The id the next node created will take. Held here because the file reader
// writes it back after adopting the ids it read.
int& bookmarksNextIdLocked();

// --- Suggestions ------------------------------------------------------------

std::vector<SuggestionRow> suggestionsLocked(const std::string& text, int limit);

// --- The file format --------------------------------------------------------
//
// One record per line, one field per tab. Both models are written whole rather
// than appended to, because both are edited in the middle: a visit deleted from
// the history and a bookmark moved between folders are the ordinary cases, and a
// log would have to be replayed to answer either.

std::string encodeHistoryLocked();
void decodeHistoryLocked(const std::string& contents);

std::string encodeBookmarksLocked();
void decodeBookmarksLocked(const std::string& contents);

// --- Files ------------------------------------------------------------------
//
// The profile root is the data store's, asked for rather than recomputed: a
// browser with two state directories is a browser that clears one of them. It is
// empty until that module has prepared its layout, so the load is retried from
// the run-loop cycle until it answers.

// Reads both files if the profile root is available and they have not been read.
// Answers true once the model is serving, whether or not either file existed.
bool filesLoad();
bool filesReady();

// Marks a model as differing from what is on disk. The write happens from the
// run-loop cycle once the change has settled, so a folder dragged across a list
// is written once rather than once per frame it moved through.
void markHistoryDirtyLocked();
void markBookmarksDirtyLocked();

// Engine thread. Writes whichever model has settled.
void filesPump();
// Any thread. Writes both models now, whatever they have settled to.
void filesFlush();
void filesStop();

} // namespace harmony::bookmarks

#endif // HARMONY_BOOKMARKS_INTERNAL_H
