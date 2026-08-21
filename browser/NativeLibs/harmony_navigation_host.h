#ifndef HARMONY_NAVIGATION_HOST_H
#define HARMONY_NAVIGATION_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

// How the navigation model is joined to the engine.
//
// The tabs registry owns WebKit: the module, the thread, the pages, and the one
// navigation client and one page-state client each page carries. So this model
// installs no client of its own. It registers with that registry and is handed
// what it needs: the client to write its own fields into, each page as it
// appears and goes, every state report, and one turn per run-loop cycle.
//
// This header is kept apart from `harmony_navigation_state.h` because that one
// is the model's Kira-facing surface and holds nothing a C-to-Kira binding
// cannot cross.

// Registers the model with the tabs registry: a page observer, the navigation
// client hook, the page-state observer, a run-loop cycle hook and a teardown
// hook. Safe from any thread, and safe to call twice.
//
// Call it before the registry is started. The registry writes a page's clients
// once, so a page that already exists carries a client this model never wrote
// into and would report nothing for the rest of its life.
void hb_nav_attach(void);

// Forgets every tab and clears the published model. The clients are the
// registry's, so nothing is uninstalled: a callback arriving afterwards looks
// its tab up by id and finds nothing. The registry's teardown does this too, so
// call it only to stop the model before the engine stops.
void hb_nav_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
