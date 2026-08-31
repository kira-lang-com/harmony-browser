#ifndef HARMONY_AGENT_HTTP_H
#define HARMONY_AGENT_HTTP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- HTTP ------------------------------------------------------------- */

/* States a request can be polled into. */
#define HARMONY_HTTP_IDLE     0
#define HARMONY_HTTP_SENDING  1
#define HARMONY_HTTP_STREAMING 2
#define HARMONY_HTTP_DONE     3
#define HARMONY_HTTP_FAILED   4

/* Opens a request. `method` is an uppercase HTTP verb. Answers a handle, or
   0 when the library could not take another request. */
int32_t harmony_http_open(const char *method, const char *url);

/* Adds one header. Legal only before `harmony_http_send`. */
void harmony_http_header(int32_t request, const char *name, const char *value);

/* Sets the whole request body. Legal only before `harmony_http_send`. */
void harmony_http_body(int32_t request, const char *body);

/* Sets how long the request may take before it fails, in milliseconds. Zero
   means no limit. Legal only before `harmony_http_send`. */
void harmony_http_timeout(int32_t request, int32_t milliseconds);

/* Begins the transfer. Returns at once. */
void harmony_http_send(int32_t request);

/* One of the HARMONY_HTTP_* states above. */
int32_t harmony_http_state(int32_t request);

/* The response status, or 0 before one has arrived. */
int32_t harmony_http_status(int32_t request);

/* Takes every response byte that has arrived since the last take, and empties
   the buffer. An empty string when nothing new arrived. The pointer belongs to
   the request and stays valid until the next call on the same request. */
const char *harmony_http_take(int32_t request);

/* A response header's value, or an empty string. Readable once the state has
   reached STREAMING. */
const char *harmony_http_header_value(int32_t request, const char *name);

/* Why the request failed, for a person. Empty unless the state is FAILED. */
const char *harmony_http_error(int32_t request);

/* Stops the transfer if it is running, and releases the handle. */
void harmony_http_close(int32_t request);

/* --- Clock ------------------------------------------------------------ */

/* Seconds since the Unix epoch. */
int64_t harmony_clock_unix_seconds(void);

/* Milliseconds on a monotonic clock with an unspecified origin. */
int64_t harmony_clock_monotonic_millis(void);

/* A value unlikely to repeat within a run, for naming a conversation or a
   JSON-RPC call. */
int64_t harmony_agent_unique(void);

#ifdef __cplusplus
}
#endif
#endif
