#include "harmony_permissions_internal.h"

#include "harmony_permissions.h"
#include "harmony_tabs_embed.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace harmony_permissions {
namespace {

// One request WebKit is waiting on. The request object is retained for as long
// as it sits here: the callback that delivered it returns immediately, and the
// reference WebKit passed dies with that call.
//
// `resolved` is set the moment the host answers, from the host's own thread.
// The answer is carried out on the WebKit thread, so the request is still here
// for a frame or two afterwards, and this is what keeps the host from being
// asked about it again in between.
struct PendingRequest {
    int id { 0 };
    int kind { 0 };
    RequestKind shape { RequestKind::Geolocation };
    std::string origin;
    WKTypeRef request { nullptr };
    WKPageRef page { nullptr };
    bool resolved { false };
};

WebKitApi g_api;
std::mutex g_apiMutex;
// Written once, under the mutex, and read from both threads without it: the
// table is only ever read after this says it is whole.
std::atomic<bool> g_apiReady { false };

std::mutex g_errorMutex;
std::string g_error;

std::mutex g_policyMutex;
std::vector<PolicyEntry> g_policy;

std::mutex g_queueMutex;
std::deque<PendingRequest> g_queue;
std::atomic<int> g_nextRequestId { 1 };

std::mutex g_resolutionMutex;
std::vector<std::pair<int, int>> g_resolutions;

std::atomic<bool> g_shutdown { false };

template<typename Function>
bool loadFunction(const char* name, Function& function)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
    if (!function) {
        setError(std::string("the WebKit runtime exports no ") + name);
        return false;
    }
    return true;
}

// Runs on the WebKit thread: answers everything the host has decided.
void drainResolutions(void*)
{
    std::vector<std::pair<int, int>> answers;
    {
        std::lock_guard<std::mutex> lock(g_resolutionMutex);
        answers.swap(g_resolutions);
    }
    for (const auto& answer : answers) {
        if (answer.first == 0)
            denyEveryRequest();
        else
            answerRequest(answer.first, answer.second);
    }
}

void deliverGeolocation(void*)
{
    deliverGeolocationState();
}

void refreshNotificationPolicy(void*)
{
    pushNotificationPolicy();
}

void shutdownOnWebKitThread(void*)
{
    denyEveryRequest();
    geolocationSourceShutdown();
    notificationSurfaceShutdown();
}

// Releases a request WebKit will never hear about again. Denying first is what
// keeps a dropped prompt from hanging a page's promise.
void denyAndRelease(const PendingRequest& pending)
{
    const auto& api = webKitApi();
    if (!pending.request)
        return;

    switch (pending.shape) {
    case RequestKind::Geolocation:
        if (api.geolocationPermissionRequestDeny)
            api.geolocationPermissionRequestDeny(pending.request);
        break;
    case RequestKind::Notification:
        if (api.notificationPermissionRequestDeny)
            api.notificationPermissionRequestDeny(pending.request);
        break;
    case RequestKind::UserMedia:
        if (api.userMediaPermissionRequestDeny)
            api.userMediaPermissionRequestDeny(pending.request, kUserMediaPermissionDenied);
        break;
    }

    if (api.release)
        api.release(pending.request);
}

// The first device uid in one of the request's device arrays, as a +1 WKString,
// or null when the request wants nothing from that array. WebKit takes an empty
// name as "no device of this kind", which is what a request for the camera
// alone should say about the microphone.
WKStringRef firstDeviceUID(WKArrayRef devices)
{
    const auto& api = webKitApi();
    if (!devices || !api.arrayGetSize || !api.arrayGetItemAtIndex)
        return nullptr;
    if (api.arrayGetSize(devices) == 0)
        return nullptr;
    WKTypeRef first = api.arrayGetItemAtIndex(devices, 0);
    if (!first)
        return nullptr;
    api.retain(first);
    return first;
}

