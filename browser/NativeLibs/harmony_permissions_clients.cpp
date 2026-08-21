#include "harmony_permissions_ui_client.h"

#include "harmony_permissions.h"
#include "harmony_tabs_embed.h"

#include <atomic>
#include <mutex>

// The WebKit clients: what a page asks, and what the two managers are told.
//
// The page's UI client is the tabs registry's, not this bridge's: that registry
// owns the engine and the pages, hands every module the client struct before it
// installs it, and says which version that struct is. The three questions a
// page asks are written into it through that seam, so nothing here installs a
// second client that would replace somebody else's.

namespace harmony_permissions {
namespace {

std::mutex g_geolocationMutex;
GeolocationFix g_lastFix;
bool g_hasFix { false };
std::string g_geolocationFailure;
bool g_hasGeolocationFailure { false };

WKGeolocationManagerRef g_geolocationManager { nullptr };
WKNotificationManagerRef g_notificationManager { nullptr };
bool g_geolocationHighAccuracy { false };

// What a media request is asking for, as one kind. A request for both devices
// is one question to a person, and two remembered answers.
int userMediaKind(WKUserMediaPermissionRequestRef request)
{
    const auto& api = webKitApi();
    const bool display = api.userMediaRequiresDisplayCapture && api.userMediaRequiresDisplayCapture(request);
    if (display)
        return HB_PERMISSION_KIND_SCREEN_CAPTURE;

    const bool camera = api.userMediaRequiresCameraCapture && api.userMediaRequiresCameraCapture(request);
    const bool microphone = api.userMediaRequiresMicrophoneCapture && api.userMediaRequiresMicrophoneCapture(request);
    if (camera && microphone)
        return HB_PERMISSION_KIND_CAMERA_AND_MICROPHONE;
    if (camera)
        return HB_PERMISSION_KIND_CAMERA;
    if (microphone)
        return HB_PERMISSION_KIND_MICROPHONE;
    return 0;
}

// The stored answer for a kind. The combined kind is not stored: it is the two
// device grants read together, so allowing the camera alone still asks before
// the microphone is opened with it.
int decisionForKind(int kind, const std::string& origin)
{
    if (kind != HB_PERMISSION_KIND_CAMERA_AND_MICROPHONE)
        return policyDecision(kind, origin);

    const int camera = policyDecision(HB_PERMISSION_KIND_CAMERA, origin);
    const int microphone = policyDecision(HB_PERMISSION_KIND_MICROPHONE, origin);
    if (camera == HB_PERMISSION_DECISION_DENY || microphone == HB_PERMISSION_DECISION_DENY)
        return HB_PERMISSION_DECISION_DENY;
    if (camera == HB_PERMISSION_DECISION_ALLOW && microphone == HB_PERMISSION_DECISION_ALLOW)
        return HB_PERMISSION_DECISION_ALLOW;
    return HB_PERMISSION_DECISION_ASK;
}

void refuse(RequestKind shape, WKTypeRef request)
{
    const auto& api = webKitApi();
    switch (shape) {
    case RequestKind::Geolocation:
        api.geolocationPermissionRequestDeny(request);
        return;
    case RequestKind::Notification:
        api.notificationPermissionRequestDeny(request);
        return;
    case RequestKind::UserMedia:
        api.userMediaPermissionRequestDeny(request, kUserMediaPermissionDenied);
        return;
    }
}

// Routes one request: answered from the table when the table knows, queued for
// a person when it does not. An origin with no name — a sandboxed frame, a data
// URL — is refused outright, because there is nothing a person could remember.
//
// A remembered allow still travels through the queue, so that every grant is
// carried out by one piece of code: a media grant has to name the devices it
// opens, and naming them twice is how the two answers drift apart.
void route(int kind, RequestKind shape, WKPageRef page, WKSecurityOriginRef origin, WKTypeRef request)
{
    if (!request)
        return;

    const std::string text = originText(origin);
    if (text.empty() || text.find("://") == std::string::npos) {
        refuse(shape, request);
        return;
    }

    const int decision = decisionForKind(kind, text);
    if (decision == HB_PERMISSION_DECISION_DENY) {
        refuse(shape, request);
        return;
    }

    const int id = enqueueRequest(kind, shape, text, request, page);
    if (id == 0) {
        refuse(shape, request);
        return;
    }

    if (decision == HB_PERMISSION_DECISION_ALLOW)
        answerRequest(id, HB_PERMISSION_DECISION_ALLOW);
}

void decideGeolocation(WKPageRef page, WKFrameRef, WKSecurityOriginRef origin, WKGeolocationPermissionRequestRef request, const void*)
{
    route(HB_PERMISSION_KIND_GEOLOCATION, RequestKind::Geolocation, page, origin, request);
}

void decideNotification(WKPageRef page, WKSecurityOriginRef origin, WKNotificationPermissionRequestRef request, const void*)
{
    route(HB_PERMISSION_KIND_NOTIFICATIONS, RequestKind::Notification, page, origin, request);
}

void decideUserMedia(WKPageRef page, WKFrameRef, WKSecurityOriginRef documentOrigin, WKSecurityOriginRef topLevelOrigin, WKUserMediaPermissionRequestRef request, const void*)
{
    const int kind = userMediaKind(request);
    if (kind == 0) {
        webKitApi().userMediaPermissionRequestDeny(request, kUserMediaPermissionDenied);
        return;
    }

    // A capture grant belongs to the page a person can see, so a frame's own
    // origin only decides when there is no top level one to name.
    WKSecurityOriginRef origin = topLevelOrigin ? topLevelOrigin : documentOrigin;
    route(kind, RequestKind::UserMedia, page, origin, request);
}

int kindForPermissionName(const std::string& name)
{
    if (name == "geolocation")
        return HB_PERMISSION_KIND_GEOLOCATION;
    if (name == "notifications")
        return HB_PERMISSION_KIND_NOTIFICATIONS;
    if (name == "camera")
        return HB_PERMISSION_KIND_CAMERA;
    if (name == "microphone")
        return HB_PERMISSION_KIND_MICROPHONE;
    if (name == "display-capture")
        return HB_PERMISSION_KIND_SCREEN_CAPTURE;
    return 0;
}

// navigator.permissions.query(). Answered from the same table the requests are,
// so a page reads back exactly what the browser will do.
void queryPermission(WKStringRef permissionName, WKSecurityOriginRef topOrigin, WKQueryPermissionResultCallbackRef callback)
{
    const auto& api = webKitApi();
    if (!callback || !api.queryPermissionCompleteWithPrompt)
        return;

    const int kind = kindForPermissionName(stringText(permissionName));
    const std::string origin = originText(topOrigin);
    if (kind == 0 || origin.empty()) {
        api.queryPermissionCompleteWithPrompt(callback);
        return;
    }

    switch (decisionForKind(kind, origin)) {
    case HB_PERMISSION_DECISION_ALLOW:
        api.queryPermissionCompleteWithGranted(callback);
        return;
    case HB_PERMISSION_DECISION_DENY:
        api.queryPermissionCompleteWithDenied(callback);
        return;
    default:
        api.queryPermissionCompleteWithPrompt(callback);
        return;
    }
}

void writeClientFields(int, hb_wk_page_ui_client_v19* shared, void*)
{
    if (!shared || !ensureWebKitApi())
        return;

    const int version = shared->base.version;
    auto* client = reinterpret_cast<WKPageUIClientV19*>(shared);
    client->decidePolicyForGeolocationPermissionRequest = decideGeolocation;
    client->decidePolicyForNotificationPermissionRequest = decideNotification;
    client->decidePolicyForUserMediaPermissionRequest = decideUserMedia;
    if (version >= kVersionQueryPermission && webKitApi().queryPermissionCompleteWithPrompt)
        client->queryPermission = queryPermission;
}

// --- Providers ---------------------------------------------------------------

void geolocationStartUpdating(WKGeolocationManagerRef manager, const void*)
{
    g_geolocationManager = manager;
    geolocationSourceStart(g_geolocationHighAccuracy);
}

void geolocationStopUpdating(WKGeolocationManagerRef, const void*)
{
    geolocationSourceStop();
}

void geolocationSetHighAccuracy(WKGeolocationManagerRef, bool enabled, const void*)
{
    g_geolocationHighAccuracy = enabled;
    geolocationSourceSetHighAccuracy(enabled);
}

void notificationShow(WKPageRef, WKNotificationRef notification, const void*)
{
    const auto& api = webKitApi();
    if (!notification)
        return;

    WKStringRef titleString = api.notificationCopyTitle(notification);
    WKStringRef bodyString = api.notificationCopyBody(notification);
    const std::string title = stringText(titleString);
    const std::string body = stringText(bodyString);
    if (titleString)
        api.release(titleString);
    if (bodyString)
        api.release(bodyString);

    const std::string origin = originText(api.notificationGetSecurityOrigin(notification));
    const uint64_t id = api.notificationGetID(notification);
    notificationSurfaceShow(id, title, body, origin);

    if (g_notificationManager)
        api.notificationManagerDidShow(g_notificationManager, id);
}

void notificationCancel(WKNotificationRef notification, const void*)
{
    const auto& api = webKitApi();
    if (!notification)
        return;
    const uint64_t id = api.notificationGetID(notification);
    notificationSurfaceCancel(id);
    notificationWasClosed(id);
}

void notificationDidDestroy(WKNotificationRef notification, const void*)
{
    if (!notification)
        return;
    notificationSurfaceCancel(webKitApi().notificationGetID(notification));
}

void notificationAddManager(WKNotificationManagerRef manager, const void*)
{
    g_notificationManager = manager;
    postNotificationPolicyRefresh();
}

void notificationRemoveManager(WKNotificationManagerRef manager, const void*)
{
    if (g_notificationManager == manager)
        g_notificationManager = nullptr;
}

// The origins that have already answered, which is what makes
// Notification.permission read "granted" without asking again.
WKDictionaryRef notificationPermissions(const void*)
{
    const auto& api = webKitApi();
    if (!api.mutableDictionaryCreate)
        return nullptr;

    WKMutableDictionaryRef permissions = api.mutableDictionaryCreate();
    if (!permissions)
        return nullptr;

    for (const auto& entry : policySnapshot(HB_PERMISSION_KIND_NOTIFICATIONS)) {
        WKStringRef key = api.stringCreateWithUTF8CString(entry.origin.c_str());
        WKBooleanRef value = api.booleanCreate(entry.decision == HB_PERMISSION_DECISION_ALLOW);
        if (key && value)
            api.dictionarySetItem(permissions, key, value);
        if (key)
            api.release(key);
        if (value)
            api.release(value);
    }
    return permissions;
}

void notificationClear(WKArrayRef notificationIDs, const void*)
{
    const auto& api = webKitApi();
    if (!notificationIDs || !api.arrayGetSize || !api.arrayGetItemAtIndex || !api.uint64GetValue)
        return;

    const size_t count = api.arrayGetSize(notificationIDs);
    for (size_t index = 0; index < count; ++index) {
        WKTypeRef item = api.arrayGetItemAtIndex(notificationIDs, index);
        if (item)
            notificationSurfaceCancel(api.uint64GetValue(item));
    }
}

WKGeolocationProviderV1 g_geolocationProvider {};
WKNotificationProviderV0 g_notificationProvider {};

// The two providers belong to the context, not to a page, so they are installed
// once, on the WebKit thread, the first time a page turns up there.
void installProviders(WKContextRef context)
{
    const auto& api = webKitApi();
    if (!context)
        return;

    if (api.contextGetGeolocationManager && api.geolocationManagerSetProvider) {
        WKGeolocationManagerRef manager = api.contextGetGeolocationManager(context);
        if (manager) {
            g_geolocationManager = manager;
            g_geolocationProvider = WKGeolocationProviderV1 {};
            g_geolocationProvider.base.version = 1;
            g_geolocationProvider.startUpdating = geolocationStartUpdating;
            g_geolocationProvider.stopUpdating = geolocationStopUpdating;
            g_geolocationProvider.setEnableHighAccuracy = geolocationSetHighAccuracy;
            api.geolocationManagerSetProvider(manager, &g_geolocationProvider);
        }
    }

    if (api.contextGetNotificationManager && api.notificationManagerSetProvider) {
        WKNotificationManagerRef manager = api.contextGetNotificationManager(context);
        if (manager) {
            g_notificationManager = manager;
            g_notificationProvider = WKNotificationProviderV0 {};
            g_notificationProvider.base.version = 0;
            g_notificationProvider.show = notificationShow;
            g_notificationProvider.cancel = notificationCancel;
            g_notificationProvider.didDestroyNotification = notificationDidDestroy;
            g_notificationProvider.addNotificationManager = notificationAddManager;
            g_notificationProvider.removeNotificationManager = notificationRemoveManager;
            g_notificationProvider.notificationPermissions = notificationPermissions;
            g_notificationProvider.clearNotifications = notificationClear;
            api.notificationManagerSetProvider(manager, &g_notificationProvider);
        }
    }
}

std::atomic<bool> g_providersInstalled { false };
std::atomic<bool> g_attached { false };

void pageCreated(int, void*, void*)
{
    if (!ensureWebKitApi())
        return;
    if (g_providersInstalled.load())
        return;

    // The context is the WebKit thread's, and this hook is the first moment
    // this bridge is on that thread with the engine already up.
    WKContextRef context = hb_tabs_context();
    if (!context)
        return;

    g_providersInstalled.store(true);
    installProviders(context);
    pushNotificationPolicy();
}

void pageDestroying(int, void* page, void*)
{
    if (page)
        denyRequestsForPage(page);
}

} // namespace

void attachToTabRegistry()
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_ui_client_hook(writeClientFields, nullptr);
    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
}

