/* A child process the assistant drives over stdio, one JSON-RPC line per
 * message. The assistant runs on a 16-millisecond loop with no threads and no
 * blocking of its own, so everything here is non-blocking: a line is asked for
 * and either one has arrived or it has not. Pipes are created non-blocking and
 * anything that cannot be written right now is kept until it can.
 *
 * Handles are small integers into a table this file owns; a slot holds all of a
 * child's state so a Kira call naming a closed or never-opened handle does
 * nothing and answers zero or an empty string. */

#include "harmony_agent_child.h"

#if defined(_WIN32)

/* ===================================================================== */
/* Windows                                                               */
/* ===================================================================== */

#include <windows.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>

#define HARMONY_CHILD_SLOTS 8

struct harmony_child_bytes {
    char *data;
    size_t length;
    size_t capacity;
};

static int harmony_bytes_reserve(struct harmony_child_bytes *buffer, size_t want) {
    if (buffer->capacity >= want) {
        return 1;
    }
    size_t next = buffer->capacity == 0 ? 256 : buffer->capacity * 2;
    while (next < want) {
        next *= 2;
    }
    char *grown = (char *)realloc(buffer->data, next);
    if (grown == 0) {
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = next;
    return 1;
}

static void harmony_bytes_append(struct harmony_child_bytes *buffer, const char *bytes, size_t count) {
    if (count == 0) {
        return;
    }
    if (!harmony_bytes_reserve(buffer, buffer->length + count)) {
        return;
    }
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
}

static wchar_t *harmony_wcsdup(const wchar_t *text) {
    if (text == 0) {
        return 0;
    }
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1) * sizeof(wchar_t));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, (length + 1) * sizeof(wchar_t));
    return copy;
}

static char *harmony_strdup_narrow(const char *text) {
    if (text == 0) {
        return 0;
    }
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length + 1);
    return copy;
}

static wchar_t *harmony_utf8_to_wide(const char *text) {
    if (text == 0) {
        return 0;
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, 0, 0);
    if (needed <= 0) {
        return 0;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (wide == 0) {
        return 0;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, needed) == 0) {
        free(wide);
        return 0;
    }
    return wide;
}

static char *harmony_wide_to_utf8(const wchar_t *text) {
    if (text == 0) {
        return 0;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (needed <= 0) {
        return 0;
    }
    char *narrow = (char *)malloc((size_t)needed);
    if (narrow == 0) {
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, narrow, needed, 0, 0) == 0) {
        free(narrow);
        return 0;
    }
    return narrow;
}

struct harmony_child_slot {
    int used;
    int32_t state;
    HANDLE process;
    DWORD pid;
    HANDLE stdin_write;
    HANDLE stdout_read;
    HANDLE stderr_read;
    wchar_t *executable;
    wchar_t **arguments;
    int argument_count;
    int argument_capacity;
    wchar_t **environment;
    int environment_count;
    int environment_capacity;
    wchar_t *directory;
    struct harmony_child_bytes output;
    struct harmony_child_bytes line;
    struct harmony_child_bytes diagnostics;
    char *diagnostics_returned;
    struct harmony_child_bytes pending;
    int32_t exit_status;
    char *error;
};

static struct harmony_child_slot harmony_children[HARMONY_CHILD_SLOTS];

static struct harmony_child_slot *harmony_child_slot_claim(void) {
    for (int index = 0; index < HARMONY_CHILD_SLOTS; index += 1) {
        if (harmony_children[index].used == 0) {
            struct harmony_child_slot *slot = &harmony_children[index];
            memset(slot, 0, sizeof(*slot));
            slot->used = 1;
            slot->state = HARMONY_CHILD_UNSTARTED;
            slot->process = INVALID_HANDLE_VALUE;
            slot->stdin_write = INVALID_HANDLE_VALUE;
            slot->stdout_read = INVALID_HANDLE_VALUE;
            slot->stderr_read = INVALID_HANDLE_VALUE;
            return slot;
        }
    }
    return 0;
}

static struct harmony_child_slot *harmony_child_slot_at(int32_t handle) {
    if (handle <= 0 || handle > HARMONY_CHILD_SLOTS) {
        return 0;
    }
    struct harmony_child_slot *slot = &harmony_children[handle - 1];
    if (slot->used == 0) {
        return 0;
    }
    return slot;
}

