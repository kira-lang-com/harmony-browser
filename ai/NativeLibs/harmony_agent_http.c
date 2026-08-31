/* harmony_agent_http.c -- non-blocking HTTP client for the Harmony agent.
 *
 * One worker thread owns every in-flight transfer; the public API functions
 * only touch shared state under a mutex. A response body is appended to a
 * growable buffer as it arrives, so a caller polling every frame sees streamed
 * bytes land as they arrive. Nothing in the public surface blocks.
 *
 *   macOS / Linux : libcurl's multi interface, driven by the worker thread
 *                   with curl_multi_poll. The write callback appends bytes and
 *                   the header callback records response headers. TLS is
 *                   libcurl's.
 *   Windows       : WinHTTP, run synchronously on the worker thread.
 *
 * Handles are small integers into a table this library owns, never pointers, so
 * nothing about the layout crosses into Kira. Handle 0 is "none". A call naming
 * a handle that was closed or never opened does nothing and answers a zero or an
 * empty string -- it must not crash, because the other side of this boundary is
 * a program that can be mid-shutdown.
 */

#include "harmony_agent_http.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wchar.h>
#include <process.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <pthread.h>
#include <curl/curl.h>
#endif

#define MAX_REQUESTS 256

typedef struct request request;

struct request {
    int32_t handle;
    int32_t state;
    int32_t status;
    int32_t timeout_ms;

    char *method;
    char *url;
    char *body;
    char *error_msg;

    /* Response body: growable, always NUL-terminated at resp_len so a slice
       returned by harmony_http_take is a valid C string. */
    char *resp_buf;
    size_t resp_len;
    size_t resp_cap;
    size_t resp_taken;

    /* Response headers: concatenated "name\0value\0..." pairs (name
       lower-cased) for case-insensitive lookup. */
    char *resp_headers;
    size_t resp_headers_len;
    size_t resp_headers_cap;

    /* Request headers, accumulated before send as "Name: value\r\n" lines. */
    char *header_block;
    size_t header_block_len;
    size_t header_block_cap;

    bool want_send;
    bool want_close;

#if defined(_WIN32)
    HINTERNET win_connect;
    HINTERNET win_request;
    bool started;
#else
    struct curl_slist *req_headers;
    CURL *easy;
    bool in_multi;
#endif
};

/* --------------------------------------------------------------------- */
/* Lock                                                                  */
/* --------------------------------------------------------------------- */

#if defined(_WIN32)
static CRITICAL_SECTION g_cs;
static void nt_init(void) __attribute__((constructor));
static void nt_init(void) { InitializeCriticalSection(&g_cs); }
#define LOCK() EnterCriticalSection(&g_cs)
#define UNLOCK() LeaveCriticalSection(&g_cs)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&g_lock)
#define UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

/* --------------------------------------------------------------------- */
/* Table                                                                 */
/* --------------------------------------------------------------------- */

static request g_requests[MAX_REQUESTS];

#if defined(_WIN32)
static HINTERNET g_session = NULL;
static volatile bool g_shutdown = false;
#else
static CURLM *g_multi = NULL;
static pthread_t g_worker;
#endif

static bool g_inited = false;

/* --------------------------------------------------------------------- */
/* Small growable buffers                                                */
/* --------------------------------------------------------------------- */

static void buf_append(request *r, const char *data, size_t n) {
    if (n == 0) return;
    if (r->resp_len + n + 1 > r->resp_cap) {
        size_t base = r->resp_len + n + 1;
        size_t newcap = r->resp_cap ? r->resp_cap * 2 : 256;
        while (newcap < base) newcap *= 2;
        char *nb = (char *)realloc(r->resp_buf, newcap);
        if (!nb) return;
        r->resp_buf = nb;
        r->resp_cap = newcap;
    }
    memcpy(r->resp_buf + r->resp_len, data, n);
    r->resp_len += n;
    r->resp_buf[r->resp_len] = 0;
}

