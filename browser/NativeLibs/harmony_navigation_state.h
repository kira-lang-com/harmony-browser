#ifndef HARMONY_NAVIGATION_STATE_H
#define HARMONY_NAVIGATION_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

// The navigation model of every open tab, published for the host's frame
// thread to read.
//
// WebKit answers questions about a page only on the thread that owns it, and
// only from inside its run loop. So the answers are taken where they are legal
// to take — in the navigation and state callbacks, on the WebKit thread — and
// written into a snapshot the frame thread reads under a lock. Every reader
// below is a lock, a copy, and an unlock: no reader enters WebKit, and no
// reader blocks on it.
//
// `tab_id` is the id the WebKit host gave the tab. Zero or negative names the
// active tab, so a host that only ever drives what the user is looking at
// never has to track an id.

// The tab the host last selected, or 0 when no tab is open.
int hb_nav_active_tab(void);

// The tab holding a host UI slot, or 0 when that slot has no tab yet.
int hb_nav_tab_for_slot(int slot);

// Every tab the model knows, in the order the tabs were attached.
int hb_nav_tab_count(void);
int hb_nav_tab_id_at(int index);

// The host UI slot a tab belongs to, or -1 for a tab opened without one.
int hb_nav_tab_slot(int tab_id);

// The URL the address bar should show: the one being loaded while a load is in
// flight, and the one that committed otherwise. Empty for an unknown tab.
const char* hb_nav_url(int tab_id);

// The URL of the document the page is currently showing.
const char* hb_nav_committed_url(int tab_id);

// The URL of the load in flight, empty when no load is in flight.
const char* hb_nav_provisional_url(int tab_id);

// The document's title, empty until the page supplies one.
const char* hb_nav_title(int tab_id);

// 0 to 1 across the load in flight. Holds its last value once the load ends.
double hb_nav_progress(int tab_id);

// Non-zero between the start of a provisional load and the navigation
// finishing or failing.
int hb_nav_is_loading(int tab_id);

int hb_nav_can_go_back(int tab_id);
int hb_nav_can_go_forward(int tab_id);

// The error the last navigation failed with: 0 when the last navigation
// succeeded or none has finished yet, WebKit's error code when one failed, and
// -1 when the page's web process ended under it. The text is WebKit's
// localized description, empty when there is no error.
int hb_nav_error_code(int tab_id);
const char* hb_nav_error_text(int tab_id);

// The back/forward list, oldest entry first, including the current one.
// `hb_nav_history_current` is the index of the current entry, or -1 when the
// tab has no history yet.
int hb_nav_history_count(int tab_id);
int hb_nav_history_current(int tab_id);
const char* hb_nav_history_url(int tab_id, int index);
const char* hb_nav_history_title(int tab_id, int index);

// Navigation, applied on the WebKit thread. The url must already be absolute:
// turning what a person typed into a URL is the host's decision, not this
// layer's, and it is made once where the host can show its result.
void hb_nav_navigate(int tab_id, const char* url);
void hb_nav_reload(int tab_id);
void hb_nav_reload_ignoring_cache(int tab_id);
void hb_nav_stop(int tab_id);
void hb_nav_go_back(int tab_id);
void hb_nav_go_forward(int tab_id);

// Goes to an entry of `hb_nav_history_*` by its index in that list.
void hb_nav_go_to_history_entry(int tab_id, int index);

// Why the model is not tracking anything, when it is not. Empty when it is.
const char* hb_nav_diagnostic(void);

#ifdef __cplusplus
}
#endif

#endif