static void harmony_child_release(struct harmony_child_slot *slot) {
    if (slot->stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(slot->stdin_write);
        slot->stdin_write = INVALID_HANDLE_VALUE;
    }
    if (slot->stdout_read != INVALID_HANDLE_VALUE) {
        CloseHandle(slot->stdout_read);
        slot->stdout_read = INVALID_HANDLE_VALUE;
    }
    if (slot->stderr_read != INVALID_HANDLE_VALUE) {
        CloseHandle(slot->stderr_read);
        slot->stderr_read = INVALID_HANDLE_VALUE;
    }
    if (slot->process != INVALID_HANDLE_VALUE) {
        CloseHandle(slot->process);
        slot->process = INVALID_HANDLE_VALUE;
    }
    free(slot->executable);
    for (int i = 0; i < slot->argument_count; i++) {
        free(slot->arguments[i]);
    }
    free(slot->arguments);
    for (int i = 0; i < slot->environment_count; i++) {
        free(slot->environment[i]);
    }
    free(slot->environment);
    free(slot->directory);
    free(slot->output.data);
    free(slot->line.data);
    free(slot->diagnostics.data);
    free(slot->diagnostics_returned);
    free(slot->pending.data);
    free(slot->error);
    memset(slot, 0, sizeof(*slot));
}

static void harmony_child_flush(struct harmony_child_slot *slot) {
    if (slot->pending.length == 0) {
        return;
    }
    if (slot->stdin_write == INVALID_HANDLE_VALUE) {
        slot->pending.length = 0;
        return;
    }
    size_t written = 0;
    while (written < slot->pending.length) {
        DWORD step = 0;
        BOOL ok = WriteFile(slot->stdin_write, slot->pending.data + written,
                            (DWORD)(slot->pending.length - written), &step, 0);
        if (!ok || step == 0) {
            /* The pipe will not take more right now, or the child is gone:
             * keep what remains for the next attempt. */
            break;
        }
        written += (size_t)step;
    }
    if (written > 0) {
        memmove(slot->pending.data, slot->pending.data + written,
                slot->pending.length - written);
        slot->pending.length -= written;
    }
}

static void harmony_child_drain(HANDLE handle, struct harmony_child_bytes *buffer) {
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(handle, 0, 0, 0, &available, 0)) {
            break;
        }
        if (available == 0) {
            break;
        }
        char chunk[4096];
        DWORD to_read = available > (DWORD)sizeof(chunk) ? (DWORD)sizeof(chunk) : available;
        DWORD got = 0;
        if (!ReadFile(handle, chunk, to_read, &got, 0)) {
            break;
        }
        if (got == 0) {
            break;
        }
        harmony_bytes_append(buffer, chunk, (size_t)got);
    }
}

static void harmony_child_reap(struct harmony_child_slot *slot) {
    if (slot->process == INVALID_HANDLE_VALUE) {
        slot->state = HARMONY_CHILD_EXITED;
        return;
    }
    if (WaitForSingleObject(slot->process, 0) == WAIT_OBJECT_0) {
        DWORD code = 0;
        if (GetExitCodeProcess(slot->process, &code)) {
            slot->exit_status = (int32_t)code;
        }
        slot->state = HARMONY_CHILD_EXITED;
    }
}

int32_t harmony_child_open(const char *executable) {
    if (executable == 0 || executable[0] == 0) {
        return 0;
    }
    struct harmony_child_slot *slot = harmony_child_slot_claim();
    if (slot == 0) {
        return 0;
    }
    slot->executable = harmony_utf8_to_wide(executable);
    if (slot->executable == 0) {
        slot->used = 0;
        return 0;
    }
    return (int32_t)((slot - harmony_children) + 1);
}

void harmony_child_argument(int32_t child, const char *argument) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (argument == 0) {
        return;
    }
    if (slot->argument_count + 1 >= slot->argument_capacity) {
        int capacity = slot->argument_capacity == 0 ? 8 : slot->argument_capacity * 2;
        wchar_t **grown = (wchar_t **)realloc(slot->arguments, (size_t)capacity * sizeof(wchar_t *));
        if (grown == 0) {
            return;
        }
        slot->arguments = grown;
        slot->argument_capacity = capacity;
    }
    wchar_t *copy = harmony_utf8_to_wide(argument);
    if (copy == 0) {
        return;
    }
    slot->arguments[slot->argument_count] = copy;
    slot->argument_count += 1;
    slot->arguments[slot->argument_count] = 0;
}

