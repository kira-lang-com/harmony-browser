#include "harmony_permissions_internal.h"

#include "harmony_text.h"

#include <shellapi.h>

#include <deque>
#include <string>

// Where a granted notification actually appears.
//
// A permission that leads nowhere is not a permission, so the grant this bridge
// hands out is backed by a real surface: the shell's notification area, driven
// through one icon this process owns for as long as something is on screen. The
// shell shows one at a time, so the rest queue and follow.
//
// Everything here runs on the WebKit thread. The window is created there, its
// messages arrive through the run loop cycle WebKit already pumps, and the
// answers go straight back to the notification manager.

#ifndef NIN_BALLOONSHOW
#define NIN_BALLOONSHOW (WM_USER + 2)
#endif
#ifndef NIN_BALLOONHIDE
#define NIN_BALLOONHIDE (WM_USER + 3)
#endif
#ifndef NIN_BALLOONTIMEOUT
#define NIN_BALLOONTIMEOUT (WM_USER + 4)
#endif
#ifndef NIN_BALLOONUSERCLICK
#define NIN_BALLOONUSERCLICK (WM_USER + 5)
#endif

namespace harmony_permissions {
namespace {

constexpr UINT kIconCallbackMessage = WM_APP + 10;
constexpr UINT kIconId = 1;
constexpr wchar_t kNotificationWindowClassName[] = L"HarmonyPermissionsNotificationWindow";
constexpr wchar_t kNotificationTooltip[] = L"Harmony Browser";

struct QueuedNotification {
    uint64_t id { 0 };
    std::wstring title;
    std::wstring body;
};

HWND g_window { nullptr };
bool g_iconAdded { false };
uint64_t g_showing { 0 };
std::deque<QueuedNotification> g_queued;

using harmony::text::widen;

void copyBounded(wchar_t* destination, size_t capacity, const std::wstring& text)
{
    const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
    for (size_t index = 0; index < count; ++index)
        destination[index] = text[index];
    destination[count] = L'\0';
}

NOTIFYICONDATAW iconData()
{
    NOTIFYICONDATAW data {};
    data.cbSize = sizeof(data);
    data.hWnd = g_window;
    data.uID = kIconId;
    return data;
}

void showNext();

void finishShowing(bool clicked)
{
    const uint64_t finished = g_showing;
    g_showing = 0;
    if (finished == 0)
        return;

    if (clicked)
        notificationWasClicked(finished);
    else
        notificationWasClosed(finished);

    showNext();
}

LRESULT CALLBACK notificationWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kIconCallbackMessage) {
        switch (static_cast<UINT>(lParam)) {
        case NIN_BALLOONUSERCLICK:
            finishShowing(true);
            return 0;
        case NIN_BALLOONTIMEOUT:
        case NIN_BALLOONHIDE:
            finishShowing(false);
            return 0;
        default:
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

bool ensureWindow()
{
    if (g_window)
        return true;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW windowClass {};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = notificationWindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kNotificationWindowClassName;
        RegisterClassExW(&windowClass);
        registered = true;
    }

    g_window = CreateWindowExW(
        0,
        kNotificationWindowClassName,
        nullptr,
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    if (!g_window) {
        setError("the notification window could not be created");
        return false;
    }
    return true;
}

bool ensureIcon()
{
    if (!ensureWindow())
        return false;
    if (g_iconAdded)
        return true;

    NOTIFYICONDATAW data = iconData();
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kIconCallbackMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    copyBounded(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), kNotificationTooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        setError("the shell refused the notification icon");
        return false;
    }
    g_iconAdded = true;
    return true;
}

void present(const QueuedNotification& notification)
{
    if (!ensureIcon())
        return;

    NOTIFYICONDATAW data = iconData();
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_NONE;
    copyBounded(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(data.szInfoTitle[0]), notification.title);
    copyBounded(data.szInfo, sizeof(data.szInfo) / sizeof(data.szInfo[0]), notification.body);

    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        setError("the shell refused to show a notification");
        notificationWasClosed(notification.id);
        return;
    }
    g_showing = notification.id;
}

void showNext()
{
    if (g_showing != 0 || g_queued.empty())
        return;

    const QueuedNotification next = g_queued.front();
    g_queued.pop_front();
    present(next);
}

void hideBalloon()
{
    if (!g_iconAdded)
        return;

    NOTIFYICONDATAW data = iconData();
    data.uFlags = NIF_INFO;
    data.szInfo[0] = L'\0';
    data.szInfoTitle[0] = L'\0';
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

} // namespace

void notificationSurfaceShow(uint64_t notificationId, const std::string& title, const std::string& body, const std::string& origin)
{
    QueuedNotification notification;
    notification.id = notificationId;
    notification.title = widen(title.empty() ? origin : title);
    notification.body = widen(body.empty() ? origin : body);

    if (g_showing != 0) {
        g_queued.push_back(std::move(notification));
        return;
    }
    present(notification);
}

void notificationSurfaceCancel(uint64_t notificationId)
{
    if (g_showing == notificationId) {
        hideBalloon();
        g_showing = 0;
        showNext();
        return;
    }

    for (auto it = g_queued.begin(); it != g_queued.end(); ++it) {
        if (it->id == notificationId) {
            g_queued.erase(it);
            return;
        }
    }
}

void notificationSurfaceShutdown()
{
    g_queued.clear();
    g_showing = 0;

    if (g_iconAdded) {
        NOTIFYICONDATAW data = iconData();
        Shell_NotifyIconW(NIM_DELETE, &data);
        g_iconAdded = false;
    }

    if (g_window) {
        DestroyWindow(g_window);
        g_window = nullptr;
    }
}

} // namespace harmony_permissions