static void hdr_append(request *r, const char *name, size_t nlen,
                       const char *val, size_t vlen) {
    size_t need = nlen + 1 + vlen + 1;
    if (r->resp_headers_len + need > r->resp_headers_cap) {
        size_t base = r->resp_headers_len + need;
        size_t newcap = r->resp_headers_cap ? r->resp_headers_cap * 2 : 256;
        while (newcap < base) newcap *= 2;
        char *nb = (char *)realloc(r->resp_headers, newcap);
        if (!nb) return;
        r->resp_headers = nb;
        r->resp_headers_cap = newcap;
    }
    char *dst = r->resp_headers + r->resp_headers_len;
    for (size_t i = 0; i < nlen; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[nlen] = 0;
    memcpy(dst + nlen + 1, val, vlen);
    dst[nlen + 1 + vlen] = 0;
    r->resp_headers_len += need;
}

static void header_block_append(request *r, const char *name, size_t nl,
                                const char *val, size_t vl) {
    static const char sep[] = ": ";
    static const char crlf[] = "\r\n";
    size_t need = nl + 2 + vl + 2 + 1; /* content + NUL */
    if (r->header_block_len + need > r->header_block_cap) {
        size_t base = r->header_block_len + need;
        size_t newcap = r->header_block_cap ? r->header_block_cap * 2 : 256;
        while (newcap < base) newcap *= 2;
        char *nb = (char *)realloc(r->header_block, newcap);
        if (!nb) return;
        r->header_block = nb;
        r->header_block_cap = newcap;
    }
    char *d = r->header_block + r->header_block_len;
    memcpy(d, name, nl); d += nl;
    memcpy(d, sep, 2); d += 2;
    memcpy(d, val, vl); d += vl;
    memcpy(d, crlf, 2); d += 2;
    *d = 0;
    r->header_block_len += (need - 1);
}

static void set_error(request *r, const char *msg) {
    free(r->error_msg);
    r->error_msg = msg ? strdup(msg) : NULL;
}

/* --------------------------------------------------------------------- */
/* Lookup                                                                */
/* --------------------------------------------------------------------- */

/* Caller must hold the lock. */
static request *lookup(int32_t h) {
    if (h < 1 || h > (int32_t)MAX_REQUESTS) return NULL;
    request *r = &g_requests[h - 1];
    if (r->handle != h) return NULL;
    return r;
}

/* --------------------------------------------------------------------- */
/* Lifetime                                                              */
/* --------------------------------------------------------------------- */

static void free_common(request *r) {
    free(r->method); r->method = NULL;
    free(r->url); r->url = NULL;
    free(r->body); r->body = NULL;
    free(r->error_msg); r->error_msg = NULL;
    free(r->resp_buf); r->resp_buf = NULL;
    r->resp_len = 0; r->resp_cap = 0; r->resp_taken = 0;
    free(r->resp_headers); r->resp_headers = NULL;
    r->resp_headers_len = 0; r->resp_headers_cap = 0;
    free(r->header_block); r->header_block = NULL;
    r->header_block_len = 0; r->header_block_cap = 0;
    r->status = 0;
    r->timeout_ms = 0;
    r->state = HARMONY_HTTP_IDLE;
    r->want_send = false;
    r->want_close = false;
}

#if defined(_WIN32)
static void cleanup_request(request *r) {
    if (r->win_request) { WinHttpCloseHandle(r->win_request); r->win_request = NULL; }
    if (r->win_connect) { WinHttpCloseHandle(r->win_connect); r->win_connect = NULL; }
    free_common(r);
    r->started = false;
    r->handle = 0;
}
#else
static void cleanup_request(request *r) {
    if (r->easy) { curl_easy_cleanup(r->easy); r->easy = NULL; }
    if (r->req_headers) { curl_slist_free_all(r->req_headers); r->req_headers = NULL; }
    free_common(r);
    r->in_multi = false;
    r->handle = 0;
}
#endif

/* --------------------------------------------------------------------- */
/* Worker thread                                                         */
/* --------------------------------------------------------------------- */

#if defined(_WIN32)
static unsigned __stdcall worker_main(void *arg);
#else
static void *worker_main(void *arg);
#endif

static void ensure_runtime(void) {
#if defined(_WIN32)
    LOCK();
    if (!g_inited) {
        g_session = WinHttpOpen(L"harmony-agent/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        g_inited = true;
        _beginthreadex(NULL, 0, worker_main, NULL, 0, NULL);
    }
    UNLOCK();
#else
    LOCK();
    if (!g_inited) {
        curl_global_init(CURL_GLOBAL_ALL);
        g_multi = curl_multi_init();
        g_inited = true;
        pthread_create(&g_worker, NULL, worker_main, NULL);
    }
    UNLOCK();
#endif
}

#if defined(_WIN32)
/* Implemented below, in the WinHTTP section. */
#else

/* ---- libcurl multi implementation ----------------------------------- */

static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    request *r = (request *)userdata;
    size_t total = size * nmemb;
    if (total > 0) {
        LOCK();
        buf_append(r, ptr, total);
        if (r->state == HARMONY_HTTP_SENDING || r->state == HARMONY_HTTP_IDLE)
            r->state = HARMONY_HTTP_STREAMING;
        UNLOCK();
    }
    return total;
}

static size_t header_cb(char *buffer, size_t size, size_t nitems, void *userdata) {
    request *r = (request *)userdata;
    size_t total = size * nitems;
    if (total == 0) return total;
    size_t len = total;
    while (len > 0 && (buffer[len - 1] == '\r' || buffer[len - 1] == '\n')) len--;
    if (len == 0) {
        /* End of headers: capture the status and mark streaming. */
        LOCK();
        if (r->status == 0) {
            long code = 0;
            curl_easy_getinfo(r->easy, CURLINFO_RESPONSE_CODE, &code);
            r->status = (int32_t)code;
        }
        if (r->state == HARMONY_HTTP_SENDING || r->state == HARMONY_HTTP_IDLE)
            r->state = HARMONY_HTTP_STREAMING;
        UNLOCK();
        return total;
    }
    char *colon = (char *)memchr(buffer, ':', len);
    if (!colon) return total; /* status line or malformed */
    size_t nlen = (size_t)(colon - buffer);
    const char *val = colon + 1;
    size_t vlen = len - nlen - 1;
    while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) vlen--;
    LOCK();
    hdr_append(r, buffer, nlen, val, vlen);
    UNLOCK();
    return total;
}