void harmony_child_environment(int32_t child, const char *name, const char *value) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (name == 0 || name[0] == 0) {
        return;
    }
    wchar_t *name_wide = harmony_utf8_to_wide(name);
    wchar_t *value_wide = harmony_utf8_to_wide(value != 0 ? value : "");
    if (name_wide == 0 || value_wide == 0) {
        free(name_wide);
        free(value_wide);
        return;
    }
    size_t total = wcslen(name_wide) + 1 + wcslen(value_wide) + 1;
    wchar_t *assignment = (wchar_t *)malloc(total * sizeof(wchar_t));
    if (assignment == 0) {
        free(name_wide);
        free(value_wide);
        return;
    }
    wcscpy(assignment, name_wide);
    wcscat(assignment, L"=");
    wcscat(assignment, value_wide);
    free(name_wide);
    free(value_wide);
    if (slot->environment_count + 1 >= slot->environment_capacity) {
        int capacity = slot->environment_capacity == 0 ? 8 : slot->environment_capacity * 2;
        wchar_t **grown = (wchar_t **)realloc(slot->environment, (size_t)capacity * sizeof(wchar_t *));
        if (grown == 0) {
            free(assignment);
            return;
        }
        slot->environment = grown;
        slot->environment_capacity = capacity;
    }
    slot->environment[slot->environment_count] = assignment;
    slot->environment_count += 1;
    slot->environment[slot->environment_count] = 0;
}

void harmony_child_directory(int32_t child, const char *path) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (path == 0) {
        return;
    }
    free(slot->directory);
    slot->directory = harmony_utf8_to_wide(path);
}

int32_t harmony_child_start(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return 0;
    }
    if (slot->executable == 0) {
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup_narrow("no executable given");
        return 0;
    }

    HANDLE child_stdin_read = INVALID_HANDLE_VALUE;
    HANDLE child_stdin_write = INVALID_HANDLE_VALUE;
    HANDLE child_stdout_read = INVALID_HANDLE_VALUE;
    HANDLE child_stdout_write = INVALID_HANDLE_VALUE;
    HANDLE child_stderr_read = INVALID_HANDLE_VALUE;
    HANDLE child_stderr_write = INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES inherit;
    memset(&inherit, 0, sizeof(inherit));
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;

    if (!CreatePipe(&child_stdin_read, &child_stdin_write, &inherit, 0) ||
        !CreatePipe(&child_stdout_read, &child_stdout_write, &inherit, 0) ||
        !CreatePipe(&child_stderr_read, &child_stderr_write, &inherit, 0)) {
        if (child_stdin_read != INVALID_HANDLE_VALUE) CloseHandle(child_stdin_read);
        if (child_stdin_write != INVALID_HANDLE_VALUE) CloseHandle(child_stdin_write);
        if (child_stdout_read != INVALID_HANDLE_VALUE) CloseHandle(child_stdout_read);
        if (child_stdout_write != INVALID_HANDLE_VALUE) CloseHandle(child_stdout_write);
        if (child_stderr_read != INVALID_HANDLE_VALUE) CloseHandle(child_stderr_read);
        if (child_stderr_write != INVALID_HANDLE_VALUE) CloseHandle(child_stderr_write);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup_narrow("could not create pipes");
        return 0;
    }

    /* Our ends must stay private to this process. */
    SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(child_stderr_read, HANDLE_FLAG_INHERIT, 0);

    /* The write end answers "would not take it" instead of stalling. */
    DWORD pipe_mode = PIPE_NOWAIT;
    SetNamedPipeHandleState(child_stdin_write, &pipe_mode, 0, 0);

    size_t command_capacity = wcslen(slot->executable) + 3;
    for (int i = 0; i < slot->argument_count; i++) {
        command_capacity += wcslen(slot->arguments[i]) + 3;
    }
    wchar_t *command = (wchar_t *)malloc((command_capacity + 1) * sizeof(wchar_t));
    if (command == 0) {
        CloseHandle(child_stdin_read); CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read); CloseHandle(child_stdout_write);
        CloseHandle(child_stderr_read); CloseHandle(child_stderr_write);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup_narrow("out of memory");
        return 0;
    }
    wchar_t *cursor = command;
    *cursor++ = L'"';
    wcscpy(cursor, slot->executable);
    cursor += wcslen(slot->executable);
    *cursor++ = L'"';
    *cursor = 0;
    for (int i = 0; i < slot->argument_count; i++) {
        wcscat(cursor, L" \"");
        wcscat(cursor, slot->arguments[i]);
        wcscat(cursor, L"\"");
    }

    wchar_t *environment_block = 0;
    if (slot->environment_count > 0) {
        wchar_t *existing = GetEnvironmentStringsW();
        if (existing != 0) {
            size_t existing_length = 0;
            wchar_t *scan = existing;
            while (*scan != 0) {
                size_t entry = wcslen(scan) + 1;
                existing_length += entry;
                scan += entry;
            }
            existing_length += 1;
            size_t added_length = 1;
            for (int i = 0; i < slot->environment_count; i++) {
                added_length += wcslen(slot->environment[i]) + 1;
            }
            wchar_t *block = (wchar_t *)malloc((existing_length + added_length) * sizeof(wchar_t));
            if (block != 0) {
                wchar_t *dst = block;
                memcpy(dst, existing, existing_length * sizeof(wchar_t));
                dst += existing_length;
                for (int i = 0; i < slot->environment_count; i++) {
                    size_t entry = wcslen(slot->environment[i]) + 1;
                    memcpy(dst, slot->environment[i], entry * sizeof(wchar_t));
                    dst += entry;
                }
                *dst = 0;
                environment_block = block;
            }
            FreeEnvironmentStringsW(existing);
        }
    }

    STARTUPINFOW startup;
    PROCESS_INFORMATION information;
    memset(&startup, 0, sizeof(startup));
    memset(&information, 0, sizeof(information));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin_read;
    startup.hStdOutput = child_stdout_write;
    startup.hStdError = child_stderr_write;

    BOOL started = CreateProcessW(slot->executable, command, 0, 0, TRUE,
                                  CREATE_NO_WINDOW, environment_block,
                                  slot->directory, &startup, &information);

    free(command);
    free(environment_block);

    if (!started) {
        CloseHandle(child_stdin_read); CloseHandle(child_stdin_write);
        CloseHandle(child_stdout_read); CloseHandle(child_stdout_write);
        CloseHandle(child_stderr_read); CloseHandle(child_stderr_write);
        slot->state = HARMONY_CHILD_FAILED;
        wchar_t message[256];
        DWORD written = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM, 0,
                                       GetLastError(), 0, message,
                                       (DWORD)(sizeof(message) / sizeof(wchar_t)), 0);
        if (written == 0) {
            wcscpy(message, L"could not start child");
        }
        slot->error = harmony_wide_to_utf8(message);
        if (slot->error == 0) {
            slot->error = harmony_strdup_narrow("could not start child");
        }
        return 0;
    }

    CloseHandle(child_stdin_read);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stderr_write);
    slot->process = information.hProcess;
    slot->pid = information.dwProcessId;
    CloseHandle(information.hThread);
    slot->stdin_write = child_stdin_write;
    slot->stdout_read = child_stdout_read;
    slot->stderr_read = child_stderr_read;
    slot->state = HARMONY_CHILD_RUNNING;
    return 1;
}

