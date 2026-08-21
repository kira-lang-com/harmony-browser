#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "harmony_text.h"

#include <climits>
#include <cstring>
#include <cwchar>

namespace harmony::text {
namespace {

// Both conversions are counted first and then performed, and both counts are
// read: a string the first call sized and the second refused to write would
// otherwise be a buffer of zeros presented as a name.
//
// The destination is written through `&buffer[0]`, which is a `char*` and a
// `wchar_t*` under every language mode; `data()` only became writable in C++17,
// and a conversion whose compilation depends on the standard level is a
// conversion that will break.
std::string narrowCounted(const wchar_t* value, int length)
{
    if (!value || !length)
        return { };

    const int size = WideCharToMultiByte(CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return { };

    std::string narrowed(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(
        CP_UTF8, 0, value, length, &narrowed[0], size, nullptr, nullptr
    );
    if (written <= 0)
        return { };

    narrowed.resize(static_cast<size_t>(written));
    return narrowed;
}

std::wstring widenCounted(const char* value, int length)
{
    if (!value || !length)
        return { };

    const int size = MultiByteToWideChar(CP_UTF8, 0, value, length, nullptr, 0);
    if (size <= 0)
        return { };

    std::wstring widened(static_cast<size_t>(size), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8, 0, value, length, &widened[0], size);
    if (written <= 0)
        return { };

    widened.resize(static_cast<size_t>(written));
    return widened;
}

}

std::string narrow(const std::wstring& value)
{
    if (value.size() > static_cast<size_t>(INT_MAX))
        return { };
    return narrowCounted(value.c_str(), static_cast<int>(value.size()));
}

std::string narrow(const wchar_t* value)
{
    if (!value)
        return { };
    // Counted rather than terminated, so the result carries no terminator of its
    // own: a `std::string` keeps one already, and a second is a character in the
    // middle of the next name this is concatenated with.
    const size_t length = std::wcslen(value);
    if (length > static_cast<size_t>(INT_MAX))
        return { };
    return narrowCounted(value, static_cast<int>(length));
}

std::wstring widen(const std::string& value)
{
    if (value.size() > static_cast<size_t>(INT_MAX))
        return { };
    return widenCounted(value.c_str(), static_cast<int>(value.size()));
}

std::wstring widen(const char* value)
{
    if (!value)
        return { };
    const size_t length = std::strlen(value);
    if (length > static_cast<size_t>(INT_MAX))
        return { };
    return widenCounted(value, static_cast<int>(length));
}

} // namespace harmony::text
