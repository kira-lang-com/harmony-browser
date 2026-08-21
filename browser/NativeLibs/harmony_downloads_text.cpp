#include "harmony_downloads_internal.h"

#include <iterator>

namespace harmony_downloads {

bool isTerminal(State state)
{
    return state == State::Finished || state == State::Failed || state == State::Cancelled;
}

std::string formatTimestamp(unsigned long long ticks)
{
    if (!ticks)
        return { };

    ULARGE_INTEGER packed { };
    packed.QuadPart = ticks;
    FILETIME utc { };
    utc.dwLowDateTime = packed.LowPart;
    utc.dwHighDateTime = packed.HighPart;

    FILETIME local { };
    if (!FileTimeToLocalFileTime(&utc, &local))
        return { };

    SYSTEMTIME time { };
    if (!FileTimeToSystemTime(&local, &time))
        return { };

    wchar_t date[128] { };
    wchar_t clock[128] { };
    const int dateLength = GetDateFormatEx(
        LOCALE_NAME_USER_DEFAULT,
        DATE_SHORTDATE,
        &time,
        nullptr,
        date,
        static_cast<int>(std::size(date)),
        nullptr
    );
    const int clockLength = GetTimeFormatEx(
        LOCALE_NAME_USER_DEFAULT,
        TIME_NOSECONDS,
        &time,
        nullptr,
        clock,
        static_cast<int>(std::size(clock))
    );
    if (dateLength <= 0 || clockLength <= 0)
        return { };

    std::wstring formatted(date, static_cast<size_t>(dateLength - 1));
    formatted += L" ";
    formatted += std::wstring(clock, static_cast<size_t>(clockLength - 1));
    return narrow(formatted);
}

unsigned long long nowTicks()
{
    FILETIME now { };
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER packed { };
    packed.LowPart = now.dwLowDateTime;
    packed.HighPart = now.dwHighDateTime;
    return packed.QuadPart;
}

}