int32_t harmony_child_state(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return HARMONY_CHILD_UNSTARTED;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    return slot->state;
}

int32_t harmony_child_status(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return 0;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_EXITED) {
        return slot->exit_status;
    }
    return 0;
}

int32_t harmony_child_write(int32_t child, const char *text) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_RUNNING) {
        return 0;
    }
    if (text == 0) {
        return 0;
    }
    size_t total = strlen(text);
    if (total > 0) {
        if (!harmony_bytes_reserve(&slot->pending, slot->pending.length + total)) {
            return 0;
        }
        memcpy(slot->pending.data + slot->pending.length, text, total);
        slot->pending.length += total;
    }
    harmony_child_flush(slot);
    return slot->pending.length == 0 ? 1 : 0;
}

const char *harmony_child_line(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    if (slot->state == HARMONY_CHILD_UNSTARTED || slot->state == HARMONY_CHILD_FAILED) {
        return "";
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_RUNNING || slot->state == HARMONY_CHILD_EXITED) {
        harmony_child_flush(slot);
        harmony_child_drain(slot->stdout_read, &slot->output);
    }
    size_t newline = 0;
    int found = 0;
    while (newline < slot->output.length) {
        if (slot->output.data[newline] == '\n') {
            found = 1;
            break;
        }
        newline += 1;
    }
    if (!found) {
        return "";
    }
    size_t line_length = newline;
    if (!harmony_bytes_reserve(&slot->line, line_length + 1)) {
        return "";
    }
    if (line_length > 0) {
        memcpy(slot->line.data, slot->output.data, line_length);
    }
    slot->line.data[line_length] = 0;
    slot->line.length = line_length;
    size_t consumed = line_length + 1;
    memmove(slot->output.data, slot->output.data + consumed, slot->output.length - consumed);
    slot->output.length -= consumed;
    return slot->line.data;
}

const char *harmony_child_diagnostics(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    free(slot->diagnostics_returned);
    slot->diagnostics_returned = 0;
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_RUNNING || slot->state == HARMONY_CHILD_EXITED) {
        harmony_child_drain(slot->stderr_read, &slot->diagnostics);
    }
    if (slot->diagnostics.length == 0) {
        return "";
    }
    if (!harmony_bytes_reserve(&slot->diagnostics, slot->diagnostics.length + 1)) {
        return "";
    }
    slot->diagnostics.data[slot->diagnostics.length] = 0;
    slot->diagnostics_returned = slot->diagnostics.data;
    slot->diagnostics.data = 0;
    slot->diagnostics.length = 0;
    slot->diagnostics.capacity = 0;
    return slot->diagnostics_returned;
}

const char *harmony_child_error(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    return slot->error != 0 ? slot->error : "";
}

