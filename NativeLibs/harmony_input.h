#ifndef HARMONY_INPUT_H
#define HARMONY_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

// Keyboard and pointer routing for a browser whose page lives in a child window
// of another thread.
//
// The page's view is a native child window created on the engine thread. It
// takes the keyboard focus on the first click in it and keeps it: from that
// moment the host's own message pump sees no key at all, so a browser that
// binds its shortcuts in the widget layer has none once the user has touched a
// page. The chords below are therefore matched where the messages actually
// are -- in a message hook on each of the two pumps -- and are consumed before
// the widget tree or WebKit can act on them.
//
// What a chord DOES is not decided here. Everything a browser system already
// owns (loading, tabs, the address bar) is queued as an event for the host to
// drain and dispatch; only what nothing else owns (page zoom, find, print, and
// the focus itself) is carried out here.
//
// Threading. Everything below runs on the host's frame thread unless it says
// otherwise, and none of it blocks: work that must touch a WKPage is queued
// onto the engine thread through the tab registry's seam.

// --- Lifetime ---------------------------------------------------------------

// Registers this module with the tab registry and installs the hooks, once.
//
// It has to run before the registry creates its first page: a page made earlier
// carries a UI client whose focus fields this module never wrote into, and
// nothing afterwards can go back and fill them in. Call it from the host's frame
// thread, which is the thread whose message queue the chrome's hook goes on.
//
// `hb_input_frame` calls this too, so the module still attaches if the host
// reaches it first; whichever runs first is the one that attaches.
void hb_input_attach(void* host_window);

// Adopts `host_window` as the window the chrome draws in, installs the hooks,
// applies stored zoom to pages that have navigated, and keeps the focus model
// in step with the window that actually holds the keyboard. Idempotent: call it
// once per frame, before the widget tree is built.
void hb_input_frame(void* host_window);

// Removes the hooks, detaches the two input queues and writes the zoom levels
// out. Call before the tab registry is stopped: the engine thread's hook is
// removed from that thread.
void hb_input_shutdown(void);

// Non-zero once the hooks are installed on both pumps.
int hb_input_ready(void);

// The last failure, or "". This thread's copy, valid until this thread asks
// again.
const char* hb_input_error(void);

// Increments whenever anything the chrome draws from this module changes: an
// event queued, the focus moved, the zoom applied, a find result. A host that
// skips its widget build on an idle frame reads this to know it cannot.
int hb_input_revision(void);

// --- Events -----------------------------------------------------------------
//
// The queue the host drains each frame. `hb_input_next_event` pops one and
// answers its kind, or HB_INPUT_EVENT_NONE when the queue is empty; the two
// accessors describe the event that was just popped.

#define HB_INPUT_EVENT_NONE 0

// Accelerators the host dispatches into the systems that own them.
#define HB_INPUT_EVENT_FOCUS_ADDRESS 1
#define HB_INPUT_EVENT_NEW_TAB 2
#define HB_INPUT_EVENT_CLOSE_TAB 3
#define HB_INPUT_EVENT_REOPEN_TAB 4
#define HB_INPUT_EVENT_NEXT_TAB 5
#define HB_INPUT_EVENT_PREVIOUS_TAB 6
// argument: the tab's index in the strip, or -1 for the last tab.
#define HB_INPUT_EVENT_SELECT_TAB 7
#define HB_INPUT_EVENT_RELOAD 8
#define HB_INPUT_EVENT_RELOAD_IGNORING_CACHE 9
#define HB_INPUT_EVENT_BACK 10
#define HB_INPUT_EVENT_FORWARD 11
#define HB_INPUT_EVENT_STOP 12
#define HB_INPUT_EVENT_OPEN_FIND 13
#define HB_INPUT_EVENT_CLOSE_FIND 14
#define HB_INPUT_EVENT_FIND_NEXT 15
#define HB_INPUT_EVENT_FIND_PREVIOUS 16

