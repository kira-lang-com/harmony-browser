#ifndef HARMONY_TABS_H
#define HARMONY_TABS_H

#ifdef __cplusplus
extern "C" {
#endif

// The browser's tab registry. It owns the WebKit runtime, the thread WebKit
// runs on, the process context every tab shares, and one WKView -- one native
// child window -- per tab inside the host's single window. Exactly one of those
// children is visible at a time; the rest are hidden and told they are out of
// the window, so a background tab holds its state without rendering.
//
// The registry is the source of truth for which tabs exist and in what order,
// because tabs are not only opened by the host: a page that calls window.open
// or follows a target=_blank link creates one from inside WebKit, and a model
// kept above this layer could not see that happen. The host reads the list back
// through the accessors below and draws it.
//
// Every function here is safe to call from the host's frame thread and none of
// them block on WebKit. Calls made before the WebKit thread finishes starting
// are queued, not dropped.

// --- Engine lifetime --------------------------------------------------------

// Starts the WebKit thread if it is not running, remembers `parent_window` as
// the window every tab's view is a child of, and opens the first tab on
// `home_url`. Idempotent: later calls only refresh the parent window. Returns
// non-zero once the thread is up and serving commands.
int hb_tabs_start(void* parent_window, const char* home_url);

// Non-zero once the WebKit runtime loaded and the thread is serving commands.
int hb_tabs_ready(void);

// The address a tab opened with no URL of its own loads.
void hb_tabs_set_home(const char* url);

// Stops the WebKit thread and destroys every tab and everything WebKit owns.
void hb_tabs_shutdown(void);

// The last startup or tab-creation failure, or "" when there has been none.
// The returned pointer is this thread's copy and is valid until this thread
// calls this function again.
const char* hb_tabs_error(void);

// --- Geometry ---------------------------------------------------------------

// The rectangle the active tab's view occupies, in the parent window's client
// pixels. Applied asynchronously; safe to call every frame.
void hb_tabs_set_bounds(int x, int y, int width, int height);

// The host's device-pixel ratio, applied to every tab's page. The host owns
// this number: the chrome around the view and the page inside it have to be
// laid out at one scale or the page reads a third larger than the window it
// sits in.
void hb_tabs_set_backing_scale(double scale);

// --- Opening and closing ----------------------------------------------------

// Opens a tab at the end of the list and returns its id, which is valid for
// every call below as soon as it is returned -- the view behind it is created
// on the WebKit thread shortly afterwards. A NULL or empty `url` opens the home
// address. `foreground` non-zero selects the new tab.
int hb_tabs_open(const char* url, int foreground);

// The same, inserted at `index`. A negative index, or one past the end,
// appends. An index inside the pinned prefix is clamped to the first unpinned
// position, because pinned tabs are a prefix and an unpinned tab cannot sit
// among them; see hb_tabs_set_pinned.
int hb_tabs_open_at(const char* url, int index, int foreground);

// Closes one tab, every tab but one, or every tab. Closing the active tab
// selects its right-hand neighbour, or its left-hand one when it was last.
void hb_tabs_close(int tab_id);
void hb_tabs_close_others(int tab_id);
void hb_tabs_close_all(void);

// Reopens the most recently closed tab at the position it was closed from,
// with the address and the label it was showing. Returns the id it will answer
// to, or 0 when nothing has been closed. Reopened tabs come back in the reverse
// of the order they went.
int hb_tabs_reopen_closed(void);

// How many closed tabs are still on the reopen stack.
int hb_tabs_closed_count(void);

// --- Selection --------------------------------------------------------------

void hb_tabs_select(int tab_id);
void hb_tabs_select_index(int index);

// Wrap around the ends of the list, so the shortcut a browser binds to these
// cycles rather than stopping.
void hb_tabs_select_next(void);
void hb_tabs_select_previous(void);

// --- Ordering ---------------------------------------------------------------

// Moves a tab to `to_index`, clamped into its own pin group: a pinned tab can
// only be reordered among pinned tabs and an unpinned one among unpinned tabs,
// which is what keeps the pinned prefix a prefix.
void hb_tabs_move(int tab_id, int to_index);

// Pinning moves the tab to the end of the pinned prefix; unpinning moves it to
// the front of the unpinned suffix.
void hb_tabs_set_pinned(int tab_id, int pinned);

// --- Navigation, per tab ----------------------------------------------------

// `url` is normalized the way an address bar normalizes: about: and anything
// carrying a scheme is loaded as given, a bare host gets https://, and anything
// else becomes a search.
void hb_tabs_load(int tab_id, const char* url);
void hb_tabs_go_back(int tab_id);
void hb_tabs_go_forward(int tab_id);
void hb_tabs_reload(int tab_id);
void hb_tabs_stop(int tab_id);

// --- The published list -----------------------------------------------------
//
// A copy of the registry the host can read from its own thread without waiting
// on WebKit. It is republished whenever the list, the order, the selection or
// any tab's title, address or load state changes.

int hb_tabs_count(void);

// Increments on every republication. A host that caches anything derived from
// the list holds this number beside it to know when the cache is stale.
int hb_tabs_revision(void);

int hb_tabs_id_at(int index);
int hb_tabs_index_of(int tab_id);
int hb_tabs_active_id(void);
int hb_tabs_active_index(void);

// Text accessors return this thread's copy, valid until this thread calls the
// same accessor again. An unknown id gives "".
const char* hb_tabs_title(int tab_id);
const char* hb_tabs_url(int tab_id);
const char* hb_tabs_host(int tab_id);

int hb_tabs_is_loading(int tab_id);
int hb_tabs_is_pinned(int tab_id);
int hb_tabs_can_go_back(int tab_id);
int hb_tabs_can_go_forward(int tab_id);

// 0 to 1. A page that has finished reads 1.
double hb_tabs_progress(int tab_id);

#ifdef __cplusplus
}
#endif

#endif
