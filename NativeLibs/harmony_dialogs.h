#ifndef HARMONY_DIALOGS_H
#define HARMONY_DIALOGS_H

#ifdef __cplusplus
extern "C" {
#endif

// Page-driven dialogs: the WebKit side of every question a page asks the user.
//
// A page asks on the WebKit thread and is suspended until it is answered. The
// answer is a person's, so it arrives frames later, from the host's frame
// thread. Everything here is that split: the WebKit thread parks a request, the
// frame thread reads it and posts an answer, and the answer is handed back to
// WebKit on the WebKit thread.
//
// The tabs registry owns the engine, the pages and the one WKPageUIClient a page
// carries, so this module fills its fields through that registry's hook rather
// than installing a client of its own. `hb_dialogs_attach` is the whole of that
// wiring.
//
// The one exception is the file picker, which is a Windows common dialog rather
// than a question the host draws. It runs on a thread of its own so the picker
// being open never stops WebKit or the host from drawing.

// The kind of question a parked request is asking. `hb_dialogs_request_kind`
// answers with one of these; the file picker never appears because it is
// answered natively.
#define HB_DIALOG_NONE 0
#define HB_DIALOG_ALERT 1
#define HB_DIALOG_CONFIRM 2
#define HB_DIALOG_PROMPT 3
#define HB_DIALOG_BEFORE_UNLOAD 4
#define HB_DIALOG_AUTHENTICATE 5
#define HB_DIALOG_CERTIFICATE 6

// `hb_dialogs_request_flags` bits.
#define HB_DIALOG_FLAG_PROXY 1
#define HB_DIALOG_FLAG_RETRY 2
#define HB_DIALOG_FLAG_SUBFRAME 4

// Joins this module to the tabs registry: a UI client hook for the questions a
// page asks, a navigation client hook for the sign-in and certificate
// challenges a page's loads raise, and a page observer that answers whatever a
// closing page still had parked. Safe from any thread, and safe to call twice.
//
// Call it before the tab registry is started. Both registries reach only the
// pages created after the hook was added, so a page that already exists carries
// a client this module never wrote into.
void hb_dialogs_attach(void);

// --- Frame thread -----------------------------------------------------------

// The request the host should be showing, or 0. Records the host window every
// call, and holds the page's child windows down for exactly as long as one is
// showing. `host_overlay` is non-zero for a frame in which the host is drawing
// something else of its own over the page — a permission prompt, a menu — and
// holds those windows down for that too, because a page's view is a child window
// composited above everything the host draws.
//
// The request answered is the newest one parked, which is the one a person is
// looking at when a page asks a second question from inside the answer to the
// first.
int hb_dialogs_frame(void* host_window, int host_overlay);

// The parked request's contents. Every string is valid until this thread's next
// call to the same function.
int hb_dialogs_request_kind(int request_id);
int hb_dialogs_request_flags(int request_id);
// The certificate verification error for a certificate request, and the count
// of attempts already refused for an authentication one.
int hb_dialogs_request_code(int request_id);
const char* hb_dialogs_request_message(int request_id);
const char* hb_dialogs_request_origin(int request_id);
const char* hb_dialogs_request_default_value(int request_id);
const char* hb_dialogs_request_detail(int request_id);

// The answers. Each is ignored unless `request_id` is still parked, so a second
// press on a dialog that has already closed does nothing.
//
// accept: alert dismissed, confirm accepted, before-unload left.
// dismiss: confirm refused, prompt cancelled, before-unload stayed, sign-in
//          abandoned, certificate refused.
void hb_dialogs_respond_accept(int request_id);
void hb_dialogs_respond_dismiss(int request_id);
void hb_dialogs_respond_text(int request_id, const char* text);
void hb_dialogs_respond_credential(int request_id, const char* user, const char* password, int remember);
// Proceeds through a certificate the system refused. `remember` keeps the
// exact certificate this host presented, so the interstitial is asked once.
void hb_dialogs_respond_trust(int request_id, int remember);

// Answers everything still parked with a refusal and stops the file-picker
// thread. Call before the tab registry is shut down: a listener does not
// outlive the page it came from, and a page waiting on an answer that never
// comes is a web process that never exits.
void hb_dialogs_shutdown(void);

// A diagnostic string for a failure that has no dialog of its own — the WebKit
// runtime missing an entry point, or the file picker failing to open.
const char* hb_dialogs_error(void);

#ifdef __cplusplus
}
#endif

#endif
