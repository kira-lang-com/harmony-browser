#ifndef HARMONY_CORE_H
#define HARMONY_CORE_H

#include <stdint.h>

/* Starting a component, and knowing whether it is still there.
 *
 * This is the only thing core needs from the platform. Everything else it does
 * -- deciding what is installed, naming an endpoint, encoding a message -- is
 * Kira: `fileExists` answers whether a component is on disk, KiraIpc carries
 * what it says, and neither needs help.
 *
 * A component is a SEPARATE EXECUTABLE, launched by the shell and connecting
 * back to it. That is what makes the install story work: Harmony Browser ships a
 * shell and a browser, the AI runtime ships a shell and an ai, and a machine
 * with both has one of each shell and both components. Nothing is dynamically
 * loaded into the shell, so a component that is absent is absent -- not a
 * missing symbol, not a failed load, just an executable that is not there. */

/* A launched component. Zero is never one: it is what a failed start answers. */
typedef int32_t harmony_process;

/* Start `executable`, passing it the one argument every component takes: the
 * endpoint name of the shell that started it.
 *
 * Nothing is inherited that the component should not have, and the child is
 * detached from the parent's controlling terminal on the platforms that have
 * one. A component writes to its own log, not to the shell's stdout. */
harmony_process harmony_process_start(const char *executable, const char *endpoint);

/* Whether the component is still running. A component that has exited answers 0
 * and keeps answering it until it is released. */
int32_t harmony_process_alive(harmony_process process);

/* Ask the component to exit, the way the platform asks politely. A component
 * that ignores it is the component's problem: the shell does not get to decide
 * how long a page's teardown takes, and killing one mid-write is how a profile
 * ends up half-written. */
void harmony_process_stop(harmony_process process);

/* Set a variable in this process's environment, which a child inherits.
 *
 * The shell uses it to tell every component the appearance it is drawing in.
 * Two processes each asking the desktop what scheme it is in can disagree --
 * they read different things, at different moments, over different session
 * connections -- and a sidebar in a different scheme from the window beside it
 * is the visible result. One process decides, and the others are told. */
void harmony_process_setenv(const char *name, const char *value);

/* This program's own process id.
 *
 * A shell names its endpoint after it, so two shells running at once -- an
 * ordinary thing, two windows -- publish two names rather than fighting over
 * one. Nothing else uses it, and nothing reads it as anything but a number that
 * differs between live processes. */
int32_t harmony_process_self(void);

/* The directory the running program's own executable sits in.
 *
 * Every platform answers this differently and none of them answers it through
 * the C standard library, so the four answers live behind one call. It is what
 * "installed beside the shell" means: a component is looked for HERE, and
 * nowhere a registry, a settings file, or a `PATH` could disagree about.
 *
 * `argv[0]` is not an answer. It is whatever the parent chose to pass, it may be
 * a bare name resolved through `PATH`, and on every platform here there is a way
 * to ask the system instead.
 *
 * The returned pointer addresses storage owned by this library and stays valid
 * until the next call on the same thread, which is as long as the seam needs.
 * An empty string means the platform would not say. */
const char *harmony_executable_directory(void);

/* Reap the record. Safe on a process that is still running -- it stops being
 * watched, not stopped. */
void harmony_process_release(harmony_process process);

#endif
