#ifndef HARMONY_SETTINGS_H
#define HARMONY_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

// The browser's preferences: what they are, where they are kept, and what they
// do to a page.
//
// Everything a person can change about how this browser behaves lives here and
// nowhere else. A value another system needs -- the address a new tab opens, the
// search a phrase is answered with, the folder a download lands in, the level a
// site with no remembered zoom is read at -- is read back through this surface
// rather than written a second time where it is used.
//
// The file is `Settings.txt` in the profile the data store lays out, so a
// browser has one directory on disk and not two.
//
// Threading. Every function below is safe from any thread and none of them
// blocks: reads answer from a copy behind this module's own lock, writes take
// that lock and mark the file dirty, and the WebKit calls a change implies are
// queued onto the engine thread.
//
// ORDER. `hb_settings_frame` must be reached before the engine is asked to
// start, on the browser's frame thread. Its first call is what reads the file
// off disk, and the profile layout it shares with the data store is prepared
// there -- once, on one thread, before the engine thread that would otherwise
// prepare it concurrently exists.

// --- Startup behaviour -------------------------------------------------------

#define HB_SETTINGS_STARTUP_NEW_TAB 0
#define HB_SETTINGS_STARTUP_RESTORE_SESSION 1

// --- Lifetime ----------------------------------------------------------------

// Reads the settings on the first call, keeps the engine in step with them on
// every call, and enforces the startup behaviour before the session is restored.
// `host_window` is the window a folder picker is raised over. Idempotent and
// safe to call every frame.
void hb_settings_frame(void* host_window);

// Writes anything still dirty and stops the folder picker's thread. Call before
// the tabs registry is stopped: the preference commands this module queues run
// on the engine thread.
void hb_settings_shutdown(void);

// Bumped whenever any value changes. A host that skips its widget build on an
// idle frame compares this against what it last drew.
int hb_settings_revision(void);

// The last failure, or "" when there has been none. This thread's copy, valid
// until this thread asks again.
const char* hb_settings_error(void);

// Where the settings file is, for a panel that wants to say so. "" while the
// profile directory could not be reached, which is also when nothing can be
// saved.
const char* hb_settings_path(void);

// --- Home page ---------------------------------------------------------------

// The address a new tab and the home button go to. Never empty: a home that was
// cleared is the browser's own default rather than a blank command.
const char* hb_settings_home_url(void);
void hb_settings_set_home_url(const char* url);

// --- Startup -----------------------------------------------------------------

int hb_settings_startup(void);
void hb_settings_set_startup(int behaviour);

// --- Search engines ----------------------------------------------------------
//
// A list, with one of them chosen. Each engine is a name and a query template
// holding `{searchTerms}` where the typed phrase goes -- the OpenSearch
// spelling, because an engine whose query carries a suffix cannot be written as
// a prefix and a browser that stored prefixes could never hold one.

int hb_settings_engine_count(void);
const char* hb_settings_engine_name(int index);
const char* hb_settings_engine_query(int index);

int hb_settings_engine_default(void);
void hb_settings_set_engine_default(int index);

// Appends an engine and returns its index, or -1 when the name is empty or the
// query holds no `{searchTerms}` -- a template that cannot carry the phrase is
// not a search engine.
int hb_settings_add_engine(const char* name, const char* query);

// Rewrites one engine in place, on the same terms. A refused edit leaves the
// engine as it was.
void hb_settings_update_engine(int index, const char* name, const char* query);

// Removes one engine. The last engine is never removed, because a browser with
// no search engine cannot answer a typed phrase at all.
void hb_settings_remove_engine(int index);

// The URL a typed phrase loads: the chosen engine's template with the phrase
// percent-encoded into it. This is the one place that substitution happens.
// This thread's copy, valid until this thread asks again.
const char* hb_settings_search_url(const char* query);

// --- Zoom --------------------------------------------------------------------

// The level a site with no remembered zoom is read at, in percent. Applied to
// every page whose zoom is still the engine's own, so a per-site level always
// wins over it.
int hb_settings_default_zoom_percent(void);
void hb_settings_set_default_zoom_percent(int percent);

// --- What a page may do ------------------------------------------------------
//
// Each of these is a WKPreferences flag, applied to every open page as soon as
// it changes and to every page created afterwards.

int hb_settings_javascript_enabled(void);
void hb_settings_set_javascript_enabled(int enabled);

int hb_settings_images_enabled(void);
void hb_settings_set_images_enabled(int enabled);

// Whether a script may open a window without a click behind it.
int hb_settings_popups_enabled(void);
void hb_settings_set_popups_enabled(int enabled);

// Whether pages are told this browser does not want to be tracked. A page reads
// it as `navigator.doNotTrack`, which is set on every document loaded after the
// preference is turned on.
int hb_settings_do_not_track(void);
void hb_settings_set_do_not_track(int enabled);

// --- Downloads ---------------------------------------------------------------

// Where downloads are saved. "" means the folder the shell nominates, which is
// what a browser that has never been told otherwise uses.
const char* hb_settings_download_directory(void);
void hb_settings_set_download_directory(const char* path);

// Raises the Windows folder picker on a thread of its own and stores what was
// chosen. Returns immediately; `hb_settings_download_directory_pending` is
// non-zero while the picker is up.
void hb_settings_choose_download_directory(void);
int hb_settings_download_directory_pending(void);

#ifdef __cplusplus
}
#endif

#endif
