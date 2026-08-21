#ifndef HARMONY_BROWSER_CHROME_H
#define HARMONY_BROWSER_CHROME_H

#ifdef __cplusplus
extern "C" {
#endif

// The window-level surface the browser chrome needs and the widget tree cannot
// reach: what the title bar says, and how large the client area a child window
// is positioned inside actually is.
//
// Every function here is called from the host's frame thread, which is the
// thread that owns the window, and none of them block.

// Sets the host window's title bar to `title`, which is UTF-8.
//
// The text already on the window is read first, so a frame loop can hand this
// the page title unconditionally: repeating what the bar says costs one
// in-process message and touches neither the bar nor the taskbar button.
void hb_chrome_set_window_title(void* window, const char* title);

// The window's client area in device pixels.
//
// This is the coordinate space a child window's rectangle is expressed in, and
// it is not the same question the renderer's drawable size answers: the drawable
// is the swapchain's, and a child placed against it is placed against a number
// that belongs to something else. Answers 0 for a handle that is not a window,
// which is every host that is not a Win32 one.
int hb_chrome_client_width(void* window);
int hb_chrome_client_height(void* window);

#ifdef __cplusplus
}
#endif

#endif
