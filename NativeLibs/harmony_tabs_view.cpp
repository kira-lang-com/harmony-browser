#include "harmony_tabs_internal.h"

#include <algorithm>

// The window half of the registry: the native child window each tab's WKView
// owns, the rectangle they share, and the stand-in frame that covers the moment
// between selecting a tab and its page rendering again.
//
// A background tab is suspended -- hidden, and told it is out of the window --
// so its process is not rendering anything. That is the only way a browser
// holds fifty tabs. The cost is that switching to one has nothing to show until
// the page renders again, so the frame it had when it was last on screen is
// kept and put up immediately, and taken down once the real view has rendered.

namespace harmony_tabs {

namespace {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

// The property the original window procedure of a subclassed tab is kept under.
constexpr wchar_t kOriginalProcProperty[] = L"HarmonyTabsOriginalProc";
constexpr wchar_t kSnapshotWindowClassName[] = L"HarmonyTabsSnapshotWindow";

// WebKit's Windows view class is registered with no background brush, so a view
// with nothing to paint -- one whose page has not rendered at this size yet --
// is left showing whatever was in the window, which reads as black. Erasing it
// white makes the same moment read as a page that has not loaded yet.
LRESULT CALLBACK tabChildProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto original = reinterpret_cast<WNDPROC>(GetPropW(window, kOriginalProcProperty));

    if (message == WM_ERASEBKGND) {
        RECT clientRect { };
        GetClientRect(window, &clientRect);
        FillRect(reinterpret_cast<HDC>(wparam), &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }

    if (message == WM_NCDESTROY) {
        RemovePropW(window, kOriginalProcProperty);
        if (original)
            SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
    }

    if (!original)
        return DefWindowProcW(window, message, wparam, lparam);
    return CallWindowProcW(original, window, message, wparam, lparam);
}

LRESULT CALLBACK snapshotWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_ERASEBKGND) {
        RECT clientRect { };
        GetClientRect(window, &clientRect);
        FillRect(reinterpret_cast<HDC>(wparam), &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        return 1;
    }

    if (message == WM_PAINT) {
        PAINTSTRUCT paint { };
        HDC dc = BeginPaint(window, &paint);
        RECT clientRect { };
        GetClientRect(window, &clientRect);

        if (g_snapshotBitmap && g_snapshotBitmapWidth > 0 && g_snapshotBitmapHeight > 0) {
            HDC memoryDC = CreateCompatibleDC(dc);
            if (memoryDC) {
                HGDIOBJ previous = SelectObject(memoryDC, g_snapshotBitmap);
                // Stretched rather than clipped: a window that has been resized
                // since the frame was taken should show the whole page scaled,
                // not a corner of it.
                StretchBlt(
                    dc,
                    0,
                    0,
                    clientRect.right,
                    clientRect.bottom,
                    memoryDC,
                    0,
                    0,
                    g_snapshotBitmapWidth,
                    g_snapshotBitmapHeight,
                    SRCCOPY
                );
                SelectObject(memoryDC, previous);
                DeleteDC(memoryDC);
            }
        } else
            FillRect(dc, &clientRect, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        EndPaint(window, &paint);
        return 0;
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

} // namespace

RECT currentBoundsRect()
{
    const int x = g_boundsX.load();
    const int y = g_boundsY.load();
    const int width = std::max(1, g_boundsWidth.load());
    const int height = std::max(1, g_boundsHeight.load());
    return RECT { x, y, x + width, y + height };
}

void subclassTabChild(HWND child)
{
    if (!child || GetPropW(child, kOriginalProcProperty))
        return;

    auto original = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(child, GWLP_WNDPROC));
    if (!original)
        return;

    SetPropW(child, kOriginalProcProperty, reinterpret_cast<HANDLE>(original));
    SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(tabChildProc));
}

HWND snapshotWindow(HWND parent)
{
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        return g_snapshotWindow;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass { };
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = snapshotWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        // IDC_ARROW resolves to the narrow form unless UNICODE is defined, and
        // this translation unit does not define it.
        windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
        windowClass.lpszClassName = kSnapshotWindowClassName;
        if (!RegisterClassExW(&windowClass))
            return nullptr;
        registered = true;
    }

    g_snapshotWindow = CreateWindowExW(
        0,
        kSnapshotWindowClassName,
        nullptr,
        WS_CHILD | WS_CLIPSIBLINGS,
        0,
        0,
        0,
        0,
        parent,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    return g_snapshotWindow;
}

void hideSnapshot()
{
    g_snapshotDeadline = 0;
    g_snapshotBitmap = nullptr;
    g_snapshotBitmapWidth = 0;
    g_snapshotBitmapHeight = 0;
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        ShowWindow(g_snapshotWindow, SW_HIDE);
}

void destroySnapshotWindow()
{
    if (g_snapshotWindow && IsWindow(g_snapshotWindow))
        DestroyWindow(g_snapshotWindow);
    g_snapshotWindow = nullptr;
    g_snapshotBitmap = nullptr;
    g_snapshotBitmapWidth = 0;
    g_snapshotBitmapHeight = 0;
    g_snapshotDeadline = 0;
}

void moveSnapshotToBounds()
{
    if (!g_snapshotDeadline || !g_snapshotWindow || !IsWindow(g_snapshotWindow))
        return;

    const RECT rect = currentBoundsRect();
    SetWindowPos(
        g_snapshotWindow,
        HWND_TOP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOACTIVATE
    );
}

void releaseSnapshot(Tab& tab)
{
    if (tab.snapshot && g_snapshotBitmap == tab.snapshot)
        hideSnapshot();
    if (tab.snapshot)
        DeleteObject(tab.snapshot);
    tab.snapshot = nullptr;
    tab.snapshotWidth = 0;
    tab.snapshotHeight = 0;
}

// Keeps the frame a tab has on screen right now, so switching back to it has
// something to show while its page renders again.
//
// `PW_RENDERFULLCONTENT` is what asks for a window's composed content rather
// than the result of sending it a paint message, which is what a view whose
// pixels belong to the GPU process needs. A capture that comes back with
// nothing leaves the white underneath, which reads as a page still loading.
void captureSnapshot(Tab& tab)
{
    if (!tab.child || !IsWindow(tab.child))
        return;

    RECT clientRect { };
    GetClientRect(tab.child, &clientRect);
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0)
        return;

