#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "harmony_browser_chrome.h"

#include "harmony_text.h"

#include <string>
#include <vector>

namespace {

// What the bar says right now. Asked of the window rather than remembered here,
// because a remembered title is wrong the moment anything else writes one and a
// window handle outlives the window it named.
std::wstring windowTitle(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
        return { };

    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, buffer.data(), length + 1);
    if (copied <= 0)
        return { };
    return std::wstring(buffer.data(), static_cast<size_t>(copied));
}

RECT clientRect(void* window)
{
    RECT client { 0, 0, 0, 0 };
    HWND handle = static_cast<HWND>(window);
    if (!handle || !IsWindow(handle))
        return client;
    if (!GetClientRect(handle, &client))
        return RECT { 0, 0, 0, 0 };
    return client;
}

}

void hb_chrome_set_window_title(void* window, const char* title)
{
    HWND handle = static_cast<HWND>(window);
    if (!handle || !IsWindow(handle))
        return;

    const std::wstring wide = harmony::text::widen(title);
    if (windowTitle(handle) == wide)
        return;

    SetWindowTextW(handle, wide.c_str());
}

int hb_chrome_client_width(void* window)
{
    const RECT client = clientRect(window);
    return static_cast<int>(client.right - client.left);
}

int hb_chrome_client_height(void* window)
{
    const RECT client = clientRect(window);
    return static_cast<int>(client.bottom - client.top);
}