void allowAndRelease(const PendingRequest& pending)
{
    const auto& api = webKitApi();
    if (!pending.request)
        return;

    switch (pending.shape) {
    case RequestKind::Geolocation:
        if (api.geolocationPermissionRequestAllow)
            api.geolocationPermissionRequestAllow(pending.request);
        break;
    case RequestKind::Notification:
        if (api.notificationPermissionRequestAllow)
            api.notificationPermissionRequestAllow(pending.request);
        break;
    case RequestKind::UserMedia: {
        if (!api.userMediaPermissionRequestAllow)
            break;
        WKStringRef audio = nullptr;
        WKStringRef video = nullptr;
        if (api.userMediaRequiresMicrophoneCapture && api.userMediaRequiresMicrophoneCapture(pending.request))
            audio = firstDeviceUID(api.userMediaAudioDeviceUIDs ? api.userMediaAudioDeviceUIDs(pending.request) : nullptr);
        if (api.userMediaRequiresCameraCapture && api.userMediaRequiresCameraCapture(pending.request))
            video = firstDeviceUID(api.userMediaVideoDeviceUIDs ? api.userMediaVideoDeviceUIDs(pending.request) : nullptr);
        if (api.userMediaRequiresDisplayCapture && api.userMediaRequiresDisplayCapture(pending.request) && !video)
            video = firstDeviceUID(api.userMediaVideoDeviceUIDs ? api.userMediaVideoDeviceUIDs(pending.request) : nullptr);

        WKStringRef emptyName = api.stringCreateWithUTF8CString("");
        api.userMediaPermissionRequestAllow(pending.request, audio ? audio : emptyName, video ? video : emptyName);
        if (audio)
            api.release(audio);
        if (video)
            api.release(video);
        if (emptyName)
            api.release(emptyName);
        break;
    }
    }

    if (api.release)
        api.release(pending.request);
}

} // namespace

// --- WebKit entry points -----------------------------------------------------

