#include "harmony_input_internal.h"

// Which window holds the keyboard, and how it moves.
//
// The browser's chrome and its pages are two windows on two threads inside one
// top-level window, and Windows gives the keyboard to exactly one window at a
// time. So "the address bar is focused" and "the page is focused" are not two
// application flags: they are the same system fact, read here rather than
// remembered, because WebKit moves it from inside the page whenever the page is
// clicked and a remembered copy would be wrong from that moment on.
//
// Above that sits one flag this module does own: which chrome field the keys go
// to while the chrome holds them. The widget layer's focus cannot answer that
// question, because the keys never reach the widget layer when the page holds
// the keyboard, and the whole purpose of Ctrl+L is to type into a field the
// widget layer was not focusing.

namespace harmony_input {

namespace {

std::atomic<int> g_publishedOwner { HB_INPUT_FOCUS_CHROME };

HWND focusedWindow()
{
    // The two input queues are attached, so this thread's answer is the
    // system's. The per-thread query below is the answer when the attachment
    // has not happened yet -- the first frames of a run, before the engine
    // thread has come up.
    if (HWND focus = GetFocus())
        return focus;

    const DWORD threads[2] = { g_engineThreadId.load(), g_frameThreadId.load() };
    for (DWORD thread : threads) {
        if (!thread)
            continue;
        GUITHREADINFO info { };
        info.cbSize = sizeof(info);
        if (GetGUIThreadInfo(thread, &info) && info.hwndFocus)
            return info.hwndFocus;
    }
    return nullptr;
}

// The chrome fields the keyboard can be given to, in the order Tab walks them.
// The page is one stop past the last of them.
int focusStops(int stops[2])
{
    int count = 0;
    stops[count++] = HB_INPUT_TARGET_ADDRESS;
    if (g_findOpen.load())
        stops[count++] = HB_INPUT_TARGET_FIND;
    return count;
}

void focusField(int target)
{
    focusChrome();
    g_textTarget.store(target);
    // A field the keyboard arrives at by Tab or by chord offers what it holds
    // for replacement, which is what makes typing an address over the one on
    // screen a single gesture.
    queueEvent(HB_INPUT_EVENT_SELECT_ALL, 0, target);
    bumpRevision();
}

// WebKit tabbed past the last focusable element of the page. The focus leaves
// the page the way the user was moving through it.
void takeFocusFromPage(WKPageRef, unsigned direction, const void*)
{
    cycleFocus(direction == 0 ? -1 : 1);
}

// The page asked for the focus -- window.focus(), or an element focused from
// script while the page is the active tab.
void pageAskedForFocus(WKPageRef, const void*)
{
    focusContent();
}

void pageGaveUpFocus(WKPageRef, const void*)
{
    focusChrome();
}

} // namespace

int focusOwner()
{
    HWND focus = focusedWindow();
    if (focus && isTabWindow(focus))
        return HB_INPUT_FOCUS_CONTENT;
    return HB_INPUT_FOCUS_CHROME;
}

void focusChrome()
{
    HWND host = g_hostWindow.load();
    if (!host || !IsWindow(host))
        return;
    if (focusedWindow() == host)
        return;

    SetFocus(host);
    bumpRevision();
}

void focusContent()
{
    HWND child = tabWindow(g_activeTabId.load());
    if (!child || !IsWindow(child))
        return;

    // The keyboard going back to the page means no chrome field is holding it,
    // and a field that kept being routed to would swallow the page's keys.
    g_textTarget.store(HB_INPUT_TARGET_NONE);
    if (focusedWindow() != child)
        SetFocus(child);
    bumpRevision();
}

void cycleFocus(int direction)
{
    int stops[2] { };
    const int count = focusStops(stops);
    const int total = count + 1;

    int current = -1;
    if (focusOwner() == HB_INPUT_FOCUS_CONTENT)
        current = count;
    else {
        const int target = g_textTarget.load();
        for (int index = 0; index < count; ++index) {
            if (stops[index] == target)
                current = index;
        }
    }

    int next = current + (direction < 0 ? -1 : 1);
    if (next < 0)
        next = total - 1;
    if (next >= total)
        next = 0;

    if (next == count) {
        focusContent();
        return;
    }
    focusField(stops[next]);
}

void installFocusClient(int, hb_wk_page_ui_client_v19* client)
{
    if (!client)
        return;

    client->takeFocus = reinterpret_cast<void*>(takeFocusFromPage);
    client->focus = reinterpret_cast<void*>(pageAskedForFocus);
    client->unfocus = reinterpret_cast<void*>(pageGaveUpFocus);
}

void serviceFocus()
{
    // A window that is not the active one has no keyboard to arbitrate, and
    // moving the focus inside it would change where Windows puts the keyboard
    // when the user comes back to the browser.
    if (!GetActiveWindow())
        return;

    HWND focus = focusedWindow();
    HWND active = tabWindow(g_activeTabId.load());

    if (focus && isTabWindow(focus) && active && focus != active) {
        // The keyboard was left on the page of the tab that was showing a
        // moment ago. That window is hidden now, or gone, and every key sent
        // to it reaches nobody.
        SetFocus(active);
        focus = active;
    } else if (!focus) {
        // The window that held the keyboard has been destroyed -- a tab
        // closing is the ordinary way -- and the browser is left with no
        // keyboard at all until something claims it.
        HWND host = g_hostWindow.load();
        if (active)
            SetFocus(active);
        else if (host && IsWindow(host))
            SetFocus(host);
        focus = focusedWindow();
    }

    const int owner = (focus && isTabWindow(focus)) ? HB_INPUT_FOCUS_CONTENT : HB_INPUT_FOCUS_CHROME;
    const int published = g_publishedOwner.load();
    if (owner == published)
        return;

    g_publishedOwner.store(owner);
    // The keyboard leaving the chrome leaves no chrome field holding it, and a
    // field still being routed to would swallow the page's keys.
    if (owner == HB_INPUT_FOCUS_CONTENT)
        g_textTarget.store(HB_INPUT_TARGET_NONE);
    bumpRevision();
}

} // namespace harmony_input

// --- The host's surface -----------------------------------------------------

using namespace harmony_input;

extern "C" int hb_input_focus_owner(void)
{
    return focusOwner();
}

extern "C" void hb_input_focus_chrome(void)
{
    focusChrome();
}

extern "C" void hb_input_focus_content(void)
{
    focusContent();
}

extern "C" int hb_input_chrome_text_target(void)
{
    return g_textTarget.load();
}

extern "C" void hb_input_set_chrome_text_target(int target)
{
    if (target != HB_INPUT_TARGET_NONE && target != HB_INPUT_TARGET_ADDRESS && target != HB_INPUT_TARGET_FIND)
        return;
    if (g_textTarget.exchange(target) != target)
        bumpRevision();
}

extern "C" void hb_input_focus_cycle(int direction)
{
    cycleFocus(direction);
}