void harmony_child_finish_input(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_flush(slot);
        if (slot->stdin_write != INVALID_HANDLE_VALUE) {
            CloseHandle(slot->stdin_write);
            slot->stdin_write = INVALID_HANDLE_VALUE;
        }
    }
}

void harmony_child_stop(int32_t child, int32_t graceMilliseconds) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_flush(slot);
        int elapsed = 0;
        int step = 10;
        while (elapsed < graceMilliseconds) {
            harmony_child_reap(slot);
            if (slot->state == HARMONY_CHILD_EXITED) {
                break;
            }
            Sleep((DWORD)step);
            elapsed += step;
        }
        if (slot->state == HARMONY_CHILD_RUNNING && slot->process != INVALID_HANDLE_VALUE) {
            TerminateProcess(slot->process, 0);
            WaitForSingleObject(slot->process, INFINITE);
            DWORD code = 0;
            if (GetExitCodeProcess(slot->process, &code)) {
                slot->exit_status = (int32_t)code;
            }
            slot->state = HARMONY_CHILD_EXITED;
        }
    }
    harmony_child_release(slot);
}

#else

/* ===================================================================== */
/* POSIX                                                                 */
/* ===================================================================== */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

extern char **environ;

#define HARMONY_CHILD_SLOTS 8

struct harmony_child_bytes {
    char *data;
    size_t length;
    size_t capacity;
};

static int harmony_bytes_reserve(struct harmony_child_bytes *buffer, size_t want) {
    if (buffer->capacity >= want) {
        return 1;
    }
    size_t next = buffer->capacity == 0 ? 256 : buffer->capacity * 2;
    while (next < want) {
        next *= 2;
    }
    char *grown = (char *)realloc(buffer->data, next);
    if (grown == 0) {
        return 0;
    }
    buffer->data = grown;
    buffer->capacity = next;
    return 1;
}

static void harmony_bytes_append(struct harmony_child_bytes *buffer, const char *bytes, size_t count) {
    if (count == 0) {
        return;
    }
    if (!harmony_bytes_reserve(buffer, buffer->length + count)) {
        return;
    }
    memcpy(buffer->data + buffer->length, bytes, count);
    buffer->length += count;
}

static char *harmony_strdup(const char *text) {
    if (text == 0) {
        return 0;
    }
    size_t length = strlen(text);
    char *copy = (char *)malloc(length + 1);
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length + 1);
    return copy;
}

struct harmony_child_slot {
    int used;
    int32_t state;
    pid_t pid;
    int reaped;
    int stdin_pipe;
    int stdout_pipe;
    int stderr_pipe;
    char *executable;
    char **arguments;
    int argument_count;
    int argument_capacity;
    char **environment;
    int environment_count;
    int environment_capacity;
    char *directory;
    struct harmony_child_bytes output;
    struct harmony_child_bytes line;
    struct harmony_child_bytes diagnostics;
    char *diagnostics_returned;
    struct harmony_child_bytes pending;
    int32_t exit_status;
    char *error;
};

static struct harmony_child_slot harmony_children[HARMONY_CHILD_SLOTS];

static struct harmony_child_slot *harmony_child_slot_claim(void) {
    for (int index = 0; index < HARMONY_CHILD_SLOTS; index += 1) {
        if (harmony_children[index].used == 0) {
            struct harmony_child_slot *slot = &harmony_children[index];
            memset(slot, 0, sizeof(*slot));
            slot->used = 1;
            slot->state = HARMONY_CHILD_UNSTARTED;
            slot->stdin_pipe = -1;
            slot->stdout_pipe = -1;
            slot->stderr_pipe = -1;
            return slot;
        }
    }
    return 0;
}

static struct harmony_child_slot *harmony_child_slot_at(int32_t handle) {
    if (handle <= 0 || handle > HARMONY_CHILD_SLOTS) {
        return 0;
    }
    struct harmony_child_slot *slot = &harmony_children[handle - 1];
    if (slot->used == 0) {
        return 0;
    }
    return slot;
}

static void harmony_child_ignore_sigpipe(void) {
    static int done = 0;
    if (done) {
        return;
    }
    done = 1;
    (void)signal(SIGPIPE, SIG_IGN);
}

static void harmony_child_flush(struct harmony_child_slot *slot) {
    if (slot->pending.length == 0) {
        return;
    }
    if (slot->stdin_pipe < 0) {
        slot->pending.length = 0;
        return;
    }
    size_t written = 0;
    while (written < slot->pending.length) {
        ssize_t step = write(slot->stdin_pipe, slot->pending.data + written,
                             slot->pending.length - written);
        if (step < 0) {
            if (errno == EINTR) {
                continue;
            }
            /* EAGAIN (would not take it) or EPIPE (child gone): stop, keep rest. */
            break;
        }
        written += (size_t)step;
    }
    if (written > 0) {
        memmove(slot->pending.data, slot->pending.data + written,
                slot->pending.length - written);
        slot->pending.length -= written;
    }
}

