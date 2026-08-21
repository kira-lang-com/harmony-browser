#ifndef HARMONY_DOWNLOADS_H
#define HARMONY_DOWNLOADS_H

#ifdef __cplusplus
extern "C" {
#endif

// The download side of the WebKit bridge.
//
// WebKit hands a download over on its own thread and writes the file itself
// once it has been told where to put it. Everything below that touches a
// WebKit object therefore runs on the WebKit thread, and everything the host's
// frame thread calls reads a snapshot behind a lock or queues a command the
// WebKit thread picks up in hb_downloads_pump.
//
// The two halves are marked THREAD: below. Calling one on the other's thread is
// the one thing this file cannot recover from.
//
// The engine half is driven by the tabs registry, which owns WebKit: attaching
// registers a download client, a run-loop cycle hook and a teardown hook with
// it, and the three engine-thread functions below are called from those.

// --- Download states ---------------------------------------------------------

#define HB_DOWNLOAD_STATE_STARTING 0
#define HB_DOWNLOAD_STATE_RUNNING 1
#define HB_DOWNLOAD_STATE_FINISHED 2
#define HB_DOWNLOAD_STATE_FAILED 3
#define HB_DOWNLOAD_STATE_CANCELLED 4

// --- Text fields -------------------------------------------------------------

#define HB_DOWNLOAD_TEXT_URL 0
#define HB_DOWNLOAD_TEXT_FILE_NAME 1
#define HB_DOWNLOAD_TEXT_PATH 2
#define HB_DOWNLOAD_TEXT_MIME_TYPE 3
#define HB_DOWNLOAD_TEXT_ERROR 4
#define HB_DOWNLOAD_TEXT_COMPLETED 5

// --- THREAD: WebKit ----------------------------------------------------------

// Drains the commands the host queued. Call once per WebKit run-loop cycle.
// Cancelling a download is a WebKit call, so it happens here and nowhere else.
void hb_downloads_pump(void);

// Whether this navigation should be turned into a download rather than shown.
// Answer 1 by calling WKFramePolicyListenerDownload on the decision's listener,
// and 0 by deciding the navigation as usual.
//
// The action hook answers a link the page marked as a download and a "save link
// as" the host asked for; the response hook answers an attachment and a main
// frame the engine cannot render.
int hb_downloads_should_download_action(const void* navigation_action);
int hb_downloads_should_download_response(const void* navigation_response);

// Takes ownership of a WKDownloadRef delivered by the navigation client and
// installs the download client on it. The download is retained here, so the
// caller keeps whatever reference it had.
void hb_downloads_adopt(const void* download);

// Releases every live download and flushes the history. Call before the WebKit
// thread exits.
void hb_downloads_shutdown(void);

// --- THREAD: host ------------------------------------------------------------

// Attaches the download engine to the tabs registry: from here on every tab's
// page turns an attachment, a file the engine cannot render and a "save link
// as" into a download. Idempotent and safe to call every frame; a tab that
// already exists is covered too, because the registry asks its download clients
// at the moment of each decision rather than at the moment a page is made.
void hb_downloads_attach(void);

// Bumped on every change: a download appearing or leaving, a state moving, a
// name or a path being decided, and a byte count arriving. A host that only
// rebuilds its interface when something moved compares this against what it
// last drew.
int hb_downloads_revision(void);

// The list, newest first.
int hb_downloads_count(void);
int hb_downloads_id_at(int index);

// How many downloads are still running. What a badge counts.
int hb_downloads_active_count(void);

int hb_downloads_state(int id);
long long hb_downloads_received_bytes(int id);

// The size the server declared, or -1 when it declared none.
long long hb_downloads_total_bytes(int id);

// One text field of one download, UTF-8. The returned pointer is the calling
// thread's own copy and holds until that thread calls the same function again,
// so a caller reads it without racing the WebKit thread's next change. An
// unknown id or field gives "".
const char* hb_downloads_text(int id, int field);

// Where downloads are saved, on the same terms.
const char* hb_downloads_directory(void);

// Queued for the WebKit thread; the state moves when WebKit confirms.
void hb_downloads_cancel(int id);

// Forgets one finished download, or every one of them. The file on disk is
// left alone: this is a history entry, not the download.
void hb_downloads_remove(int id);
void hb_downloads_clear_finished(void);

// Shows the file in Explorer, or opens it with its registered handler. Both run
// on a thread of their own so a shell handler cannot stall a frame.
void hb_downloads_reveal(int id);
void hb_downloads_open(int id);
void hb_downloads_reveal_directory(void);

// Marks a URL so the next navigation to it downloads instead of being shown.
// This is "save link as": the host asks for the download, then loads the URL.
void hb_downloads_force_next(const char* url);

#ifdef __cplusplus
}
#endif

#endif
