#include "harmony_input_internal.h"

#include <imm.h>

// The accelerator table, matched on both message pumps.
//
// A hook on a pump sees each message as it is taken off the queue and before
// anything is done with it, which is the only place in this browser where a
// chord can be claimed: the chrome's keys are turned into widget events inside
// the host's pump, and the page's keys are dispatched to a window WebKit owns,
// so a shortcut matched anywhere later would have to undo one or the other.
//
// A claimed message is rewritten to WM_NULL. The pump then translates and
// dispatches nothing, which is exactly what "the browser took that key" means.

namespace harmony_input {

namespace {

HHOOK g_frameHook { nullptr };
HHOOK g_engineHook { nullptr };

std::atomic<int> g_pressSerial { 0 };

// The leading half of a supplementary character, held between the two WM_CHAR
// messages Windows delivers it as.
thread_local wchar_t t_pendingHighSurrogate { 0 };

bool held(int virtualKey)
{
    return (GetKeyState(virtualKey) & 0x8000) != 0;
}

// Whether an input method is mid-composition in `window`. While it is, every
// key belongs to the composition: a chord matched here would take a key out of
// a word the user is still assembling.
bool composing(HWND window)
{
    if (!window)
        return false;

    HIMC context = ImmGetContext(window);
    if (!context)
        return false;

    const LONG length = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(window, context);
    return length > 0;
}

bool routingToChrome(const MSG& message, bool engineThread)
{
    if (engineThread)
        return false;
    if (g_textTarget.load() == HB_INPUT_TARGET_NONE)
        return false;
    return message.hwnd == g_hostWindow.load();
}

void queueForTarget(int kind)
{
    queueEvent(kind, 0, g_textTarget.load());
}

// Ctrl+V in a chrome field. The clipboard is the system's, so the text is read
// here and delivered as the keystrokes it stands in for -- which is what makes
// a paste replace a selection and land at the caret without this having to know
// where either is.
void pasteIntoChrome()
{
    HWND host = g_hostWindow.load();
    if (!OpenClipboard(host))
        return;

    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle) {
        auto* text = static_cast<const wchar_t*>(GlobalLock(handle));
        if (text) {
            const int target = g_textTarget.load();
            for (size_t index = 0; text[index]; ++index) {
                const wchar_t unit = text[index];
                // An address bar and a find field are one line each: a pasted
                // newline ends the paste rather than becoming a character no
                // font draws.
                if (unit == L'\r' || unit == L'\n')
                    break;
                if (unit < 0x20)
                    continue;

                int codepoint = unit;
                if (unit >= 0xD800 && unit <= 0xDBFF && text[index + 1] >= 0xDC00 && text[index + 1] <= 0xDFFF) {
                    codepoint = 0x10000 + ((unit - 0xD800) << 10) + (text[index + 1] - 0xDC00);
                    ++index;
                }
                queueEvent(HB_INPUT_EVENT_TEXT, codepoint, target);
            }
            GlobalUnlock(handle);
        }
    }

