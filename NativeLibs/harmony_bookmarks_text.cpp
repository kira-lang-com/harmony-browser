#include "harmony_bookmarks_internal.h"

#include <atomic>
#include <cstdio>

// What both models are made of: the lock they share, the number that says one of
// them moved, the case folding a query runs against, the host a row is named by,
// and the clock and the calendar a visit is stamped and grouped by.
//
// None of it is history's or bookmarks' alone, and a copy of any of it in either
// would be a copy that can disagree with the other about whether two addresses
// are the same address.

namespace harmony::bookmarks {

namespace {

std::mutex g_modelMutex;
std::atomic<int> g_revision { 0 };

std::mutex g_errorMutex;
std::string g_error;

// 1970-01-01 was a Thursday, and this calendar numbers Sunday zero.
constexpr int kEpochWeekday = 4;

// Days from 1970-01-01 to a proleptic Gregorian date, by the shift-the-era
// method: March opens the year, so the leap day falls at the end of it and
// every other month keeps a fixed length.
long long daysFromCivil(int year, int month, int dayOfMonth)
{
    year -= month <= 2;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned dayOfYear = static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + dayOfMonth - 1);
    const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    return era * 146097 + static_cast<long long>(dayOfEra) - 719468;
}

void civilFromDays(long long day, int& year, int& month, int& dayOfMonth)
{
    day += 719468;
    const long long era = (day >= 0 ? day : day - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(day - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const long long shifted = static_cast<long long>(yearOfEra) + era * 400;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthOfYear = (5 * dayOfYear + 2) / 153;

    dayOfMonth = static_cast<int>(dayOfYear - (153 * monthOfYear + 2) / 5 + 1);
    month = static_cast<int>(monthOfYear + (monthOfYear < 10 ? 3 : -9));
    year = static_cast<int>(shifted + (month <= 2 ? 1 : 0));
}

// FILETIME counts 100-nanosecond intervals from 1601-01-01; the Unix epoch is
// 11644473600 seconds later.
constexpr double kHundredNanosecondsPerSecond = 10000000.0;
constexpr double kEpochDelta = 11644473600.0;

FILETIME fileTimeOf(double unixSeconds)
{
    ULARGE_INTEGER value;
    const double ticks = (unixSeconds + kEpochDelta) * kHundredNanosecondsPerSecond;
    value.QuadPart = ticks > 0.0 ? static_cast<unsigned long long>(ticks) : 0ull;

    FILETIME time { };
    time.dwLowDateTime = value.LowPart;
    time.dwHighDateTime = value.HighPart;
    return time;
}

// The scheme a URL opens with, lowercased, or "" when it opens with none.
std::string schemeOf(const std::string& url)
{
    std::string scheme;
    for (char character : url) {
        if (character == ':')
            return scheme;
        const bool letter = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '+' && character != '-' && character != '.')
            return { };
        scheme += static_cast<char>(character >= 'A' && character <= 'Z' ? character - 'A' + 'a' : character);
    }
    return { };
}

} // namespace

// --- Shared state -----------------------------------------------------------

std::mutex& modelMutex()
{
    return g_modelMutex;
}

void bumpRevisionLocked()
{
    g_revision.fetch_add(1);
}

int currentRevision()
{
    return g_revision.load();
}

void setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    // Said once per distinct failure. A write is retried whenever the model
    // moves again, and a line per attempt would bury the reason it is failing.
    const bool repeated = g_error == message;
    g_error = message;
    if (repeated)
        return;

    // A history that could not be written otherwise looks like a browser that
    // simply forgets where it has been between runs.
    std::fprintf(stderr, "harmony: bookmarks: %s\n", g_error.c_str());
}

void clearError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error.clear();
}

std::string currentError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

const char* answer(const std::string& value)
{
    constexpr size_t slotCount = 8;
    static thread_local std::string slots[slotCount];
    static thread_local size_t next = 0;

    std::string& slot = slots[next];
    next = (next + 1) % slotCount;
    slot = value;
    return slot.c_str();
}

// --- Text -------------------------------------------------------------------