static void harmony_child_reap(struct harmony_child_slot *slot) {
    if (slot->reaped) {
        return;
    }
    int status = 0;
    pid_t answered = waitpid(slot->pid, &status, WNOHANG);
    if (answered == slot->pid) {
        slot->reaped = 1;
        if (WIFEXITED(status)) {
            slot->exit_status = (int32_t)WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            slot->exit_status = -1;
        }
        slot->state = HARMONY_CHILD_EXITED;
    } else if (answered == -1) {
        /* Already reaped, or never ours: either way it is gone. */
        slot->reaped = 1;
        slot->state = HARMONY_CHILD_EXITED;
    }
}

static void harmony_child_release(struct harmony_child_slot *slot) {
    if (slot->stdin_pipe >= 0) {
        (void)close(slot->stdin_pipe);
        slot->stdin_pipe = -1;
    }
    if (slot->stdout_pipe >= 0) {
        (void)close(slot->stdout_pipe);
        slot->stdout_pipe = -1;
    }
    if (slot->stderr_pipe >= 0) {
        (void)close(slot->stderr_pipe);
        slot->stderr_pipe = -1;
    }
    free(slot->executable);
    for (int i = 0; i < slot->argument_count; i++) {
        free(slot->arguments[i]);
    }
    free(slot->arguments);
    for (int i = 0; i < slot->environment_count; i++) {
        free(slot->environment[i]);
    }
    free(slot->environment);
    free(slot->directory);
    free(slot->output.data);
    free(slot->line.data);
    free(slot->diagnostics.data);
    free(slot->diagnostics_returned);
    free(slot->pending.data);
    free(slot->error);
    memset(slot, 0, sizeof(*slot));
}

int32_t harmony_child_open(const char *executable) {
    if (executable == 0 || executable[0] == 0) {
        return 0;
    }
    struct harmony_child_slot *slot = harmony_child_slot_claim();
    if (slot == 0) {
        return 0;
    }
    slot->executable = harmony_strdup(executable);
    if (slot->executable == 0) {
        slot->used = 0;
        return 0;
    }
    return (int32_t)((slot - harmony_children) + 1);
}

void harmony_child_argument(int32_t child, const char *argument) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (argument == 0) {
        return;
    }
    if (slot->argument_count + 1 >= slot->argument_capacity) {
        int capacity = slot->argument_capacity == 0 ? 8 : slot->argument_capacity * 2;
        char **grown = (char **)realloc(slot->arguments, (size_t)capacity * sizeof(char *));
        if (grown == 0) {
            return;
        }
        slot->arguments = grown;
        slot->argument_capacity = capacity;
    }
    char *copy = harmony_strdup(argument);
    if (copy == 0) {
        return;
    }
    slot->arguments[slot->argument_count] = copy;
    slot->argument_count += 1;
    slot->arguments[slot->argument_count] = 0;
}

void harmony_child_environment(int32_t child, const char *name, const char *value) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (name == 0 || name[0] == 0) {
        return;
    }
    size_t name_length = strlen(name);
    size_t value_length = value != 0 ? strlen(value) : 0;
    char *assignment = (char *)malloc(name_length + value_length + 2);
    if (assignment == 0) {
        return;
    }
    memcpy(assignment, name, name_length);
    assignment[name_length] = '=';
    memcpy(assignment + name_length + 1, value != 0 ? value : "", value_length);
    assignment[name_length + 1 + value_length] = 0;
    if (slot->environment_count + 1 >= slot->environment_capacity) {
        int capacity = slot->environment_capacity == 0 ? 8 : slot->environment_capacity * 2;
        char **grown = (char **)realloc(slot->environment, (size_t)capacity * sizeof(char *));
        if (grown == 0) {
            free(assignment);
            return;
        }
        slot->environment = grown;
        slot->environment_capacity = capacity;
    }
    slot->environment[slot->environment_count] = assignment;
    slot->environment_count += 1;
    slot->environment[slot->environment_count] = 0;
}

void harmony_child_directory(int32_t child, const char *path) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return;
    }
    if (path == 0) {
        return;
    }
    free(slot->directory);
    slot->directory = harmony_strdup(path);
}

