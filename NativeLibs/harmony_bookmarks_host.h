#ifndef HARMONY_BOOKMARKS_HOST_H
#define HARMONY_BOOKMARKS_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

// How the history and bookmarks model is joined to the engine.
//
// The tabs registry owns WebKit: the module, the thread, the pages, and the one
// navigation client and one page-state client each page carries. So this model
// installs no client of its own, and writes no field of the shared one either:
// did-commit is already the navigation model's, and a second module writing it
// would take the report away from the first.
//
// What it registers instead is the page-state observer, which is the seam built
// for several listeners, and asks WebKit which document is committed each time
// it fires.
//
// This header is kept apart from `harmony_bookmarks.h` because that one is the
// model's Kira-facing surface and holds nothing a C-to-Kira binding cannot
// cross.

// Registers the model with the tabs registry: a page observer, the page-state
// observer every visit and every title is heard through, a run-loop cycle hook
// and a teardown hook. Safe from any thread, and safe to call twice.
//
// The registry consults its observers at the moment it reports, so a tab that
// already exists is heard as well as one created afterwards.
void hb_bookmarks_attach(void);

// Writes both models and stops the disk worker. Call it before the tabs registry
// is stopped: the registry's own teardown writes them too, but only when the
// engine thread is still running to reach it.
void hb_bookmarks_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