std::string foldCase(const std::string& text)
{
    std::string folded;
    folded.reserve(text.size());
    for (char character : text) {
        if (character >= 'A' && character <= 'Z')
            folded += static_cast<char>(character - 'A' + 'a');
        else
            folded += character;
    }
    return folded;
}

std::string hostOfURL(const std::string& url)
{
    const auto separator = url.find("://");
    if (separator == std::string::npos)
        return { };

    const size_t start = separator + 3;
    size_t end = url.size();
    for (size_t at = start; at < url.size(); ++at) {
        const char character = url[at];
        if (character == '/' || character == '?' || character == '#') {
            end = at;
            break;
        }
    }

    std::string authority = url.substr(start, end - start);

    // Userinfo is not part of what the site is, and a list that spelled it would
    // be a list showing a password.
    const auto at = authority.find_last_of('@');
    if (at != std::string::npos)
        authority.erase(0, at + 1);

    // A port is dropped with the userinfo: a history row is named by the site,
    // and the address it carries is what actually loads.
    const auto colon = authority.find_last_of(':');
    if (colon != std::string::npos && authority.find(']') == std::string::npos)
        authority.erase(colon);

    std::string host = foldCase(authority);
    if (host.rfind("www.", 0) == 0)
        host.erase(0, 4);
    return host;
}

std::string matchableURL(const std::string& url)
{
    std::string text = foldCase(url);
    const auto separator = text.find("://");
    if (separator != std::string::npos)
        text.erase(0, separator + 3);
    if (text.rfind("www.", 0) == 0)
        text.erase(0, 4);
    return text;
}

bool isRecordableURL(const std::string& url)
{
    if (url.empty())
        return false;

    const std::string scheme = schemeOf(url);
    if (scheme.empty())
        return false;
    if (scheme == "about" || scheme == "data" || scheme == "blob" || scheme == "javascript")
        return false;
    return true;
}

// --- Time -------------------------------------------------------------------

double unixTimeOf(const FILETIME& time)
{
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    if (!value.QuadPart)
        return 0.0;
    return static_cast<double>(value.QuadPart) / kHundredNanosecondsPerSecond - kEpochDelta;
}

double unixNow()
{
    FILETIME now { };
    GetSystemTimeAsFileTime(&now);
    return unixTimeOf(now);
}

int localDayOf(double unixSeconds)
{
    if (unixSeconds <= 0.0)
        return 0;

    const FILETIME utc = fileTimeOf(unixSeconds);
    FILETIME local { };
    if (!FileTimeToLocalFileTime(&utc, &local))
        return static_cast<int>(unixSeconds / 86400.0);

    SYSTEMTIME fields { };
    if (!FileTimeToSystemTime(&local, &fields))
        return static_cast<int>(unixSeconds / 86400.0);

    // The day is computed from the local CALENDAR rather than by dividing the
    // clock, because an offset that is not a whole number of hours -- and a
    // daylight change that moves it -- would otherwise put two visits an hour
    // apart on days that are not next to each other.
    return static_cast<int>(daysFromCivil(
        static_cast<int>(fields.wYear),
        static_cast<int>(fields.wMonth),
        static_cast<int>(fields.wDay)
    ));
}

int localMinuteOfDay(double unixSeconds)
{
    if (unixSeconds <= 0.0)
        return 0;

    const FILETIME utc = fileTimeOf(unixSeconds);
    FILETIME local { };
    if (!FileTimeToLocalFileTime(&utc, &local))
        return 0;

    SYSTEMTIME fields { };
    if (!FileTimeToSystemTime(&local, &fields))
        return 0;
    return static_cast<int>(fields.wHour) * 60 + static_cast<int>(fields.wMinute);
}

bool calendarOfDay(int day, int& year, int& month, int& dayOfMonth, int& weekday)
{
    if (day <= 0)
        return false;

    civilFromDays(day, year, month, dayOfMonth);
    weekday = static_cast<int>(((day % 7) + kEpochWeekday + 7) % 7);
    return true;
}

} // namespace harmony::bookmarks