// Hands the notification manager every origin that has already answered, which
// is what makes Notification.permission read "granted" without asking again.
void pushNotificationPolicy()
{
    const auto& api = webKitApi();
    if (!g_notificationManager || !api.notificationManagerDidUpdatePolicy || !api.securityOriginCreateFromString)
        return;

    for (const auto& entry : policySnapshot(HB_PERMISSION_KIND_NOTIFICATIONS)) {
        WKStringRef text = api.stringCreateWithUTF8CString(entry.origin.c_str());
        if (!text)
            continue;
        WKSecurityOriginRef origin = api.securityOriginCreateFromString(text);
        api.release(text);
        if (!origin)
            continue;
        api.notificationManagerDidUpdatePolicy(g_notificationManager, origin, entry.decision == HB_PERMISSION_DECISION_ALLOW);
        api.release(origin);
    }
}

WKNotificationManagerRef notificationManager()
{
    return g_notificationManager;
}

WKGeolocationManagerRef geolocationManager()
{
    return g_geolocationManager;
}

void publishGeolocationFix(const GeolocationFix& fix)
{
    {
        std::lock_guard<std::mutex> lock(g_geolocationMutex);
        g_lastFix = fix;
        g_hasFix = true;
        g_hasGeolocationFailure = false;
        g_geolocationFailure.clear();
    }
    postGeolocationUpdate();
}