// Editing routed to the chrome's keyboard target. `hb_input_event_target` says
// which field it belongs to, so a target that changed mid-frame cannot deliver
// a keystroke to the wrong one.
#define HB_INPUT_EVENT_TEXT 20 // argument: a Unicode scalar value
#define HB_INPUT_EVENT_BACKSPACE 21
#define HB_INPUT_EVENT_DELETE 22
#define HB_INPUT_EVENT_CARET_LEFT 23
#define HB_INPUT_EVENT_CARET_RIGHT 24
#define HB_INPUT_EVENT_CARET_HOME 25
#define HB_INPUT_EVENT_CARET_END 26
#define HB_INPUT_EVENT_SELECT_ALL 27
#define HB_INPUT_EVENT_SUBMIT 28
#define HB_INPUT_EVENT_CANCEL 29

int hb_input_next_event(void);
int hb_input_event_argument(void);
int hb_input_event_target(void);

// --- Focus ------------------------------------------------------------------

#define HB_INPUT_FOCUS_CHROME 0
#define HB_INPUT_FOCUS_CONTENT 1

#define HB_INPUT_TARGET_NONE 0
#define HB_INPUT_TARGET_ADDRESS 1
#define HB_INPUT_TARGET_FIND 2

// Which of the two windows holds the keyboard, read from the system rather than
// remembered: a page takes the focus from inside WebKit when it is clicked, and
// a copy kept here would disagree with the window that is receiving the keys.
int hb_input_focus_owner(void);

// Moves the keyboard. Focusing the chrome takes it off the page's window, which
// is what lets the address bar be typed into at all; focusing the content hands
// it back, so the page keeps its caret and its key handlers.
void hb_input_focus_chrome(void);
void hb_input_focus_content(void);

// Which chrome field the keyboard is routed to while the chrome holds it.
// Setting it to a field is what makes the hooks consume keys rather than leave
// them to the widget tree, so the field types even when the widget layer's own
// focus is on something else.
int hb_input_chrome_text_target(void);
void hb_input_set_chrome_text_target(int target);

// Tab and Shift+Tab: the next chrome field in `direction`, and the page once
// the fields run out. Also how WebKit's own tabbing leaves the page -- the page
// asks for the focus to be taken and this is what answers.
void hb_input_focus_cycle(int direction);

// Counts the pointer presses that have landed on the chrome's window.
//
// Which chrome field a press claimed is the widget layer's answer, and it is
// read out of the widget layer's own hit test. This is what says a press
// happened at all, including one that began and ended inside a single frame,
// which is the press the widget layer's latched snapshot cannot be asked about
// afterwards.
int hb_input_press_serial(void);

// --- Page zoom --------------------------------------------------------------
//
// Zoom belongs to the site rather than to the tab: a level set on one page of a
// site is the level every page of that site opens at, in this tab and in every
// other, now and after a restart. That is what a browser's zoom is, and it is
// why this is stored per origin rather than per page.

void hb_input_zoom_in(void);
void hb_input_zoom_out(void);
void hb_input_zoom_reset(void);

// A level chosen from a menu rather than stepped into, in percent.
void hb_input_zoom_set(int percent);

// The active tab's zoom, in percent, and the origin it is remembered under.
int hb_input_zoom_percent(void);
const char* hb_input_zoom_origin(void);

// --- Find in page -----------------------------------------------------------

// Opens and closes the find session. Closing takes WebKit's highlight down.
void hb_input_find_open(void);
void hb_input_find_close(void);
int hb_input_find_is_open(void);

// Searches for `text`, wrapping, case-insensitively, highlighting every match.
// An empty string takes the highlight down without closing the session.
void hb_input_find_search(const char* text, int backwards);

// What the last search found. The count is every match on the page; the index
// is which of them is selected, 1-based, or 0 before a match is selected.
int hb_input_find_match_count(void);
int hb_input_find_match_index(void);
int hb_input_find_searched(void);

// --- Printing ---------------------------------------------------------------

// Prints the active tab through the system print dialog.
//
// The page is spooled as it is composed on screen: this port's web process
// exposes no paginated print path -- the message that carries printed pages out
// of it is built only for Cocoa and GTK -- so what a printer can be given is the
// composed view, captured the same way a tab's stand-in frame is.
//
// The dialog runs on a thread of this module's own, so neither the frame thread
// nor the engine thread stops while it is up.
void hb_input_print(void);

#ifdef __cplusplus
}
#endif

#endif
