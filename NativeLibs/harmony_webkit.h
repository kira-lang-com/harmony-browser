#ifndef HARMONY_WEBKIT_H
#define HARMONY_WEBKIT_H

#ifdef __cplusplus
extern "C" {
#endif

// WebKit runs on its own thread and owns every WebKit object on it. The host's
// frame thread never calls into WebKit: it starts the thread, posts commands,
// and moves the child windows asynchronously. Nothing here blocks on WebKit,
// which is what keeps a debug build of it off the host's frame time.
//
// Every function below is safe to call from the host's frame thread. Calls made
// before the thread finishes starting are queued, not dropped.

// Starts the WebKit thread if it is not running and opens the first tab.
// Returns immediately; non-zero once the thread is up and serving commands.
int hb_webkit_start(void* parent_window, const char* initial_url);

// Non-zero once the WebKit runtime loaded and the thread is serving commands.
int hb_webkit_supported(void);

// The rectangle the active tab's view should occupy, in the parent window's
// client pixels. Applied asynchronously; safe to call every frame.
void hb_webkit_set_bounds(int x, int y, int width, int height);

// Opens a tab and returns its id, or 0 when the id could not be allocated. The
// view behind the id is created on the WebKit thread shortly afterwards; the id
// is valid for every call below as soon as it is returned.
//
// Tabs do not share a web process: no tab is created as a related page, so each
// one gets its own. They do share the context's data store, so a login in one
// tab is a login in all of them, as in any other browser.
int hb_webkit_tab_open(const char* url);
void hb_webkit_tab_close(int tab_id);
void hb_webkit_tab_select(int tab_id);

// Shows the tab belonging to a host UI slot, opening it on first use. The url
// is that slot's home: loaded when the tab is created, ignored afterwards, so
// returning to a slot returns to wherever that tab had got to. This is the call
// a fixed row in the host's tab list should make.
void hb_webkit_tab_activate_slot(int slot, const char* url);
void hb_webkit_tab_close_slot(int slot);

// The tab list as the host last saw it. Index order is open order.
int hb_webkit_tab_count(void);
int hb_webkit_tab_id_at(int index);
int hb_webkit_tab_active(void);

// Navigation, applied to the active tab.
void hb_webkit_go_back(void);
void hb_webkit_go_forward(void);
void hb_webkit_reload(void);
void hb_webkit_load_url(const char* url);

// Stops the WebKit thread and destroys everything it owns.
void hb_webkit_shutdown(void);

// A process-local diagnostic string for startup failures. It remains valid
// until the next bridge call that changes the error.
const char* hb_webkit_error(void);

#ifdef __cplusplus
}
#endif

#endif
