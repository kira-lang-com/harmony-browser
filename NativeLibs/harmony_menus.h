#ifndef HARMONY_MENUS_H
#define HARMONY_MENUS_H

#ifdef __cplusplus
extern "C" {
#endif

// The menu a right-click on a page raises.
//
// WebKit proposes a menu, this module decides what is on it, and the Windows
// port shows it: the engine's own `WebContextMenuProxyWin` builds a real popup
// menu out of the items handed back and tracks it over the page's window, so a
// browser's context menu is the system's menu and not a panel drawn to look like
// one.
//
// What WebKit proposes covers what a rendering engine knows about -- spelling,
// writing direction, media controls. What it does NOT propose is everything that
// is about the BROWSER: a link opened in another tab, an address copied, a file
// saved, where the page has been, what its source says. Those are built here and
// routed to the system that owns each of them.
//
// Threading. The menu is built and its selection is answered on the WebKit
// thread. An action that belongs to a system living above the engine is queued
// here and drained by the host's frame thread through the four calls below, in
// the same shape the input system's accelerators are.

// --- Commands the host drains -------------------------------------------------

#define HB_MENU_COMMAND_NONE 0

// text: the URL. Opening in the background leaves the page in front.
#define HB_MENU_COMMAND_OPEN_IN_NEW_TAB 1
#define HB_MENU_COMMAND_OPEN_IN_BACKGROUND_TAB 2

// text: the URL, tab: the tab the menu was raised in. The download system marks
// the address and loads it, which is what "save link as" is.
#define HB_MENU_COMMAND_SAVE_AS 3

// tab: the tab the menu was raised in.
#define HB_MENU_COMMAND_BACK 4
#define HB_MENU_COMMAND_FORWARD 5
#define HB_MENU_COMMAND_RELOAD 6
#define HB_MENU_COMMAND_STOP 7

// --- Lifetime -----------------------------------------------------------------

// Registers this module with the tabs registry and installs the context-menu
// client on every page, including any that already exist. Idempotent and safe to
// call every frame.
void hb_menus_attach(void);

// Takes the client off every page and drops the source dumps this run wrote.
// Call before the tabs registry is stopped.
void hb_menus_shutdown(void);

// --- The queue ----------------------------------------------------------------
//
// `hb_menus_next_command` pops one and answers its kind, or
// HB_MENU_COMMAND_NONE when the queue is empty; the two accessors describe the
// command that was just popped.

int hb_menus_next_command(void);

// This thread's copy, valid until this thread pops again.
const char* hb_menus_command_text(void);
int hb_menus_command_tab(void);

// Bumped whenever a command is queued. A host that skips its widget build on an
// idle frame reads this to know it cannot.
int hb_menus_revision(void);

// The last failure, or "". This thread's copy, valid until this thread asks
// again.
const char* hb_menus_error(void);

#ifdef __cplusplus
}
#endif

#endif
