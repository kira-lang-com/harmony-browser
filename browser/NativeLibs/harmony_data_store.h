#ifndef HARMONY_DATA_STORE_H
#define HARMONY_DATA_STORE_H

#ifdef __cplusplus
extern "C" {
#endif

// The browser profile: where WebKit keeps cookies, caches and per-origin
// storage, which of those a user may inspect and delete, which tabs browse
// outside all of it, and the open tabs and their history across a restart.
//
// The profile is rooted at %LOCALAPPDATA%\HarmonyBrowser and every WebKit
// storage location is set into it, so nothing this browser writes lands in a
// directory shared with another WebKit embedder. Owning the whole tree is what
// lets per-origin and time-scoped removal be exact: WebKit's C API removes a
// data type for all time and never for a range, so a range is served by the
// profile deleting its own files under a cutoff, and it can only do that for
// files it put there.
//
// Threading. Functions marked "engine thread" touch WebKit objects and must run
// on the thread that cycles WebKit's run loop. Everything else is safe from the
// host's frame thread: requests are queued and serviced from the tab registry's
// cycle hook, disk work runs on a worker of this module's own, and results are
// published as a snapshot the frame thread polls.

// --- Lifecycle --------------------------------------------------------------

// Engine thread. Creates the profile directories, reads the session written by
// the last run, and creates the persistent store. Idempotent; returns non-zero
// once the persistent store exists.
//
// Call it from the tabs registry's createContext, before the context is
// created: the context configuration is where the profile's process policy is
// set, and the first page is created moments later.
int hb_data_store_prepare(void);

// Non-zero once the persistent store exists and the profile is serving.
int hb_data_store_ready(void);

// Frame thread. Writes the origin ledger and forces a last session write. Call
// it before the tabs registry is stopped: capturing a tab's history needs the
// tab's page, and the registry destroys every page on its way down.
//
// Nothing else has to be scheduled. Preparing the profile registers a cycle
// hook and a teardown hook on the registry, so the queued requests, the removal
// in flight, the origin ledger, the session writer and the release of the
// stores are all driven from the WebKit thread's own schedule.
void hb_data_store_shutdown(void);

// The WKWebsiteDataStoreRef backing normal and private browsing. Borrowed: the
// module owns the reference. The ephemeral store is created on first use and
// released again when the last private tab closes, so a private session leaves
// nothing behind even in memory.
void* hb_data_store_persistent(void);
void* hb_data_store_ephemeral(void);

// Engine thread. Binds a store to the configuration a tab's page is about to be
// created from, and records which store that tab browses in.
//
// The tab is private when hb_data_store_open_private_tab asked for it, or when
// the configuration already carries the ephemeral store -- which is how a page
// opened by window.open from a private tab stays private.
void hb_data_store_apply_to_page_configuration(void* page_configuration, int tab_id);

// Engine thread. Profile-scoped process policy on a WKContextConfigurationRef.
void hb_data_store_apply_to_context_configuration(void* context_configuration);

// The profile root, UTF-8. Valid for the life of the process.
const char* hb_data_store_profile_path(void);

// The last failure, or an empty string. Cleared by the next successful step.
// The returned pointer is this thread's copy, valid until this thread asks
// again.
const char* hb_data_store_error(void);

// --- Private browsing -------------------------------------------------------

// Frame thread. Opens a tab in the ephemeral store and returns its id, or 0 when
// the registry could not allocate one. A null or empty url opens the home
// address.
int hb_data_store_open_private_tab(const char* url);

// Non-zero when the tab browses in the ephemeral store.
int hb_data_store_tab_is_private(int tab_id);

// How many private tabs are open. Zero means the ephemeral store has been
// released and the next private tab starts a fresh private session.
int hb_data_store_private_tab_count(void);

// --- Data types and time ranges ---------------------------------------------

#define HB_DATA_TYPE_COOKIES 1
#define HB_DATA_TYPE_CACHE 2
#define HB_DATA_TYPE_LOCAL_STORAGE 4
#define HB_DATA_TYPE_INDEXED_DB 8
#define HB_DATA_TYPE_SERVICE_WORKERS 16
#define HB_DATA_TYPE_DOM_CACHE 32
#define HB_DATA_TYPE_MEDIA_KEYS 64
#define HB_DATA_TYPE_TRACKING 128
#define HB_DATA_TYPE_ALL 255

#define HB_DATA_RANGE_LAST_HOUR 0
#define HB_DATA_RANGE_LAST_DAY 1
#define HB_DATA_RANGE_LAST_WEEK 2
#define HB_DATA_RANGE_LAST_FOUR_WEEKS 3
#define HB_DATA_RANGE_ALL_TIME 4

// --- Records (frame thread) -------------------------------------------------

// Asks for a fresh set of records. Queued; hb_data_store_records_pending stays
// non-zero until the answer is published, and the generation changes once it
// is.
void hb_data_store_request_records(void);
int hb_data_store_records_pending(void);
int hb_data_store_records_generation(void);

// Answers how many records the published set holds, latching that set for this
// thread. The accessors below read the latched set, so a record's fields stay
// consistent even if a new set is published mid-frame.
int hb_data_store_record_count(void);
const char* hb_data_store_record_origin(int index);
const char* hb_data_store_record_host(int index);
// Bytes on disk under the origin's storage, plus the size WebKit reports for
// its DOM caches.
double hb_data_store_record_size(int index);
// Which HB_DATA_TYPE_* bits this origin holds data for. Cookies and the network
// cache are never among them: the jar is one database with no per-site index
// this API can read, and the cache is keyed by URL hash rather than by origin.
int hb_data_store_record_types(int index);
// Seconds since the Unix epoch, the later of the last write to the origin's
// storage and the last time the browser navigated to it.
double hb_data_store_record_last_used(int index);
double hb_data_store_records_total_size(void);

// Seconds since the Unix epoch, on the same clock the records are stamped with.
// A site list showing how long ago a site was used needs both ends of that
// subtraction to come from one clock.
double hb_data_store_now(void);

// Bytes the whole profile holds for one HB_DATA_TYPE_* bit, so a row of the
// clear-data panel can say what clearing it would free. Measured by the same
// scan that publishes the records.
double hb_data_store_type_size(int type);

// --- Removal (frame thread) -------------------------------------------------

// Deletes everything the profile holds for one origin: its storage tree, the
// partitioned trees it has under other sites, its DOM caches and its tracking
// record.
void hb_data_store_remove_origin(const char* origin);

// Deletes the selected types over the selected range.
//
// Cookies are one database with no per-cookie age reachable from WebKit's C
// API, so selecting them clears the jar whole whatever the range is. Every
// other type is scoped to the range.
void hb_data_store_clear(int types, int range);

int hb_data_store_clear_pending(void);
int hb_data_store_clear_generation(void);
// Bytes the last completed removal freed.
double hb_data_store_cleared_bytes(void);

// --- Session ----------------------------------------------------------------
//
// The session is written by the module itself: the pump watches the tab
// registry and writes the file once a change to the list, the order, the
// selection or a tab's address has settled. Private tabs are never written.
//
// Restoring is the host's, because only the host decides how a window comes up.
// It reads the entries below, opens a tab per entry, and hands each tab's id
// back through hb_data_store_session_adopt so the tab's back/forward list is
// restored onto it.

// Frame thread. The session read at launch, in the order it was written.
int hb_data_store_session_restore_count(void);
const char* hb_data_store_session_restore_url(int index);
const char* hb_data_store_session_restore_title(int index);
int hb_data_store_session_restore_pinned(int index);
// The index of the entry that was showing, or -1.
int hb_data_store_session_restore_active_index(void);

// Frame thread. Says that `tab_id` is the tab standing in for restore entry
// `index`. The pump restores that entry's history onto the tab's page as soon
// as the page exists. An entry that carried no history is dropped.
void hb_data_store_session_adopt(int index, int tab_id);

// Non-zero while an adopted entry is still waiting for its page.
int hb_data_store_session_restore_pending(void);

// Frame thread. Forgets the read session, so a host that has finished opening
// its window stops being offered it again.
void hb_data_store_session_restore_finished(void);

#ifdef __cplusplus
}
#endif

#endif // HARMONY_DATA_STORE_H