int32_t harmony_child_start(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_UNSTARTED) {
        return 0;
    }
    if (slot->executable == 0) {
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup("no executable given");
        return 0;
    }
    harmony_child_ignore_sigpipe();

    int in_pipe[2];
    int out_pipe[2];
    int err_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0 || pipe(err_pipe) != 0) {
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup("could not create pipes");
        return 0;
    }
    if (fcntl(out_pipe[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(err_pipe[0], F_SETFL, O_NONBLOCK) != 0 ||
        fcntl(in_pipe[1], F_SETFL, O_NONBLOCK) != 0) {
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        (void)close(err_pipe[0]); (void)close(err_pipe[1]);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup("could not configure pipes");
        return 0;
    }

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        (void)close(err_pipe[0]); (void)close(err_pipe[1]);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup("could not prepare child");
        return 0;
    }
    (void)posix_spawn_file_actions_adddup2(&actions, in_pipe[0], STDIN_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
    (void)posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
    (void)posix_spawn_file_actions_addclose(&actions, in_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, in_pipe[1]);
    (void)posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, out_pipe[1]);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
    (void)posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (slot->directory != 0) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (posix_spawn_file_actions_addchdir_np(&actions, slot->directory) != 0) {
#pragma clang diagnostic pop
            posix_spawn_file_actions_destroy(&actions);
            (void)close(in_pipe[0]); (void)close(in_pipe[1]);
            (void)close(out_pipe[0]); (void)close(out_pipe[1]);
            (void)close(err_pipe[0]); (void)close(err_pipe[1]);
            slot->state = HARMONY_CHILD_FAILED;
            slot->error = harmony_strdup("could not set child directory");
            return 0;
        }
    }
#elif defined(__linux__)
    if (slot->directory != 0) {
        if (posix_spawn_file_actions_addchdir(&actions, slot->directory) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            (void)close(in_pipe[0]); (void)close(in_pipe[1]);
            (void)close(out_pipe[0]); (void)close(out_pipe[1]);
            (void)close(err_pipe[0]); (void)close(err_pipe[1]);
            slot->state = HARMONY_CHILD_FAILED;
            slot->error = harmony_strdup("could not set child directory");
            return 0;
        }
    }
#endif

    int argc = slot->argument_count + 2;
    char **argv = (char **)calloc((size_t)argc, sizeof(char *));
    if (argv == 0) {
        posix_spawn_file_actions_destroy(&actions);
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        (void)close(err_pipe[0]); (void)close(err_pipe[1]);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup("out of memory");
        return 0;
    }
    argv[0] = slot->executable;
    for (int i = 0; i < slot->argument_count; i++) {
        argv[i + 1] = slot->arguments[i];
    }
    argv[slot->argument_count + 1] = 0;

    char **envp = 0;
    if (slot->environment_count > 0) {
        int environ_count = 0;
        while (environ[environ_count] != 0) {
            environ_count += 1;
        }
        envp = (char **)calloc((size_t)(environ_count + slot->environment_count + 1),
                               sizeof(char *));
        if (envp == 0) {
            free(argv);
            posix_spawn_file_actions_destroy(&actions);
            (void)close(in_pipe[0]); (void)close(in_pipe[1]);
            (void)close(out_pipe[0]); (void)close(out_pipe[1]);
            (void)close(err_pipe[0]); (void)close(err_pipe[1]);
            slot->state = HARMONY_CHILD_FAILED;
            slot->error = harmony_strdup("out of memory");
            return 0;
        }
        for (int i = 0; i < environ_count; i++) {
            envp[i] = environ[i];
        }
        for (int i = 0; i < slot->environment_count; i++) {
            envp[environ_count + i] = slot->environment[i];
        }
        envp[environ_count + slot->environment_count] = 0;
    }

    pid_t pid = 0;
    int spawn_error = posix_spawn(&pid, slot->executable, &actions, 0, argv, envp);
    free(argv);
    free(envp);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        (void)close(in_pipe[0]); (void)close(in_pipe[1]);
        (void)close(out_pipe[0]); (void)close(out_pipe[1]);
        (void)close(err_pipe[0]); (void)close(err_pipe[1]);
        slot->state = HARMONY_CHILD_FAILED;
        slot->error = harmony_strdup(strerror(spawn_error));
        return 0;
    }

    slot->pid = pid;
    slot->stdin_pipe = in_pipe[1];
    slot->stdout_pipe = out_pipe[0];
    slot->stderr_pipe = err_pipe[0];
    (void)close(in_pipe[0]);
    (void)close(out_pipe[1]);
    (void)close(err_pipe[1]);
    slot->state = HARMONY_CHILD_RUNNING;
    return 1;
}

int32_t harmony_child_state(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return HARMONY_CHILD_UNSTARTED;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    return slot->state;
}

int32_t harmony_child_status(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return 0;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_EXITED) {
        return slot->exit_status;
    }
    return 0;
}