    HDC windowDC = GetDC(tab.child);
    if (!windowDC)
        return;

    HDC memoryDC = CreateCompatibleDC(windowDC);
    HBITMAP bitmap = CreateCompatibleBitmap(windowDC, width, height);
    if (memoryDC && bitmap) {
        HGDIOBJ previous = SelectObject(memoryDC, bitmap);
        RECT fill { 0, 0, width, height };
        FillRect(memoryDC, &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        PrintWindow(tab.child, memoryDC, PW_RENDERFULLCONTENT);
        SelectObject(memoryDC, previous);

        releaseSnapshot(tab);
        tab.snapshot = bitmap;
        tab.snapshotWidth = width;
        tab.snapshotHeight = height;
        bitmap = nullptr;
    }

    if (bitmap)
        DeleteObject(bitmap);
    if (memoryDC)
        DeleteDC(memoryDC);
    ReleaseDC(tab.child, windowDC);
}

// Puts a tab's kept frame on screen at the size the view is about to occupy.
void showSnapshot(Tab& tab)
{
    HWND parent = g_parentWindow.load();
    HWND window = snapshotWindow(parent);
    if (!window)
        return;

    const RECT rect = currentBoundsRect();
    g_snapshotBitmap = tab.snapshot;
    g_snapshotBitmapWidth = tab.snapshotWidth;
    g_snapshotBitmapHeight = tab.snapshotHeight;

    SetWindowPos(
        window,
        HWND_TOP,
        rect.left,
        rect.top,
        rect.right - rect.left,
        rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_SHOWWINDOW
    );
    RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    g_snapshotDeadline = GetTickCount64() + kSnapshotHoldMs;
}

// Stops a tab rendering. Hidden alone is not enough: the view has to be told it
// is out of the window, or its process carries on as though it were visible.
void suspendTab(Tab& tab)
{
    if (!tab.child || tab.suspended)
        return;

    tab.suspended = true;
    ShowWindow(tab.child, SW_HIDE);
    if (g_api.viewSetIsInWindow && tab.view)
        g_api.viewSetIsInWindow(tab.view, false);
}

void resumeTab(Tab& tab)
{
    if (!tab.child)
        return;

    const RECT rect = currentBoundsRect();
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    UINT flags = SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOREDRAW;
    if (tab.width == width && tab.height == height)
        flags |= SWP_NOSIZE;
    else {
        tab.width = width;
        tab.height = height;
    }

    // Below the snapshot while one is up, because the snapshot is holding this
    // tab's last frame until the page has rendered into the view underneath it.
    // Raising the view over it instead would uncover the very blank the stand-in
    // frame exists to hide.
    HWND insertAfter = HWND_TOP;
    if (g_snapshotDeadline && g_snapshotWindow && IsWindow(g_snapshotWindow))
        insertAfter = g_snapshotWindow;
    SetWindowPos(tab.child, insertAfter, rect.left, rect.top, width, height, flags);

    if (tab.suspended) {
        tab.suspended = false;
        if (g_api.viewSetIsInWindow && tab.view)
            g_api.viewSetIsInWindow(tab.view, true);
    }
}

// Runs on the WebKit thread, from the run loop, once the page has rendered.
void repaintCompleted(WKTypeRef, void* context)
{
    const int id = static_cast<int>(reinterpret_cast<intptr_t>(context));
    if (Tab* tab = findTab(id))
        tab->repaintInFlight = false;

    // The view underneath has pixels now, so the frame standing in for it can
    // come down.
    if (id == g_activeTabId)
        hideSnapshot();
}

// Asks a page for a frame. In accelerated compositing mode the UI process holds
// no pixels at all -- the view's content is a surface the GPU process presents
// when the page renders -- so this is the only way to put pixels back after the
// host's swapchain reconfigure, or a resize, has dropped them.
//
// The in-flight flag is per tab: a repaint owed to the tab being switched away
// from must not swallow the one the incoming tab needs.
void requestRepaint(Tab& tab)
{
    if (!g_api.pageForceRepaint || !tab.page || tab.repaintInFlight)
        return;

    tab.repaintInFlight = true;
    g_api.pageForceRepaint(tab.page, reinterpret_cast<void*>(static_cast<intptr_t>(tab.id)), repaintCompleted);
}

} // namespace harmony_tabs
