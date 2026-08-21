#ifndef HARMONY_PERMISSIONS_H
#define HARMONY_PERMISSIONS_H

#ifdef __cplusplus
extern "C" {
#endif

// The permission seam between WebKit and the host UI.
//
// WebKit asks a page's UI client whether a page may read a location, post a
// notification, or open a camera. Those questions arrive on the WebKit thread
// and each carries a request object that must be answered exactly once; a
// request object that is dropped unanswered denies (notifications, user media)
// or hangs the page's promise forever (geolocation).
//
// This bridge answers the ones the host has already made up its mind about,
// from a policy table the host pushes down, and queues the rest for the host to
// ask a person about. Queueing retains the request, so it outlives the callback
// that delivered it; answering releases it. Nothing here blocks either thread:
// the host's answer is posted to the WebKit thread through the tabs registry
// and applied there.
//
// Every function here is safe to call from the host's frame thread.

#define HB_PERMISSION_KIND_GEOLOCATION 1
#define HB_PERMISSION_KIND_NOTIFICATIONS 2
#define HB_PERMISSION_KIND_CAMERA 3
#define HB_PERMISSION_KIND_MICROPHONE 4
#define HB_PERMISSION_KIND_CAMERA_AND_MICROPHONE 5
#define HB_PERMISSION_KIND_SCREEN_CAPTURE 6

#define HB_PERMISSION_DECISION_ASK 0
#define HB_PERMISSION_DECISION_ALLOW 1
#define HB_PERMISSION_DECISION_DENY 2

// Joins this bridge to the tabs registry, which owns the engine, the context
// and the one WKPageUIClient a page carries: a client hook for the three
// questions a page asks, and a page observer that installs the position and
// notification providers on the context and refuses whatever a closing page
// still had queued. Safe from any thread, and safe to call twice.
//
// Call it before the tab registry is started. That registry reaches only the
// pages created after a hook was added, so a page that already exists carries a
// client this bridge never wrote into.
void hb_permissions_attach(void);

// Non-zero once the WebKit entry points this bridge needs have been resolved.
int hb_permissions_supported(void);

// The queue of requests no stored decision could answer, oldest first.
int hb_permissions_pending_count(void);

// The oldest unanswered request, or 0 when there is none. An id is never
// reused, so a host that remembers one can tell it apart from its successor.
int hb_permissions_pending_id(void);

// What the oldest unanswered request asks for, as a HB_PERMISSION_KIND_*.
int hb_permissions_pending_kind(void);

// The origin the oldest unanswered request comes from, as "scheme://host[:port]".
// The pointer is thread-local and stays valid until this thread asks again.
const char* hb_permissions_pending_origin(void);

// Answers a queued request. A decision of HB_PERMISSION_DECISION_ALLOW allows;
// anything else denies, so a dismissed prompt and an explicit refusal reach
// WebKit as the same answer. Answering an id that is not queued does nothing.
void hb_permissions_resolve(int request_id, int decision);

// Denies every queued request, for a host shutting down or dropping its prompt.
void hb_permissions_deny_all(void);

// The remembered decisions, as the host holds them. The table answers a request
// without a prompt, and answers the Permissions API's own queries. Setting a
// decision of HB_PERMISSION_DECISION_ASK removes the entry.
void hb_permissions_policy_clear(void);
void hb_permissions_policy_set(int kind, const char* origin, int decision);
int hb_permissions_policy_get(int kind, const char* origin);

// A diagnostic string for the last failure, empty when there was none. The
// pointer is thread-local and stays valid until this thread asks again.
const char* hb_permissions_error(void);

// Denies everything queued, stops the position source, and takes down the
// notification surface. Safe to call from any thread, and safe to call twice.
void hb_permissions_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