int32_t harmony_child_write(int32_t child, const char *text) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0 || slot->state != HARMONY_CHILD_RUNNING) {
        return 0;
    }
    if (text == 0) {
        return 0;
    }
    size_t total = strlen(text);
    if (total > 0) {
        if (!harmony_bytes_reserve(&slot->pending, slot->pending.length + total)) {
            return 0;
        }
        memcpy(slot->pending.data + slot->pending.length, text, total);
        slot->pending.length += total;
    }
    harmony_child_flush(slot);
    return slot->pending.length == 0 ? 1 : 0;
}

const char *harmony_child_line(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    if (slot->state == HARMONY_CHILD_UNSTARTED || slot->state == HARMONY_CHILD_FAILED) {
        return "";
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_RUNNING || slot->state == HARMONY_CHILD_EXITED) {
        harmony_child_flush(slot);
        for (;;) {
            char chunk[4096];
            ssize_t got = read(slot->stdout_pipe, chunk, sizeof(chunk));
            if (got > 0) {
                harmony_bytes_append(&slot->output, chunk, (size_t)got);
            } else if (got == 0) {
                break;
            } else {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
        }
    }
    size_t newline = 0;
    int found = 0;
    while (newline < slot->output.length) {
        if (slot->output.data[newline] == '\n') {
            found = 1;
            break;
        }
        newline += 1;
    }
    if (!found) {
        return "";
    }
    size_t line_length = newline;
    if (!harmony_bytes_reserve(&slot->line, line_length + 1)) {
        return "";
    }
    if (line_length > 0) {
        memcpy(slot->line.data, slot->output.data, line_length);
    }
    slot->line.data[line_length] = 0;
    slot->line.length = line_length;
    size_t consumed = line_length + 1;
    memmove(slot->output.data, slot->output.data + consumed, slot->output.length - consumed);
    slot->output.length -= consumed;
    return slot->line.data;
}

const char *harmony_child_diagnostics(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    free(slot->diagnostics_returned);
    slot->diagnostics_returned = 0;
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_reap(slot);
    }
    if (slot->state == HARMONY_CHILD_RUNNING || slot->state == HARMONY_CHILD_EXITED) {
        for (;;) {
            char chunk[4096];
            ssize_t got = read(slot->stderr_pipe, chunk, sizeof(chunk));
            if (got > 0) {
                harmony_bytes_append(&slot->diagnostics, chunk, (size_t)got);
            } else if (got == 0) {
                break;
            } else {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }
        }
    }
    if (slot->diagnostics.length == 0) {
        return "";
    }
    if (!harmony_bytes_reserve(&slot->diagnostics, slot->diagnostics.length + 1)) {
        return "";
    }
    slot->diagnostics.data[slot->diagnostics.length] = 0;
    slot->diagnostics_returned = slot->diagnostics.data;
    slot->diagnostics.data = 0;
    slot->diagnostics.length = 0;
    slot->diagnostics.capacity = 0;
    return slot->diagnostics_returned;
}

const char *harmony_child_error(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return "";
    }
    return slot->error != 0 ? slot->error : "";
}

void harmony_child_finish_input(int32_t child) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_flush(slot);
        if (slot->stdin_pipe >= 0) {
            (void)close(slot->stdin_pipe);
            slot->stdin_pipe = -1;
        }
    }
}

void harmony_child_stop(int32_t child, int32_t graceMilliseconds) {
    struct harmony_child_slot *slot = harmony_child_slot_at(child);
    if (slot == 0) {
        return;
    }
    if (slot->state == HARMONY_CHILD_RUNNING) {
        harmony_child_flush(slot);
        (void)kill(slot->pid, SIGTERM);
        int elapsed = 0;
        int step = 10;
        while (elapsed < graceMilliseconds) {
            int status = 0;
            pid_t answered = waitpid(slot->pid, &status, WNOHANG);
            if (answered == slot->pid) {
                if (WIFEXITED(status)) {
                    slot->exit_status = (int32_t)WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    slot->exit_status = -1;
                }
                slot->reaped = 1;
                slot->state = HARMONY_CHILD_EXITED;
                break;
            }
            if (answered == -1) {
                slot->reaped = 1;
                slot->state = HARMONY_CHILD_EXITED;
                break;
            }
            (void)usleep((useconds_t)step * 1000U);
            elapsed += step;
        }
        if (slot->state == HARMONY_CHILD_RUNNING) {
            (void)kill(slot->pid, SIGKILL);
            int status = 0;
            (void)waitpid(slot->pid, &status, 0);
            if (WIFSIGNALED(status)) {
                slot->exit_status = -1;
            } else if (WIFEXITED(status)) {
                slot->exit_status = (int32_t)WEXITSTATUS(status);
            }
            slot->reaped = 1;
            slot->state = HARMONY_CHILD_EXITED;
        }
    }
    harmony_child_release(slot);
}

#endif
