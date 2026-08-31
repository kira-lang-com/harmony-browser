#ifndef HARMONY_AGENT_CHILD_H
#define HARMONY_AGENT_CHILD_H

#include <stdint.h>

/* --- Child processes -------------------------------------------------- */

#define HARMONY_CHILD_UNSTARTED 0
#define HARMONY_CHILD_RUNNING   1
#define HARMONY_CHILD_EXITED    2
#define HARMONY_CHILD_FAILED    3

/* Describes a child that has not started yet. Answers a handle, or 0. */
int32_t harmony_child_open(const char *executable);

/* Appends one argument. Legal only before `harmony_child_start`. */
void harmony_child_argument(int32_t child, const char *argument);

/* Sets one environment variable for the child, on top of this process's
   environment. Legal only before `harmony_child_start`. */
void harmony_child_environment(int32_t child, const char *name, const char *value);

/* The directory the child starts in. Legal only before `harmony_child_start`. */
void harmony_child_directory(int32_t child, const char *path);

/* Starts it. Answers 1 when the child is running, 0 when it could not start,
   and `harmony_child_error` then says why. */
int32_t harmony_child_start(int32_t child);

/* One of the HARMONY_CHILD_* states. */
int32_t harmony_child_state(int32_t child);

/* The child's exit status once the state is EXITED, and 0 before that. */
int32_t harmony_child_status(int32_t child);

/* Writes to the child's standard input. `text` is written whole; the caller
   includes its own newline. Answers 0 when the pipe would not take it. */
int32_t harmony_child_write(int32_t child, const char *text);

/* One complete line from the child's standard output, without its newline, or
   an empty string when no complete line has arrived yet. Never blocks. The
   pointer belongs to the child and stays valid until the next call on it. */
const char *harmony_child_line(int32_t child);

/* Everything the child has written to standard error since the last call.
   A server explains its own failures there, and a person should see it. */
const char *harmony_child_diagnostics(int32_t child);

/* Why the child could not start, or why it stopped. */
const char *harmony_child_error(int32_t child);

/* Closes the child's input, so a server that exits on end-of-input can. */
void harmony_child_finish_input(int32_t child);

/* Ends the child if it is running, and releases the handle. Terminates it
   after `graceMilliseconds` if it has not left on its own. */
void harmony_child_stop(int32_t child, int32_t graceMilliseconds);

#endif