bool ensureWebKitApi()
{
    if (g_apiReady.load(std::memory_order_acquire))
        return true;

    std::lock_guard<std::mutex> lock(g_apiMutex);
    if (g_apiReady.load(std::memory_order_relaxed))
        return true;

    // The tabs registry owns the runtime, so its symbol seam answers nothing
    // until the engine is up. Nothing is remembered as a failure: the browser
    // asks again on the next frame, and by then the engine is usually loaded.
    if (!hb_tabs_webkit_symbol("WKRelease")) {
        setError("the WebKit runtime is not loaded in this process yet");
        return false;
    }

    WebKitApi api;
    bool ok = true;
    ok &= loadFunction("WKRetain", api.retain);
    ok &= loadFunction("WKRelease", api.release);
    ok &= loadFunction("WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString);
    ok &= loadFunction("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize);
    ok &= loadFunction("WKStringGetUTF8CString", api.stringGetUTF8CString);
    ok &= loadFunction("WKSecurityOriginCopyProtocol", api.securityOriginCopyProtocol);
    ok &= loadFunction("WKSecurityOriginCopyHost", api.securityOriginCopyHost);
    ok &= loadFunction("WKSecurityOriginGetPort", api.securityOriginGetPort);
    ok &= loadFunction("WKSecurityOriginCreateFromString", api.securityOriginCreateFromString);
    ok &= loadFunction("WKGeolocationPermissionRequestAllow", api.geolocationPermissionRequestAllow);
    ok &= loadFunction("WKGeolocationPermissionRequestDeny", api.geolocationPermissionRequestDeny);
    ok &= loadFunction("WKNotificationPermissionRequestAllow", api.notificationPermissionRequestAllow);
    ok &= loadFunction("WKNotificationPermissionRequestDeny", api.notificationPermissionRequestDeny);
    ok &= loadFunction("WKUserMediaPermissionRequestAllow", api.userMediaPermissionRequestAllow);
    ok &= loadFunction("WKUserMediaPermissionRequestDeny", api.userMediaPermissionRequestDeny);
    ok &= loadFunction("WKUserMediaPermissionRequestRequiresCameraCapture", api.userMediaRequiresCameraCapture);
    ok &= loadFunction("WKUserMediaPermissionRequestRequiresMicrophoneCapture", api.userMediaRequiresMicrophoneCapture);
    ok &= loadFunction("WKUserMediaPermissionRequestRequiresDisplayCapture", api.userMediaRequiresDisplayCapture);
    ok &= loadFunction("WKUserMediaPermissionRequestVideoDeviceUIDs", api.userMediaVideoDeviceUIDs);
    ok &= loadFunction("WKUserMediaPermissionRequestAudioDeviceUIDs", api.userMediaAudioDeviceUIDs);
    ok &= loadFunction("WKArrayGetItemAtIndex", api.arrayGetItemAtIndex);
    ok &= loadFunction("WKArrayGetSize", api.arrayGetSize);
    ok &= loadFunction("WKArrayCreateAdoptingValues", api.arrayCreateAdoptingValues);
    ok &= loadFunction("WKContextGetGeolocationManager", api.contextGetGeolocationManager);
    ok &= loadFunction("WKGeolocationManagerSetProvider", api.geolocationManagerSetProvider);
    ok &= loadFunction("WKGeolocationManagerProviderDidChangePosition", api.geolocationManagerDidChangePosition);
    ok &= loadFunction("WKGeolocationManagerProviderDidFailToDeterminePositionWithErrorMessage", api.geolocationManagerDidFailWithMessage);
    ok &= loadFunction("WKGeolocationPositionCreate_b", api.geolocationPositionCreate);
    ok &= loadFunction("WKContextGetNotificationManager", api.contextGetNotificationManager);
    ok &= loadFunction("WKNotificationManagerSetProvider", api.notificationManagerSetProvider);
    ok &= loadFunction("WKNotificationManagerProviderDidShowNotification", api.notificationManagerDidShow);
    ok &= loadFunction("WKNotificationManagerProviderDidClickNotification", api.notificationManagerDidClick);
    ok &= loadFunction("WKNotificationManagerProviderDidCloseNotifications", api.notificationManagerDidClose);
    ok &= loadFunction("WKNotificationManagerProviderDidUpdateNotificationPolicy", api.notificationManagerDidUpdatePolicy);
    ok &= loadFunction("WKNotificationCopyTitle", api.notificationCopyTitle);
    ok &= loadFunction("WKNotificationCopyBody", api.notificationCopyBody);
    ok &= loadFunction("WKNotificationGetSecurityOrigin", api.notificationGetSecurityOrigin);
    ok &= loadFunction("WKNotificationGetID", api.notificationGetID);
    ok &= loadFunction("WKMutableDictionaryCreate", api.mutableDictionaryCreate);
    ok &= loadFunction("WKDictionarySetItem", api.dictionarySetItem);
    ok &= loadFunction("WKBooleanCreate", api.booleanCreate);
    ok &= loadFunction("WKUInt64Create", api.uint64Create);
    ok &= loadFunction("WKUInt64GetValue", api.uint64GetValue);

    // The Permissions API's own query hook is newer than the rest; a runtime
    // without it still prompts, it just cannot answer navigator.permissions.
    if (!loadFunction("WKQueryPermissionResultCallbackCompleteWithGranted", api.queryPermissionCompleteWithGranted))
        api.queryPermissionCompleteWithGranted = nullptr;
    if (!loadFunction("WKQueryPermissionResultCallbackCompleteWithDenied", api.queryPermissionCompleteWithDenied))
        api.queryPermissionCompleteWithDenied = nullptr;
    if (!loadFunction("WKQueryPermissionResultCallbackCompleteWithPrompt", api.queryPermissionCompleteWithPrompt))
        api.queryPermissionCompleteWithPrompt = nullptr;

    if (!ok)
        return false;

    g_api = api;
    g_apiReady.store(true, std::memory_order_release);
    setError(std::string());
    return true;
}

const WebKitApi& webKitApi()
{
    return g_api;
}

void setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error = message;
}

std::string takeErrorCopy()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

// --- Strings and origins -----------------------------------------------------

std::string stringText(WKStringRef value)
{
    const auto& api = webKitApi();
    if (!value || !api.stringGetMaximumUTF8CStringSize || !api.stringGetUTF8CString)
        return {};

    const size_t capacity = api.stringGetMaximumUTF8CStringSize(value);
    if (capacity == 0)
        return {};

    std::string text(capacity, '\0');
    const size_t written = api.stringGetUTF8CString(value, &text[0], capacity);
    if (written == 0)
        return {};

    // The count includes the terminator WebKit writes.
    text.resize(written - 1);
    return text;
}

std::string originText(WKSecurityOriginRef origin)
{
    const auto& api = webKitApi();
    if (!origin || !api.securityOriginCopyProtocol || !api.securityOriginCopyHost)
        return {};

    WKStringRef protocolString = api.securityOriginCopyProtocol(origin);
    WKStringRef hostString = api.securityOriginCopyHost(origin);
    const std::string protocolText = stringText(protocolString);
    const std::string hostText = stringText(hostString);
    if (protocolString)
        api.release(protocolString);
    if (hostString)
        api.release(hostString);

    if (protocolText.empty() && hostText.empty())
        return {};
    if (hostText.empty())
        return protocolText + "://";

    std::string text = protocolText + "://" + hostText;
    const unsigned short port = api.securityOriginGetPort ? api.securityOriginGetPort(origin) : 0;
    const bool defaultPort = (protocolText == "https" && port == 443) || (protocolText == "http" && port == 80);
    if (port != 0 && !defaultPort)
        text += ":" + std::to_string(static_cast<unsigned>(port));
    return text;
}

// --- The policy table --------------------------------------------------------

int policyDecision(int kind, const std::string& origin)
{
    std::lock_guard<std::mutex> lock(g_policyMutex);
    for (const auto& entry : g_policy) {
        if (entry.kind == kind && entry.origin == origin)
            return entry.decision;
    }
    return HB_PERMISSION_DECISION_ASK;
}

std::vector<PolicyEntry> policySnapshot(int kind)
{
    std::lock_guard<std::mutex> lock(g_policyMutex);
    std::vector<PolicyEntry> selected;
    for (const auto& entry : g_policy) {
        if (kind == 0 || entry.kind == kind)
            selected.push_back(entry);
    }
    return selected;
}

// --- Crossing to the WebKit thread -------------------------------------------

void postResolution(int requestId, int decision)
{
    markResolved(requestId);
    {
        std::lock_guard<std::mutex> lock(g_resolutionMutex);
        g_resolutions.emplace_back(requestId, decision);
    }
    hb_tabs_invoke_on_webkit_thread(drainResolutions, nullptr);
}

void postNotificationPolicyRefresh()
{
    hb_tabs_invoke_on_webkit_thread(refreshNotificationPolicy, nullptr);
}

void postGeolocationUpdate()
{
    hb_tabs_invoke_on_webkit_thread(deliverGeolocation, nullptr);
}

// --- The request queue -------------------------------------------------------

int enqueueRequest(int kind, RequestKind shape, std::string origin, WKTypeRef request, WKPageRef page)
{
    const auto& api = webKitApi();
    if (!request || !api.retain)
        return 0;
    if (g_shutdown.load())
        return 0;

    PendingRequest pending;
    pending.id = g_nextRequestId.fetch_add(1);
    pending.kind = kind;
    pending.shape = shape;
    pending.origin = std::move(origin);
    pending.request = request;
    pending.page = page;

    api.retain(request);

    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(std::move(pending));
    return g_queue.back().id;
}

void markResolved(int requestId)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    for (auto& pending : g_queue) {
        if (requestId == 0 || pending.id == requestId)
            pending.resolved = true;
    }
}