    if (!CloseClipboard())
        setError("the clipboard could not be closed after a paste");
}

// --- The table --------------------------------------------------------------

bool matchControlChord(WPARAM key, bool shift)
{
    switch (key) {
    case 'L':
        // The address bar takes the keyboard off the page's window, which is
        // the whole point of the chord: until it does, every key belongs to a
        // window this process does not draw.
        focusChrome();
        g_textTarget.store(HB_INPUT_TARGET_ADDRESS);
        queueEvent(HB_INPUT_EVENT_FOCUS_ADDRESS, 0, HB_INPUT_TARGET_ADDRESS);
        return true;
    case 'T':
        queueEvent(shift ? HB_INPUT_EVENT_REOPEN_TAB : HB_INPUT_EVENT_NEW_TAB);
        return true;
    case 'W':
        queueEvent(HB_INPUT_EVENT_CLOSE_TAB);
        return true;
    case VK_TAB:
        queueEvent(shift ? HB_INPUT_EVENT_PREVIOUS_TAB : HB_INPUT_EVENT_NEXT_TAB);
        return true;
    case 'R':
        queueEvent(shift ? HB_INPUT_EVENT_RELOAD_IGNORING_CACHE : HB_INPUT_EVENT_RELOAD);
        return true;
    case VK_F5:
        queueEvent(HB_INPUT_EVENT_RELOAD_IGNORING_CACHE);
        return true;
    case 'F':
        findOpen();
        focusChrome();
        g_textTarget.store(HB_INPUT_TARGET_FIND);
        queueEvent(HB_INPUT_EVENT_OPEN_FIND, 0, HB_INPUT_TARGET_FIND);
        return true;
    case 'G':
        // The other half of find, on the chord a browser has always carried it
        // on: the match after this one, and before it with Shift.
        if (!g_findOpen.load())
            return false;
        queueEvent(shift ? HB_INPUT_EVENT_FIND_PREVIOUS : HB_INPUT_EVENT_FIND_NEXT, 0, HB_INPUT_TARGET_FIND);
        return true;
    case 'P':
        printActiveTab();
        return true;
    case 'A':
        if (g_textTarget.load() == HB_INPUT_TARGET_NONE)
            return false;
        queueForTarget(HB_INPUT_EVENT_SELECT_ALL);
        return true;
    case 'V':
        if (g_textTarget.load() == HB_INPUT_TARGET_NONE)
            return false;
        pasteIntoChrome();
        return true;
    case VK_OEM_PLUS:
    case VK_ADD:
        zoomStep(1);
        return true;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
        zoomStep(-1);
        return true;
    case '0':
    case VK_NUMPAD0:
        zoomReset();
        return true;
    default:
        break;
    }

    // Ctrl+1 to Ctrl+8 select a tab by position, Ctrl+9 the last one, which is
    // what every browser binds them to.
    if (key >= '1' && key <= '8') {
        queueEvent(HB_INPUT_EVENT_SELECT_TAB, static_cast<int>(key - '1'));
        return true;
    }
    if (key == '9') {
        queueEvent(HB_INPUT_EVENT_SELECT_TAB, -1);
        return true;
    }
    return false;
}

bool matchAltChord(WPARAM key)
{
    if (key == VK_LEFT) {
        queueEvent(HB_INPUT_EVENT_BACK);
        return true;
    }
    if (key == VK_RIGHT) {
        queueEvent(HB_INPUT_EVENT_FORWARD);
        return true;
    }
    return false;
}

bool matchPlainKey(WPARAM key, bool shift, bool routing)
{
    switch (key) {
    case VK_F5:
        queueEvent(HB_INPUT_EVENT_RELOAD);
        return true;
    case VK_F6:
        cycleFocus(shift ? -1 : 1);
        return true;
    case VK_TAB:
        // Tab inside the page belongs to the page: it walks the document's own
        // controls, and WebKit hands the focus back through takeFocus when it
        // runs out of them. Tab in the chrome walks the chrome.
        if (focusOwner() == HB_INPUT_FOCUS_CONTENT)
            return false;
        cycleFocus(shift ? -1 : 1);
        return true;
    case VK_ESCAPE:
        if (routing)
            return false;
        if (g_findOpen.load()) {
            findClose();
            queueEvent(HB_INPUT_EVENT_CLOSE_FIND);
            return true;
        }
        queueEvent(HB_INPUT_EVENT_STOP);
        return true;
    case VK_BROWSER_BACK:
        queueEvent(HB_INPUT_EVENT_BACK);
        return true;
    case VK_BROWSER_FORWARD:
        queueEvent(HB_INPUT_EVENT_FORWARD);
        return true;
    case VK_BROWSER_REFRESH:
        queueEvent(HB_INPUT_EVENT_RELOAD);
        return true;
    case VK_BROWSER_STOP:
        queueEvent(HB_INPUT_EVENT_STOP);
        return true;
    default:
        return false;
    }
}

// --- Editing routed to a chrome field ---------------------------------------

bool routeEditKey(WPARAM key, bool shift)
{
    switch (key) {
    case VK_BACK:
        queueForTarget(HB_INPUT_EVENT_BACKSPACE);
        return true;
    case VK_DELETE:
        queueForTarget(HB_INPUT_EVENT_DELETE);
        return true;
    case VK_LEFT:
        queueForTarget(HB_INPUT_EVENT_CARET_LEFT);
        return true;
    case VK_RIGHT:
        queueForTarget(HB_INPUT_EVENT_CARET_RIGHT);
        return true;
    case VK_HOME:
        queueForTarget(HB_INPUT_EVENT_CARET_HOME);
        return true;
    case VK_END:
        queueForTarget(HB_INPUT_EVENT_CARET_END);
        return true;
    case VK_RETURN:
        if (g_textTarget.load() == HB_INPUT_TARGET_FIND) {
            queueEvent(shift ? HB_INPUT_EVENT_FIND_PREVIOUS : HB_INPUT_EVENT_FIND_NEXT, 0, HB_INPUT_TARGET_FIND);
            return true;
        }
        queueForTarget(HB_INPUT_EVENT_SUBMIT);
        return true;
    case VK_ESCAPE:
        queueForTarget(HB_INPUT_EVENT_CANCEL);
        g_textTarget.store(HB_INPUT_TARGET_NONE);
        focusContent();
        return true;
    default:
        return false;
    }
}

bool routeCharacter(const MSG& message)
{
    const wchar_t unit = static_cast<wchar_t>(message.wParam);

    // A control character is the tail of a key this already answered as an
    // edit command, and inserting it would put a glyph nobody typed into the
    // field.
    if (unit < 0x20 || unit == 0x7F) {
        t_pendingHighSurrogate = 0;
        return true;
    }

    if (unit >= 0xD800 && unit <= 0xDBFF) {
        t_pendingHighSurrogate = unit;
        return true;
    }

    int codepoint = unit;
    if (unit >= 0xDC00 && unit <= 0xDFFF) {
        if (!t_pendingHighSurrogate)
            return true;
        codepoint = 0x10000 + ((t_pendingHighSurrogate - 0xD800) << 10) + (unit - 0xDC00);
        t_pendingHighSurrogate = 0;
    }

    queueEvent(HB_INPUT_EVENT_TEXT, codepoint, g_textTarget.load());
    return true;
}

// --- Messages ---------------------------------------------------------------

bool handleKeyDown(const MSG& message, bool engineThread)
{
    const WPARAM key = message.wParam;

    // The input method claimed this key before the window could see it.
    if (key == VK_PROCESSKEY || key == VK_PACKET)
        return false;
    if (composing(message.hwnd))
        return false;

    const bool control = held(VK_CONTROL);
    const bool shift = held(VK_SHIFT);
    const bool alt = held(VK_MENU);
    const bool routing = routingToChrome(message, engineThread);

    if (control && !alt) {
        if (matchControlChord(key, shift))
            return true;
        // A Ctrl chord this does not carry is the page's or the field's, and
        // the character it would otherwise produce is refused below.
        return false;
    }
    if (alt && !control) {
        if (matchAltChord(key))
            return true;
        return false;
    }
    if (!control && !alt) {
        if (matchPlainKey(key, shift, routing))
            return true;
        if (routing)
            return routeEditKey(key, shift);
    }
    return false;
}

bool handleCharacter(const MSG& message, bool engineThread)
{
    if (!routingToChrome(message, engineThread))
        return false;
    // A character produced under Ctrl is the residue of a chord rather than
    // text, and never belongs in the field.
    if (held(VK_CONTROL) && !held(VK_MENU))
        return true;
    return routeCharacter(message);
}

bool handleExtraButton(const MSG& message, bool down)
{
    const WORD button = GET_XBUTTON_WPARAM(message.wParam);
    if (button != XBUTTON1 && button != XBUTTON2)
        return false;

    if (down)
        queueEvent(button == XBUTTON1 ? HB_INPUT_EVENT_BACK : HB_INPUT_EVENT_FORWARD);
    // The release is swallowed with the press: half a button event delivered to
    // a page is a button the page thinks is still down.
    return true;
}

bool handleWheel(const MSG& message)
{
    if ((GET_KEYSTATE_WPARAM(message.wParam) & MK_CONTROL) == 0)
        return false;

    const int delta = GET_WHEEL_DELTA_WPARAM(message.wParam);
    if (delta != 0)
        zoomStep(delta > 0 ? 1 : -1);
    return true;
}

// A press in the chrome. The page's window holds the keyboard until something
// takes it, and a click on the toolbar is that something -- without this, the
// address bar cannot be typed into after the first click in a page.
void noticePress(const MSG& message, bool engineThread)
{
    if (engineThread || message.hwnd != g_hostWindow.load())
        return;

    g_pressSerial.fetch_add(1);
    if (focusOwner() == HB_INPUT_FOCUS_CONTENT)
        focusChrome();
    bumpRevision();
}

bool consumeMessage(const MSG& message, bool engineThread)
{
    switch (message.message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        return handleKeyDown(message, engineThread);
    case WM_CHAR:
        return handleCharacter(message, engineThread);
    case WM_XBUTTONDOWN:
    case WM_XBUTTONDBLCLK:
        return handleExtraButton(message, true);
    case WM_XBUTTONUP:
        return handleExtraButton(message, false);
    case WM_MOUSEWHEEL:
        return handleWheel(message);
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        noticePress(message, engineThread);
        return false;
    default:
        return false;
    }
}

LRESULT CALLBACK frameGetMessageProc(int code, WPARAM wparam, LPARAM lparam)
{
    // PM_NOREMOVE means the pump is only looking: the message stays on the
    // queue and will arrive here again, so claiming it now would claim it
    // twice.
    if (code == HC_ACTION && wparam == PM_REMOVE) {
        auto* message = reinterpret_cast<MSG*>(lparam);
        if (message && consumeMessage(*message, false))
            message->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK engineGetMessageProc(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && wparam == PM_REMOVE) {
        auto* message = reinterpret_cast<MSG*>(lparam);
        if (message && consumeMessage(*message, true))
            message->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace

void installHooks()
{
    if (g_frameHook)
        return;

    g_frameHook = SetWindowsHookExW(WH_GETMESSAGE, frameGetMessageProc, nullptr, GetCurrentThreadId());
    if (!g_frameHook)
        setError("the chrome's keyboard hook could not be installed");
}

void removeHooks()
{
    if (!g_frameHook)
        return;

    if (!UnhookWindowsHookEx(g_frameHook))
        setError("the chrome's keyboard hook could not be removed");
    g_frameHook = nullptr;
}

void installEngineHook()
{
    if (g_engineHook)
        return;

    g_engineHook = SetWindowsHookExW(WH_GETMESSAGE, engineGetMessageProc, nullptr, GetCurrentThreadId());
    if (!g_engineHook)
        setError("the page's keyboard hook could not be installed");
}

void removeEngineHook()
{
    if (!g_engineHook)
        return;

    if (!UnhookWindowsHookEx(g_engineHook))
        setError("the page's keyboard hook could not be removed");
    g_engineHook = nullptr;
}

int pressSerial()
{
    return g_pressSerial.load();
}

} // namespace harmony_input

extern "C" int hb_input_press_serial(void)
{
    return harmony_input::pressSerial();
}
