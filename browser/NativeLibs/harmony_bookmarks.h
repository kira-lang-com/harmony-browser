#ifndef HARMONY_BOOKMARKS_H
#define HARMONY_BOOKMARKS_H

#ifdef __cplusplus
extern "C" {
#endif

// Where the browser has been, and what it was told to keep.
//
// Two models live here because they are read together and never apart: the
// address bar's dropdown ranks history and bookmarks against one typed prefix,
// and a star that lit for a page the history has never recorded would be a star
// lying about a page. One module, one lock, one file pair, one revision.
//
// Both are rooted in the profile the data store owns -- %LOCALAPPDATA%\
// HarmonyBrowser -- because a browser with two state directories is a browser
// that clears one of them. The path is asked of that module rather than
// recomputed here.
//
// Threading. Recording happens on the WebKit thread, from the registry's
// page-state observer; everything below is called from the host's frame thread
// and answers from the model behind its own lock. Nothing here enters WebKit and
// nothing here touches disk: the writes are encoded under the lock and handed to
// a worker of this module's own.
//
// A private tab records nothing. Which tabs those are is the profile's answer,
// asked at the moment a navigation commits.

// --- Lifecycle --------------------------------------------------------------

// Non-zero once both files have been read -- or found absent, which is the same
// answer for a profile that has never been browsed in.
int hb_bookmarks_ready(void);

// The last failure, or "". The returned pointer is this thread's copy and is
// valid until this thread asks again.
const char* hb_bookmarks_error(void);

// Seconds since the Unix epoch, on the clock every time below is stamped with.
// A row saying how long ago a page was read needs both ends of that subtraction
// to come from one place.
double hb_bookmarks_now(void);

// Bumped on every change either model makes. A host that skips its widget build
// on a frame with no input holds this number to know when it may not.
int hb_bookmarks_revision(void);

// --- History ----------------------------------------------------------------
//
// One entry per address, carrying every visit to it. Visits are kept rather than
// counted because a range-scoped removal deletes visits and not addresses: an
// address read yesterday and again last year survives "clear the last 24 hours"
// with one visit fewer, which is what the user asked for.

#define HB_HISTORY_MATCH_PREFIX 0
#define HB_HISTORY_MATCH_SUBSTRING 1

// Runs a query and latches its answer for this thread, returning how many rows
// it holds. `text` is matched case-insensitively against the address and the
// title; empty text matches everything. Rows come back most-recent-first with
// visit-count weighting -- see `hb_history_score`. `limit` of zero or less means
// every match.
//
// The accessors below read the latched answer, so a row's fields stay consistent
// even while the WebKit thread records a visit mid-frame.
int hb_history_query(const char* text, int match, int limit);

const char* hb_history_url(int index);
const char* hb_history_title(int index);
const char* hb_history_host(int index);
// Seconds since the Unix epoch.
double hb_history_last_visit(int index);
int hb_history_visit_count(int index);
// The local-time day the last visit fell in, counted from 1970-01-01. This is
// what a history list groups its rows by; two visits either side of midnight
// belong to different days whatever the hour between them was.
int hb_history_day(int index);
// The rank the query gave the row, so a caller can tell a strong match from a
// weak one without recomputing the weighting.
double hb_history_score(int index);

// How many addresses the history holds, whatever the last query matched.
int hb_history_count(void);
// How many visits it holds across all of them.
int hb_history_visit_total(void);

// The calendar a local day number falls on. The names of the months and the
// weekdays are the interface's, so only the numbers cross this boundary.
// `weekday` is 0 for Sunday.
int hb_history_today(void);
int hb_history_day_year(int day);
int hb_history_day_month(int day);
int hb_history_day_of_month(int day);
int hb_history_day_weekday(int day);

// Minutes since local midnight for a Unix time. A local offset is not always a
// whole number of hours and moves twice a year, so the platform is asked rather
// than the interface subtracting one it assumed.
int hb_history_minute_of_day(double unix_seconds);

// --- Forgetting -------------------------------------------------------------

// One address, with every visit to it.
void hb_history_forget_url(const char* url);
// Every address on one host, which is what "forget this site" means to a person
// looking at a list of sites.
void hb_history_forget_host(const char* host);
// Every visit at or after `since`, in Unix seconds, dropping an address once no
// visit to it is left. A `since` of zero or less clears the history whole.
void hb_history_forget_since(double since);

// --- Bookmarks --------------------------------------------------------------
//
// A tree of folders and bookmarks with the order the user put them in. The two
// roots always exist and cannot be removed or moved: the bar under the toolbar
// is a place on screen, and a browser that let it be deleted would have a strip
// with nothing to draw and no way to get it back.

#define HB_BOOKMARK_ROOT_BAR 1
#define HB_BOOKMARK_ROOT_OTHER 2

int hb_bookmarks_child_count(int folder_id);
// The id of a folder's nth child, or 0 past the end.
int hb_bookmarks_child_at(int folder_id, int index);
int hb_bookmarks_parent_of(int node_id);
// The node's position among its parent's children, or -1 when it is not there.
int hb_bookmarks_index_of(int node_id);
int hb_bookmarks_is_folder(int node_id);
// How many folders stand between the node and its root. A root is 0.
int hb_bookmarks_depth(int node_id);
// How many bookmarks the subtree under a folder holds, folders excluded.
int hb_bookmarks_leaf_count(int folder_id);

const char* hb_bookmarks_title(int node_id);
const char* hb_bookmarks_url(int node_id);
const char* hb_bookmarks_host(int node_id);
double hb_bookmarks_added(int node_id);

// The node bookmarking an address, or 0. This is what the toolbar's star reads.
int hb_bookmarks_node_for_url(const char* url);

// Creates a bookmark or a folder and returns its id, or 0 when the parent is
// not a folder. A negative index, or one past the end, appends.
int hb_bookmarks_add(int parent_id, int index, const char* url, const char* title);
int hb_bookmarks_add_folder(int parent_id, int index, const char* title);

// Removes a node and everything under it. A root is left alone.
void hb_bookmarks_remove(int node_id);
void hb_bookmarks_rename(int node_id, const char* title);
void hb_bookmarks_set_url(int node_id, const char* url);
// Moves a node under `parent_id` at `index`. A move into the node's own subtree
// is refused: a tree that can be made to contain itself is a tree that cannot be
// walked.
void hb_bookmarks_move(int node_id, int parent_id, int index);

// Bookmarks the page onto the bar when it is not bookmarked, and removes every
// node for it when it is. Returns non-zero when the page is bookmarked
// afterwards, which is what the star draws.
int hb_bookmarks_toggle(const char* url, const char* title);

// --- Suggestions ------------------------------------------------------------
//
// What the address bar offers while a person types: the bookmarks and the
// history that match the prefix, ranked against one another and capped.
//
// What the TYPED text itself means -- a place or a question -- is not decided
// here. That decision is the navigation system's, made once, where its result is
// what the bar then shows; a second copy of it here could offer a row that loads
// something else.

#define HB_SUGGESTION_BOOKMARK 1
#define HB_SUGGESTION_HISTORY 2

// Runs the suggestion query and latches its answer for this thread, returning
// how many rows it holds. Empty text answers with nothing: a dropdown over an
// empty address bar is a dropdown covering the page for no reason.
int hb_suggestions_query(const char* text, int limit);

int hb_suggestion_kind(int index);
const char* hb_suggestion_url(int index);
const char* hb_suggestion_title(int index);
const char* hb_suggestion_host(int index);
// The bookmark this row came from, or 0 for a history row.
int hb_suggestion_node(int index);
int hb_suggestion_visit_count(int index);
double hb_suggestion_last_visit(int index);

#ifdef __cplusplus
}
#endif

#endif
