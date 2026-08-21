/* Launching a component.
 *
 * A fixed table, like KiraIpc's channels and for the same reason: a shell holds
 * one component of each kind, so the number is small by construction, and a
 * growing table would move the slot an id names. */

#include "harmony_core.h"

#include <string.h>

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Where the answer to `harmony_executable_directory` is kept. One per thread,
 * so two threads asking at once do not write over each other's answer. */
#if defined(_MSC_VER)
#define HARMONY_THREAD_LOCAL __declspec(thread)
#else
#define HARMONY_THREAD_LOCAL __thread
#endif

static HARMONY_THREAD_LOCAL char harmony_directory[4096];

/* Cut the trailing path component off, leaving the directory. Both separators
 * are honoured because Windows accepts either and a path may have come from
 * anywhere. */
static const char *harmony_directory_of(char *path) {
    size_t end = 0;
    size_t index = 0;
    while (path[index] != 0) {
        if (path[index] == '/' || path[index] == '\\') {
            end = index;
        }
        index += 1;
    }
    path[end] = 0;
    return path;
}

void harmony_process_setenv(const char *name, const char *value) {
    if (name == 0 || name[0] == 0) {
        return;
    }
#if defined(_WIN32)
    char assignment[1024];
    _snprintf_s(assignment, sizeof(assignment), _TRUNCATE, "%s=%s", name, value != 0 ? value : "");
    _putenv(assignment);
#else
    if (value == 0 || value[0] == 0) {
        unsetenv(name);
        return;
    }
    setenv(name, value, 1);
#endif
}

int32_t harmony_process_self(void) {
#if defined(_WIN32)
    return (int32_t)GetCurrentProcessId();
#else
    return (int32_t)getpid();
#endif
}

const char *harmony_executable_directory(void) {
#if defined(_WIN32)
    DWORD written = GetModuleFileNameA(0, harmony_directory, (DWORD)sizeof(harmony_directory));
    if (written == 0 || written >= sizeof(harmony_directory)) {
        return "";
    }
    return harmony_directory_of(harmony_directory);
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(harmony_directory);
    if (_NSGetExecutablePath(harmony_directory, &size) != 0) {
        return "";
    }
    return harmony_directory_of(harmony_directory);
#else
    /* Linux, and the BSDs that provide the same link. */
    ssize_t written = readlink("/proc/self/exe", harmony_directory, sizeof(harmony_directory) - 1);
    if (written <= 0) {
        return "";
    }
    harmony_directory[written] = 0;
    return harmony_directory_of(harmony_directory);
#endif
}

#define HARMONY_PROCESS_SLOTS 8

struct harmony_process_slot {
    int used;
#if defined(_WIN32)
    HANDLE handle;
#else
    pid_t pid;
    /* Set once the child has been reaped. A pid is reused by the system, so
     * asking after a dead one has been reaped could answer for a stranger. */
    int reaped;
#endif
};

static struct harmony_process_slot harmony_processes[HARMONY_PROCESS_SLOTS];

static struct harmony_process_slot *harmony_slot_at(harmony_process process) {
    if (process <= 0 || process > HARMONY_PROCESS_SLOTS) {
        return 0;
    }
    struct harmony_process_slot *slot = &harmony_processes[process - 1];
    if (slot->used == 0) {
        return 0;
    }
    return slot;
}

static harmony_process harmony_slot_claim(void) {
    for (int index = 0; index < HARMONY_PROCESS_SLOTS; index += 1) {
        if (harmony_processes[index].used == 0) {
            memset(&harmony_processes[index], 0, sizeof(harmony_processes[index]));
            harmony_processes[index].used = 1;
            return (harmony_process)(index + 1);
        }
    }
    return 0;
}

#if defined(_WIN32)

harmony_process harmony_process_start(const char *executable, const char *endpoint) {
    if (executable == 0) {
        return 0;
    }
    char command[1024];
    _snprintf_s(command, sizeof(command), _TRUNCATE, "\"%s\" \"%s\"",
                executable, endpoint != 0 ? endpoint : "");
    STARTUPINFOA startup;
    PROCESS_INFORMATION information;
    memset(&startup, 0, sizeof(startup));
    memset(&information, 0, sizeof(information));
    startup.cb = sizeof(startup);
    /* No console of its own, and none of the shell's: a component logs where it
     * decides to, and a window that spawned a console would show one. */
    if (CreateProcessA(0, command, 0, 0, FALSE, CREATE_NO_WINDOW, 0, 0, &startup, &information) == 0) {
        return 0;
    }
    CloseHandle(information.hThread);
    harmony_process process = harmony_slot_claim();
    if (process == 0) {
        CloseHandle(information.hProcess);
        return 0;
    }
    harmony_slot_at(process)->handle = information.hProcess;
    return process;
}

int32_t harmony_process_alive(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0) {
        return 0;
    }
    return WaitForSingleObject(slot->handle, 0) == WAIT_TIMEOUT ? 1 : 0;
}

void harmony_process_stop(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0) {
        return;
    }
    /* Windows has no polite signal for a windowless process. The component is
     * expected to exit when its channel to the shell closes, which is the
     * ordinary path; this is the backstop for one that does not. */
    TerminateProcess(slot->handle, 0);
}

void harmony_process_release(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0) {
        return;
    }
    CloseHandle(slot->handle);
    memset(slot, 0, sizeof(*slot));
}

#else

harmony_process harmony_process_start(const char *executable, const char *endpoint) {
    if (executable == 0) {
        return 0;
    }
    harmony_process process = harmony_slot_claim();
    if (process == 0) {
        return 0;
    }
    pid_t pid = fork();
    if (pid < 0) {
        memset(&harmony_processes[process - 1], 0, sizeof(harmony_processes[process - 1]));
        return 0;
    }
    if (pid == 0) {
        /* The child. `setsid` detaches it from the shell's controlling terminal,
         * so a Ctrl-C meant for the shell does not also take down the component
         * that is mid-write to a profile. */
        setsid();
        execl(executable, executable, endpoint != 0 ? endpoint : "", (char *)0);
        /* execl only returns on failure, and there is nothing left to run here:
         * this is the child's address space, and returning would give the caller
         * a second copy of the shell. */
        _exit(127);
    }
    harmony_slot_at(process)->pid = pid;
    return process;
}

int32_t harmony_process_alive(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0 || slot->reaped) {
        return 0;
    }
    int status = 0;
    pid_t answered = waitpid(slot->pid, &status, WNOHANG);
    if (answered == 0) {
        return 1;
    }
    /* Either it exited (and is now reaped) or it was never ours. Both mean there
     * is nothing there, and both must stop us asking again: a pid is reused, and
     * a later question about this one could be answered for a stranger. */
    slot->reaped = 1;
    return 0;
}

void harmony_process_stop(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0 || slot->reaped) {
        return;
    }
    kill(slot->pid, SIGTERM);
}

void harmony_process_release(harmony_process process) {
    struct harmony_process_slot *slot = harmony_slot_at(process);
    if (slot == 0) {
        return;
    }
    if (slot->reaped == 0) {
        /* Reap it if it has already gone, so the entry does not become a zombie
         * the shell holds for as long as it runs. */
        int status = 0;
        waitpid(slot->pid, &status, WNOHANG);
    }
    memset(slot, 0, sizeof(*slot));
}

#endif
