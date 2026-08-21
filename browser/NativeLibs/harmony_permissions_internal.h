#ifndef HARMONY_PERMISSIONS_INTERNAL_H
#define HARMONY_PERMISSIONS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

// The bridge's own view of WebKit.
//
// WebKit2.dll is loaded by the page host, never by this file, and every entry
// point below is resolved out of the already-loaded module. Nothing here
// includes a WebKit header: the header tree needs the build's own include
// layout, and a browser that only links against a checkout it built itself is a
// browser that cannot ship. The types are opaque pointers and the structs are
// transcribed layouts, which is exactly what the C API guarantees.

namespace harmony_permissions {

using WKTypeRef = const void*;
using WKArrayRef = const void*;
using WKBooleanRef = const void*;
using WKContextRef = const void*;
using WKDictionaryRef = const void*;
using WKFrameRef = const void*;
using WKGeolocationManagerRef = const void*;
using WKGeolocationPermissionRequestRef = const void*;
using WKGeolocationPositionRef = const void*;
using WKMutableDictionaryRef = const void*;
using WKNotificationManagerRef = const void*;
using WKNotificationPermissionRequestRef = const void*;
using WKNotificationRef = const void*;
using WKPageRef = const void*;
using WKQueryPermissionResultCallbackRef = const void*;
using WKSecurityOriginRef = const void*;
using WKStringRef = const void*;
using WKUserMediaPermissionRequestRef = const void*;

// The denial reason WebKit spells for a person saying no, from
// WKUserMediaPermissionRequest.h's enum.
constexpr uint32_t kUserMediaPermissionDenied = 5;

struct WebKitApi {
    WKTypeRef (*retain)(WKTypeRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };

    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CString)(WKStringRef, char*, size_t) { nullptr };

    WKStringRef (*securityOriginCopyProtocol)(WKSecurityOriginRef) { nullptr };
    WKStringRef (*securityOriginCopyHost)(WKSecurityOriginRef) { nullptr };
    unsigned short (*securityOriginGetPort)(WKSecurityOriginRef) { nullptr };
    WKSecurityOriginRef (*securityOriginCreateFromString)(WKStringRef) { nullptr };

    void (*geolocationPermissionRequestAllow)(WKGeolocationPermissionRequestRef) { nullptr };
    void (*geolocationPermissionRequestDeny)(WKGeolocationPermissionRequestRef) { nullptr };
    void (*notificationPermissionRequestAllow)(WKNotificationPermissionRequestRef) { nullptr };
    void (*notificationPermissionRequestDeny)(WKNotificationPermissionRequestRef) { nullptr };

    void (*userMediaPermissionRequestAllow)(WKUserMediaPermissionRequestRef, WKStringRef, WKStringRef) { nullptr };
    void (*userMediaPermissionRequestDeny)(WKUserMediaPermissionRequestRef, uint32_t) { nullptr };
    bool (*userMediaRequiresCameraCapture)(WKUserMediaPermissionRequestRef) { nullptr };
    bool (*userMediaRequiresMicrophoneCapture)(WKUserMediaPermissionRequestRef) { nullptr };
    bool (*userMediaRequiresDisplayCapture)(WKUserMediaPermissionRequestRef) { nullptr };
    WKArrayRef (*userMediaVideoDeviceUIDs)(WKUserMediaPermissionRequestRef) { nullptr };
    WKArrayRef (*userMediaAudioDeviceUIDs)(WKUserMediaPermissionRequestRef) { nullptr };

    WKTypeRef (*arrayGetItemAtIndex)(WKArrayRef, size_t) { nullptr };
    size_t (*arrayGetSize)(WKArrayRef) { nullptr };
    WKArrayRef (*arrayCreateAdoptingValues)(WKTypeRef*, size_t) { nullptr };

    void (*queryPermissionCompleteWithGranted)(WKQueryPermissionResultCallbackRef) { nullptr };
    void (*queryPermissionCompleteWithDenied)(WKQueryPermissionResultCallbackRef) { nullptr };
    void (*queryPermissionCompleteWithPrompt)(WKQueryPermissionResultCallbackRef) { nullptr };

    WKGeolocationManagerRef (*contextGetGeolocationManager)(WKContextRef) { nullptr };
    void (*geolocationManagerSetProvider)(WKGeolocationManagerRef, const void*) { nullptr };
    void (*geolocationManagerDidChangePosition)(WKGeolocationManagerRef, WKGeolocationPositionRef) { nullptr };
    void (*geolocationManagerDidFailWithMessage)(WKGeolocationManagerRef, WKStringRef) { nullptr };
    WKGeolocationPositionRef (*geolocationPositionCreate)(double, double, double, double, bool, double, bool, double, bool, double, bool, double) { nullptr };

    WKNotificationManagerRef (*contextGetNotificationManager)(WKContextRef) { nullptr };
    void (*notificationManagerSetProvider)(WKNotificationManagerRef, const void*) { nullptr };
    void (*notificationManagerDidShow)(WKNotificationManagerRef, uint64_t) { nullptr };
    void (*notificationManagerDidClick)(WKNotificationManagerRef, uint64_t) { nullptr };
    void (*notificationManagerDidClose)(WKNotificationManagerRef, WKArrayRef) { nullptr };
    void (*notificationManagerDidUpdatePolicy)(WKNotificationManagerRef, WKSecurityOriginRef, bool) { nullptr };