static struct curl_slist *build_slist(const char *block, size_t len) {
    struct curl_slist *list = NULL;
    size_t i = 0;
    while (i < len) {
        size_t e = i;
        while (e < len && block[e] != '\r' && block[e] != '\n') e++;
        if (e > i) {
            size_t l = e - i;
            char *line = (char *)malloc(l + 1);
            if (line) {
                memcpy(line, block + i, l);
                line[l] = 0;
                list = curl_slist_append(list, line);
                free(line);
            }
        }
        while (e < len && (block[e] == '\r' || block[e] == '\n')) e++;
        i = e;
    }
    return list;
}

static void configure_easy(request *r) {
    CURL *e = r->easy;
    (void)curl_easy_setopt(e, CURLOPT_URL, r->url);
    (void)curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, r->method);
    if (r->body) {
        (void)curl_easy_setopt(e, CURLOPT_POSTFIELDS, r->body);
        (void)curl_easy_setopt(e, CURLOPT_POSTFIELDSIZE, (long)strlen(r->body));
    }
    if (r->header_block_len > 0) {
        r->req_headers = build_slist(r->header_block, r->header_block_len);
        if (r->req_headers)
            (void)curl_easy_setopt(e, CURLOPT_HTTPHEADER, r->req_headers);
    }
    (void)curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, write_cb);
    (void)curl_easy_setopt(e, CURLOPT_WRITEDATA, (void *)r);
    (void)curl_easy_setopt(e, CURLOPT_HEADERFUNCTION, header_cb);
    (void)curl_easy_setopt(e, CURLOPT_HEADERDATA, (void *)r);
    (void)curl_easy_setopt(e, CURLOPT_FOLLOWLOCATION, 1L);
    (void)curl_easy_setopt(e, CURLOPT_MAXREDIRS, 10L);
    (void)curl_easy_setopt(e, CURLOPT_ACCEPT_ENCODING, "identity");
    (void)curl_easy_setopt(e, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(e, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_setopt(e, CURLOPT_NOPROGRESS, 1L);
    (void)curl_easy_setopt(e, CURLOPT_PRIVATE, (void *)r);
    if (r->timeout_ms > 0)
        (void)curl_easy_setopt(e, CURLOPT_TIMEOUT_MS, (long)r->timeout_ms);
}

static void *worker_main(void *arg) {
    (void)arg;
    for (;;) {
        LOCK();
        for (int i = 0; i < MAX_REQUESTS; i++) {
            request *r = &g_requests[i];
            if (r->handle == 0) continue;
            if (r->want_close) {
                if (r->in_multi) curl_multi_remove_handle(g_multi, r->easy);
                cleanup_request(r);
                continue;
            }
            if (r->want_send && !r->in_multi) {
                configure_easy(r);
                if (curl_multi_add_handle(g_multi, r->easy) == CURLM_OK) {
                    r->in_multi = true;
                    r->want_send = false;
                    r->state = HARMONY_HTTP_SENDING;
                }
            }
        }
        UNLOCK();

        int running = 0;
        curl_multi_perform(g_multi, &running);

        CURLMsg *msg;
        int msgs_left = 0;
        while ((msg = curl_multi_info_read(g_multi, &msgs_left)) != NULL) {
            (void)msgs_left;
            if (msg->msg == CURLMSG_DONE) {
                char *priv = NULL;
                curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
                request *r = (request *)priv;
                if (r) {
                    LOCK();
                    if (msg->data.result == CURLE_OK) {
                        r->state = HARMONY_HTTP_DONE;
                    } else {
                        r->state = HARMONY_HTTP_FAILED;
                        set_error(r, curl_easy_strerror(msg->data.result));
                    }
                    r->in_multi = false;
                    curl_multi_remove_handle(g_multi, r->easy);
                    UNLOCK();
                }
            }
        }

        int numfds = 0;
        curl_multi_poll(g_multi, NULL, 0, 20, &numfds);
        (void)numfds;
    }
    return NULL;
}

#endif /* !_WIN32 */

/* --------------------------------------------------------------------- */
/* Public API                                                            */
/* --------------------------------------------------------------------- */

int32_t harmony_http_open(const char *method, const char *url) {
    if (!method || !url) return 0;
    ensure_runtime();
    LOCK();
    int idx = -1;
    for (int i = 0; i < MAX_REQUESTS; i++) {
        if (g_requests[i].handle == 0) { idx = i; break; }
    }
    if (idx < 0) { UNLOCK(); return 0; }
    request *r = &g_requests[idx];
    r->handle = (int32_t)(idx + 1);
    r->state = HARMONY_HTTP_IDLE;
    r->status = 0;
    r->timeout_ms = 0;
    r->method = strdup(method);
    r->url = strdup(url);
    r->body = NULL;
    r->error_msg = NULL;
    r->resp_buf = NULL; r->resp_len = 0; r->resp_cap = 0; r->resp_taken = 0;
    r->resp_headers = NULL; r->resp_headers_len = 0; r->resp_headers_cap = 0;
    r->header_block = NULL; r->header_block_len = 0; r->header_block_cap = 0;
    r->want_send = false;
    r->want_close = false;
#if defined(_WIN32)
    r->win_connect = NULL;
    r->win_request = NULL;
    r->started = false;
#else
    r->req_headers = NULL;
    r->easy = curl_easy_init();
    r->in_multi = false;
    if (!r->easy) {
        free(r->method); r->method = NULL;
        free(r->url); r->url = NULL;
        r->handle = 0;
        UNLOCK();
        return 0;
    }
#endif
    UNLOCK();
    return r->handle;
}

void harmony_http_header(int32_t req, const char *name, const char *value) {
    if (!name || !value) return;
    LOCK();
    request *r = lookup(req);
    if (r && r->state == HARMONY_HTTP_IDLE)
        header_block_append(r, name, strlen(name), value, strlen(value));
    UNLOCK();
}

void harmony_http_body(int32_t req, const char *body) {
    if (!body) return;
    LOCK();
    request *r = lookup(req);
    if (r && r->state == HARMONY_HTTP_IDLE) {
        free(r->body);
        r->body = strdup(body);
    }
    UNLOCK();
}

void harmony_http_timeout(int32_t req, int32_t milliseconds) {
    LOCK();
    request *r = lookup(req);
    if (r && r->state == HARMONY_HTTP_IDLE) r->timeout_ms = milliseconds;
    UNLOCK();
}

void harmony_http_send(int32_t req) {
    LOCK();
    request *r = lookup(req);
    if (r && r->state == HARMONY_HTTP_IDLE) {
        r->want_send = true;
        r->state = HARMONY_HTTP_SENDING;
    }
    UNLOCK();
}

int32_t harmony_http_state(int32_t req) {
    LOCK();
    request *r = lookup(req);
    int32_t s = r ? r->state : HARMONY_HTTP_IDLE;
    UNLOCK();
    return s;
}

int32_t harmony_http_status(int32_t req) {
    LOCK();
    request *r = lookup(req);
    int32_t st = r ? r->status : 0;
    UNLOCK();
    return st;
}

const char *harmony_http_take(int32_t req) {
    LOCK();
    request *r = lookup(req);
    if (!r) { UNLOCK(); return ""; }
    if (r->resp_len > r->resp_taken) {
        const char *p = r->resp_buf + r->resp_taken;
        r->resp_taken = r->resp_len;
        UNLOCK();
        return p;
    }
    UNLOCK();
    return "";
}

const char *harmony_http_header_value(int32_t req, const char *name) {
    if (!name) return "";
    LOCK();
    request *r = lookup(req);
    if (!r || r->state < HARMONY_HTTP_STREAMING) { UNLOCK(); return ""; }
    size_t i = 0;
    size_t nlen = strlen(name);
    while (i + 1 < r->resp_headers_len) {
        const char *hname = r->resp_headers + i;
        size_t hn = strlen(hname);
        const char *hval = hname + hn + 1;
        size_t hv = strlen(hval);
        if (hn == nlen && strncmp(hname, name, nlen) == 0) {
            UNLOCK();
            return hval;
        }
        i += hn + 1 + hv + 1;
    }
    UNLOCK();
    return "";
}

const char *harmony_http_error(int32_t req) {
    LOCK();
    request *r = lookup(req);
    const char *e = (r && r->state == HARMONY_HTTP_FAILED && r->error_msg)
                        ? r->error_msg
                        : "";
    UNLOCK();
    return e;
}

void harmony_http_close(int32_t req) {
    LOCK();
    request *r = lookup(req);
    if (r) r->want_close = true;
    UNLOCK();
}

/* --------------------------------------------------------------------- */
/* WinHTTP implementation                                                */
/* --------------------------------------------------------------------- */

#if defined(_WIN32)

static void transfer_win(request *r);

static unsigned __stdcall worker_main(void *arg) {
    (void)arg;
    for (;;) {
        request *todo = NULL;
        LOCK();
        for (int i = 0; i < MAX_REQUESTS; i++) {
            request *r = &g_requests[i];
            if (r->handle == 0) continue;
            if (r->want_close) { cleanup_request(r); continue; }
            if (r->want_send && !r->started) {
                r->started = true;
                todo = r;
                break;
            }
        }
        UNLOCK();
        if (todo) transfer_win(todo);
        Sleep(5);
        if (g_shutdown) break;
    }
    return 0;
}

static wchar_t *to_wide(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void transfer_win(request *r) {
    r->state = HARMONY_HTTP_SENDING;
    wchar_t *wide_url = to_wide(r->url);
    if (!wide_url) { set_error(r, "bad url"); r->state = HARMONY_HTTP_FAILED; return; }

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.dwSchemeLength = (DWORD)-1;
    uc.dwHostNameLength = (DWORD)-1;
    uc.dwUrlPathLength = (DWORD)-1;
    if (!WinHttpCrackUrl(wide_url, 0, 0, &uc)) {
        free(wide_url);
        set_error(r, "could not parse url");
        r->state = HARMONY_HTTP_FAILED;
        return;
    }

    int secure = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? 1 : 0;
    wchar_t *host = (wchar_t *)malloc((size_t)uc.dwHostNameLength * sizeof(wchar_t) + sizeof(wchar_t));
    if (!host) { free(wide_url); set_error(r, "out of memory"); r->state = HARMONY_HTTP_FAILED; return; }
    memcpy(host, uc.lpszHostName, (size_t)uc.dwHostNameLength * sizeof(wchar_t));
    host[uc.dwHostNameLength] = 0;

    INTERNET_PORT port = uc.nPort ? uc.nPort : (secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);
    r->win_connect = WinHttpConnect(g_session, host, port, 0);
    free(host);
    if (!r->win_connect) { free(wide_url); set_error(r, "connect failed"); r->state = HARMONY_HTTP_FAILED; return; }

    wchar_t *method_w = to_wide(r->method);
    wchar_t *path_w = NULL;
    if (uc.dwUrlPathLength > 0) {
        path_w = (wchar_t *)malloc((size_t)uc.dwUrlPathLength * sizeof(wchar_t) + sizeof(wchar_t));
        if (path_w) {
            memcpy(path_w, uc.lpszUrlPath, (size_t)uc.dwUrlPathLength * sizeof(wchar_t));
            path_w[uc.dwUrlPathLength] = 0;
        }
    }
    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    r->win_request = WinHttpOpenRequest(r->win_connect, method_w ? method_w : L"GET",
                                       path_w ? path_w : L"/", NULL, NULL, NULL, flags);
    free(method_w);
    free(path_w);
    free(wide_url);
    if (!r->win_request) { set_error(r, "open request failed"); r->state = HARMONY_HTTP_FAILED; return; }

    if (r->timeout_ms > 0) {
        DWORD ms = (DWORD)r->timeout_ms;
        WinHttpSetOption(r->win_request, WINHTTP_OPTION_RESOLVE_TIMEOUT, &ms, sizeof(ms));
        WinHttpSetOption(r->win_request, WINHTTP_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
        WinHttpSetOption(r->win_request, WINHTTP_OPTION_SEND_TIMEOUT, &ms, sizeof(ms));
        WinHttpSetOption(r->win_request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    }

    wchar_t *headers_w = NULL;
    if (r->header_block_len > 0) {
        headers_w = to_wide(r->header_block);
    }

    DWORD body_len = r->body ? (DWORD)strlen(r->body) : 0;
    BOOL ok = WinHttpSendRequest(r->win_request,
                                headers_w ? headers_w : WINHTTP_NO_ADDITIONAL_HEADERS,
                                headers_w ? (DWORD)wcslen(headers_w) : 0,
                                WINHTTP_NO_REQUEST_DATA, 0, body_len, 0);
    free(headers_w);
    if (!ok) { set_error(r, "send failed"); r->state = HARMONY_HTTP_FAILED; WinHttpCloseHandle(r->win_request); r->win_request = NULL; return; }

    if (body_len > 0) {
        DWORD written = 0;
        if (!WinHttpWriteData(r->win_request, r->body, body_len, &written)) {
            set_error(r, "write failed");
            r->state = HARMONY_HTTP_FAILED;
            WinHttpCloseHandle(r->win_request); r->win_request = NULL;
            return;
        }
    }

    if (!WinHttpReceiveResponse(r->win_request, NULL)) {
        set_error(r, "receive failed");
        r->state = HARMONY_HTTP_FAILED;
        WinHttpCloseHandle(r->win_request); r->win_request = NULL;
        return;
    }

    DWORD code = 0;
    DWORD cb = sizeof(code);
    if (WinHttpQueryHeaders(r->win_request,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &code, &cb, NULL)) {
        r->status = (int32_t)code;
    }
    r->state = HARMONY_HTTP_STREAMING;

    /* Capture response headers (raw, one per line). */
    DWORD hdr_size = 0;
    WinHttpQueryHeaders(r->win_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                        WINHTTP_HEADER_NAME_BY_INDEX, NULL, &hdr_size,
                        WINHTTP_NO_HEADER_INDEX);
    if (hdr_size > 0) {
        wchar_t *raw = (wchar_t *)malloc((size_t)hdr_size);
        if (raw) {
            if (WinHttpQueryHeaders(r->win_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                    WINHTTP_HEADER_NAME_BY_INDEX, raw, &hdr_size,
                                    WINHTTP_NO_HEADER_INDEX)) {
                int need = WideCharToMultiByte(CP_UTF8, 0, raw, -1, NULL, 0, NULL, NULL);
                if (need > 0) {
                    char *mb = (char *)malloc((size_t)need);
                    if (mb) {
                        WideCharToMultiByte(CP_UTF8, 0, raw, -1, mb, need, NULL, NULL);
                        /* Store each "Name: value" line as a lower-cased pair. */
                        char *line = mb;
                        while (*line) {
                            char *nl = line;
                            while (*nl && *nl != '\r' && *nl != '\n') nl++;
                            if (nl > line) {
                                size_t ll = (size_t)(nl - line);
                                char *colon = (char *)memchr(line, ':', ll);
                                if (colon) {
                                    size_t nlen = (size_t)(colon - line);
                                    const char *val = colon + 1;
                                    size_t vlen = ll - nlen - 1;
                                    while (vlen > 0 && (*val == ' ' || *val == '\t')) { val++; vlen--; }
                                    while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) vlen--;
                                    LOCK();
                                    hdr_append(r, line, nlen, val, vlen);
                                    UNLOCK();
                                }
                            }
                            while (*nl == '\r' || *nl == '\n') nl++;
                            line = nl;
                        }
                        free(mb);
                    }
                }
            }
            free(raw);
        }
    }

    /* Read the body in a loop, appending as bytes land. */
    for (;;) {
        if (r->want_close) { set_error(r, "canceled"); r->state = HARMONY_HTTP_FAILED; break; }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(r->win_request, &avail)) {
            set_error(r, "read failed");
            r->state = HARMONY_HTTP_FAILED;
            break;
        }
        if (avail == 0) { r->state = HARMONY_HTTP_DONE; break; }
        /* Cap each read so a huge avail does not overallocate at once. */
        DWORD to_read = avail > 65536 ? 65536 : avail;
        char *chunk = (char *)malloc((size_t)to_read);
        if (!chunk) { set_error(r, "out of memory"); r->state = HARMONY_HTTP_FAILED; break; }
        DWORD got = 0;
        if (!WinHttpReadData(r->win_request, chunk, to_read, &got) || got == 0) {
            free(chunk);
            set_error(r, "read failed");
            r->state = HARMONY_HTTP_FAILED;
            break;
        }
        LOCK();
        buf_append(r, chunk, (size_t)got);
        UNLOCK();
        free(chunk);
    }

    if (r->win_request) { WinHttpCloseHandle(r->win_request); r->win_request = NULL; }
    if (r->win_connect) { WinHttpCloseHandle(r->win_connect); r->win_connect = NULL; }
}

#endif /* _WIN32 */