void publishGeolocationFailure(const std::string& message)
{
    {
        std::lock_guard<std::mutex> lock(g_geolocationMutex);
        g_geolocationFailure = message;
        g_hasGeolocationFailure = true;
        g_hasFix = false;
    }
    postGeolocationUpdate();
}

void deliverGeolocationState()
{
    const auto& api = webKitApi();
    if (!g_geolocationManager)
        return;

    GeolocationFix fix;
    std::string failure;
    bool hasFix = false;
    bool hasFailure = false;
    {
        std::lock_guard<std::mutex> lock(g_geolocationMutex);
        hasFix = g_hasFix;
        hasFailure = g_hasGeolocationFailure;
        fix = g_lastFix;
        failure = g_geolocationFailure;
        g_hasGeolocationFailure = false;
    }

    if (hasFailure) {
        if (!api.geolocationManagerDidFailWithMessage)
            return;
        WKStringRef message = api.stringCreateWithUTF8CString(failure.c_str());
        if (!message)
            return;
        api.geolocationManagerDidFailWithMessage(g_geolocationManager, message);
        api.release(message);
        return;
    }

    if (!hasFix || !api.geolocationPositionCreate || !api.geolocationManagerDidChangePosition)
        return;

    WKGeolocationPositionRef position = api.geolocationPositionCreate(
        fix.timestamp,
        fix.latitude,
        fix.longitude,
        fix.accuracy,
        fix.hasAltitude,
        fix.altitude,
        fix.hasAltitudeAccuracy,
        fix.altitudeAccuracy,
        fix.hasHeading,
        fix.heading,
        fix.hasSpeed,
        fix.speed
    );
    if (!position)
        return;

    api.geolocationManagerDidChangePosition(g_geolocationManager, position);
    api.release(position);
}

void notificationWasClicked(uint64_t notificationId)
{
    const auto& api = webKitApi();
    if (g_notificationManager && api.notificationManagerDidClick)
        api.notificationManagerDidClick(g_notificationManager, notificationId);
    notificationWasClosed(notificationId);
}

void notificationWasClosed(uint64_t notificationId)
{
    const auto& api = webKitApi();
    if (!g_notificationManager || !api.notificationManagerDidClose || !api.uint64Create || !api.arrayCreateAdoptingValues)
        return;

    WKTypeRef values[1] = { api.uint64Create(notificationId) };
    if (!values[0])
        return;

    WKArrayRef closed = api.arrayCreateAdoptingValues(values, 1);
    if (!closed) {
        api.release(values[0]);
        return;
    }
    api.notificationManagerDidClose(g_notificationManager, closed);
    api.release(closed);
}

} // namespace harmony_permissions