    WKStringRef (*notificationCopyTitle)(WKNotificationRef) { nullptr };
    WKStringRef (*notificationCopyBody)(WKNotificationRef) { nullptr };
    WKSecurityOriginRef (*notificationGetSecurityOrigin)(WKNotificationRef) { nullptr };
    uint64_t (*notificationGetID)(WKNotificationRef) { nullptr };

    WKMutableDictionaryRef (*mutableDictionaryCreate)() { nullptr };
    bool (*dictionarySetItem)(WKMutableDictionaryRef, WKStringRef, WKTypeRef) { nullptr };
    WKBooleanRef (*booleanCreate)(bool) { nullptr };
    WKTypeRef (*uint64Create)(uint64_t) { nullptr };
    uint64_t (*uint64GetValue)(WKTypeRef) { nullptr };
};

// What a queued request is, which decides how it is answered and released.
enum class RequestKind {
    Geolocation,
    Notification,
    UserMedia,
};

// A location the position source has produced, in the shape WebKit takes it.
struct GeolocationFix {
    double timestamp { 0 };
    double latitude { 0 };
    double longitude { 0 };
    double accuracy { 0 };
    bool hasAltitude { false };
    double altitude { 0 };
    bool hasAltitudeAccuracy { false };
    double altitudeAccuracy { 0 };
    bool hasHeading { false };
    double heading { 0 };
    bool hasSpeed { false };
    double speed { 0 };
};

// --- Shared services --------------------------------------------------------

// Resolves the WebKit entry points through the tabs registry's symbol seam.
// False when the engine has not loaded yet or an entry point is missing.
bool ensureWebKitApi();
const WebKitApi& webKitApi();

void setError(const std::string& message);
std::string takeErrorCopy();

// Posts an answer across to the WebKit thread, where it is applied.
void postResolution(int requestId, int decision);

// One remembered decision.
struct PolicyEntry {
    int kind { 0 };
    std::string origin;
    int decision { 0 };
};

// The remembered decisions. Safe from either thread.
int policyDecision(int kind, const std::string& origin);
std::vector<PolicyEntry> policySnapshot(int kind);

// Asks the WebKit thread to push the notification policy at the notification
// manager, so a page that already asked sees a grant or a revocation.
void postNotificationPolicyRefresh();

// Asks the WebKit thread to hand WebKit the position the source just produced.
void postGeolocationUpdate();

// Queues a request the policy table could not answer, retaining it. Returns the
// id the host will answer with, or 0 when the queue could not take it.
int enqueueRequest(int kind, RequestKind shape, std::string origin, WKTypeRef request, WKPageRef page);

// Marks queued requests as spoken for, from the host's thread, the moment it
// answers them. The WebKit thread is what actually answers WebKit, and it may
// be a frame or two behind; without this the host would be asked again about a
// question it has already answered. A request id of 0 marks the whole queue.
void markResolved(int requestId);

// Answers one queued request and releases it. WebKit thread only.
void answerRequest(int requestId, int decision);

// Answers every queued request with a denial. WebKit thread only.
void denyEveryRequest();

// Drops the queued requests belonging to one page, denying each. WebKit thread.
void denyRequestsForPage(WKPageRef page);

// The origin of a security origin, as "scheme://host[:port]", empty when there
// is nothing to name. WebKit thread only.
std::string originText(WKSecurityOriginRef origin);

// A WKString as UTF-8. WebKit thread only.
std::string stringText(WKStringRef value);

// --- Clients (harmony_permissions_clients.cpp) ------------------------------

// Registers the UI client hook and the page observer with the tabs registry.
void attachToTabRegistry();

// Runs on the WebKit thread: pushes the remembered notification answers at the
// notification manager, so a page that already asked sees a grant or a
// revocation without asking again.
void pushNotificationPolicy();

// The notification manager the provider was installed on, so a policy change
// can be pushed to it. Null until a context has been attached.
WKNotificationManagerRef notificationManager();

// The geolocation manager the provider was installed on. Null until then.
WKGeolocationManagerRef geolocationManager();

// Runs on the WebKit thread: hands WebKit whatever the position source last
// produced, or the failure it last reported.
void deliverGeolocationState();

// Called by the position source from whichever thread produced the fix.
void publishGeolocationFix(const GeolocationFix& fix);
void publishGeolocationFailure(const std::string& message);

// --- Position source (harmony_permissions_geolocation.cpp) ------------------

void geolocationSourceStart(bool highAccuracy);
void geolocationSourceStop();
void geolocationSourceSetHighAccuracy(bool highAccuracy);
void geolocationSourceShutdown();

// --- Notification surface (harmony_permissions_notify.cpp) ------------------

void notificationSurfaceShow(uint64_t notificationId, const std::string& title, const std::string& body, const std::string& origin);
void notificationSurfaceCancel(uint64_t notificationId);
void notificationSurfaceShutdown();

// Called by the notification surface on the WebKit thread.
void notificationWasClicked(uint64_t notificationId);
void notificationWasClosed(uint64_t notificationId);

} // namespace harmony_permissions

#endif