void answerRequest(int requestId, int decision)
{
    PendingRequest pending;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
            if (it->id != requestId)
                continue;
            pending = std::move(*it);
            g_queue.erase(it);
            found = true;
            break;
        }
    }
    if (!found)
        return;

    if (decision == HB_PERMISSION_DECISION_ALLOW)
        allowAndRelease(pending);
    else
        denyAndRelease(pending);
}

void denyEveryRequest()
{
    std::deque<PendingRequest> queued;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        queued.swap(g_queue);
    }
    for (const auto& pending : queued)
        denyAndRelease(pending);
}

void denyRequestsForPage(WKPageRef page)
{
    std::vector<PendingRequest> dropped;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        for (auto it = g_queue.begin(); it != g_queue.end();) {
            if (it->page == page) {
                dropped.push_back(std::move(*it));
                it = g_queue.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& pending : dropped)
        denyAndRelease(pending);
}

} // namespace harmony_permissions

// --- The C surface -----------------------------------------------------------

using namespace harmony_permissions;

void hb_permissions_attach(void)
{
    attachToTabRegistry();
}

// Reported rather than resolved: the entry points are resolved on the WebKit
// thread, where they are used, and asking from the host's thread would be a
// second writer of the table this only reads.
int hb_permissions_supported(void)
{
    return g_apiReady.load(std::memory_order_acquire) ? 1 : 0;
}

