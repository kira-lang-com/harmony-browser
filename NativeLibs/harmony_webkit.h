#ifndef HARMONY_WEBKIT_H
#define HARMONY_WEBKIT_H

#ifdef __cplusplus
extern "C" {
#endif

// The browser's host: the one place every system of this browser is joined to
// the engine.
//
// WebKit keeps ONE client of each kind per page. A browser whose modules each
// called WKPageSetPage*Client would keep whichever module happened to install
// last and silently lose the rest, so the tabs registry owns the engine, the
// thread, the process context, the views, and the three clients a page carries;
// every other system fills its own fields in through that registry's hooks.
//
// A page's clients are written once, when the registry creates it, and reach only
// the hooks the registry holds at that moment. So every system has to be
// registered before the first page exists, and no system can sequence that for
// itself. This host is where all of them are, in one place, on the call that
// starts the registry.
//
// Every function here is called from the host's frame thread and none of them
// blocks on WebKit. Stopping the browser is a sequence too, and it lives beside
// the frame order in `app/WebKitBridge.kira`, because every system it stops has a
// Kira-side half to stop with it.

// Registers every system on the first call, and on every call keeps the engine in
// step with the window: the rectangle the showing tab's view occupies in the host
// window's CLIENT PIXELS, and the host's device-pixel ratio, which every page is
// laid out at so it reads at the same size as the chrome around it.
//
// The geometry is applied before the engine is asked to start, so the first tab
// is created at the size it will be shown at rather than at nothing. Idempotent
// and safe to call every frame; a call with a different window re-parents every
// view. `home_url` is the address a tab opened with no URL of its own loads.
//
// Returns non-zero once the engine is up and serving commands.
int hb_browser_frame(
    void* host_window,
    int x,
    int y,
    int width,
    int height,
    double backing_scale,
    const char* home_url
);

// Non-zero once the engine is up and serving commands.
int hb_browser_ready(void);

// Why the browser is not showing a page, or "" when nothing is wrong. The first
// system with something to say answers, starting with the one that owns the
// engine. The pointer is this thread's copy and holds until this thread asks
// again.
const char* hb_browser_error(void);

#ifdef __cplusplus
}
#endif

#endif
