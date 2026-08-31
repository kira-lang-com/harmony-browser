/* harmony_agent_clock.c -- clock and unique-id source for the Harmony agent.
 *
 * This file holds only the three clock and id functions, and nothing else.
 */

#include "harmony_agent_http.h"

#include <stdint.h>
#include <stdatomic.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#endif

/* --------------------------------------------------------------------- */
/* Unix seconds                                                          */
/* --------------------------------------------------------------------- */

int64_t harmony_clock_unix_seconds(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    /* 100-nanosecond intervals since 1601-01-01; shift to Unix epoch. */
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    int64_t ticks = (int64_t)li.QuadPart;        /* 100ns since 1601 */
    int64_t unix100ns = ticks - 116444736000000000LL;
    return unix100ns / 10000000LL;
#else
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) return 0;
    return (int64_t)tv.tv_sec;
#endif
}

/* --------------------------------------------------------------------- */
/* Monotonic milliseconds                                                */
/* --------------------------------------------------------------------- */

int64_t harmony_clock_monotonic_millis(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!QueryPerformanceFrequency(&freq) || !QueryPerformanceCounter(&now))
        return 0;
    return (int64_t)((now.QuadPart * 1000) / freq.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
#endif
}

/* --------------------------------------------------------------------- */
/* Unique id                                                             */
/* --------------------------------------------------------------------- */

/* Monotonic within a process and seeded so two processes started in the same
 * second do not agree. The high part is a per-process seed derived from the
 * pid, the Unix second and a monotonic reading; the low part is a counter. */

static atomic_int_least64_t g_seed = 0;
static atomic_int_least64_t g_counter = 0;

int64_t harmony_agent_unique(void) {
    int64_t s = atomic_load(&g_seed);
    if (s == 0) {
        int64_t pid = 0;
        int64_t sec = harmony_clock_unix_seconds();
        int64_t mono = harmony_clock_monotonic_millis();
#if defined(_WIN32)
        pid = (int64_t)GetCurrentProcessId();
#else
        pid = (int64_t)getpid();
#endif
        int64_t ns = ((pid & 0xffffff) << 40) |
                     ((sec & 0xffffff) << 16) |
                     (mono & 0xffff);
        if (ns == 0) ns = 1;
        if (atomic_compare_exchange_strong(&g_seed, &s, ns)) {
            s = ns;
        } else {
            s = atomic_load(&g_seed);
        }
    }
    int64_t c = atomic_fetch_add(&g_counter, 1) + 1;
    return s * 1000000 + c;
}