namespace {

// The oldest request nobody has answered. A request the host has already
// answered stays queued until the WebKit thread carries the answer out, and is
// stepped over here so it is never asked about twice.
const PendingRequest* frontUnresolved()
{
    for (const auto& pending : g_queue) {
        if (!pending.resolved)
            return &pending;
    }
    return nullptr;
}

} // namespace

int hb_permissions_pending_count(void)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    int count = 0;
    for (const auto& pending : g_queue) {
        if (!pending.resolved)
            ++count;
    }
    return count;
}

int hb_permissions_pending_id(void)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    const PendingRequest* pending = frontUnresolved();
    return pending ? pending->id : 0;
}

int hb_permissions_pending_kind(void)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    const PendingRequest* pending = frontUnresolved();
    return pending ? pending->kind : 0;
}

const char* hb_permissions_pending_origin(void)
{
    static thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        const PendingRequest* pending = frontUnresolved();
        copy = pending ? pending->origin : std::string();
    }
    return copy.c_str();
}

void hb_permissions_resolve(int request_id, int decision)
{
    if (request_id == 0)
        return;
    postResolution(request_id, decision);
}

void hb_permissions_deny_all(void)
{
    postResolution(0, HB_PERMISSION_DECISION_DENY);
}

void hb_permissions_policy_clear(void)
{
    {
        std::lock_guard<std::mutex> lock(g_policyMutex);
        g_policy.clear();
    }
    postNotificationPolicyRefresh();
}

void hb_permissions_policy_set(int kind, const char* origin, int decision)
{
    if (!origin || kind == 0)
        return;

    const std::string text(origin);
    {
        std::lock_guard<std::mutex> lock(g_policyMutex);
        auto it = std::find_if(g_policy.begin(), g_policy.end(), [&](const PolicyEntry& entry) {
            return entry.kind == kind && entry.origin == text;
        });
        if (decision == HB_PERMISSION_DECISION_ASK) {
            if (it != g_policy.end())
                g_policy.erase(it);
        } else if (it != g_policy.end()) {
            it->decision = decision;
        } else {
            PolicyEntry entry;
            entry.kind = kind;
            entry.origin = text;
            entry.decision = decision;
            g_policy.push_back(std::move(entry));
        }
    }

    if (kind == HB_PERMISSION_KIND_NOTIFICATIONS)
        postNotificationPolicyRefresh();
}

int hb_permissions_policy_get(int kind, const char* origin)
{
    if (!origin)
        return HB_PERMISSION_DECISION_ASK;
    return policyDecision(kind, std::string(origin));
}

const char* hb_permissions_error(void)
{
    static thread_local std::string copy;
    copy = takeErrorCopy();
    return copy.c_str();
}

// Everything torn down here belongs to the WebKit thread, so the work is posted
// to it rather than done here. Call this before the page host stops that
// thread: after it stops, nothing is left to answer with, and the process is
// ending anyway.
void hb_permissions_shutdown(void)
{
    g_shutdown.store(true);
    markResolved(0);
    hb_tabs_invoke_on_webkit_thread(shutdownOnWebKitThread, nullptr);
}
