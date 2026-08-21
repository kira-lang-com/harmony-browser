#ifndef HARMONY_PERMISSIONS_UI_CLIENT_H
#define HARMONY_PERMISSIONS_UI_CLIENT_H

#include "harmony_permissions_internal.h"

// The WebKit client layouts this bridge writes through.
//
// They are transcribed rather than included: the WebKit headers need that
// build's own include layout, and a browser that only links against a checkout
// it built itself is a browser that cannot ship. WebKit fills its own copy by
// memcpy'ing `interfaceSizes[version]` bytes out of what it is handed, so a
// layout and the version beside it must agree exactly; every member after the
// base is pointer-sized, and the ones this bridge does not implement are named
// and left null so the order can be read against the header.

namespace harmony_permissions {

// Every WebKit client struct starts with this pair, and WebKit reads the
// version out of it to decide how many bytes of the rest it may copy.
struct WKClientBase {
    int version;
    const void* clientInfo;
};

using GeolocationRequestCallback = void (*)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKGeolocationPermissionRequestRef, const void*);
using NotificationRequestCallback = void (*)(WKPageRef, WKSecurityOriginRef, WKNotificationPermissionRequestRef, const void*);
using UserMediaRequestCallback = void (*)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKSecurityOriginRef, WKUserMediaPermissionRequestRef, const void*);
using QueryPermissionCallback = void (*)(WKStringRef, WKSecurityOriginRef, WKQueryPermissionResultCallbackRef);

// The version of WKPageUIClient the Permissions API's own query hook arrived
// in. The three requests are older than any client this browser installs.
constexpr int kVersionQueryPermission = 17;

// WKPageUIClientV19, field for field. It is the layout the tabs registry hands
// over, typed, so writing through it puts a field where that registry's own
// struct has it; a field is written only when the client's version says WebKit
// will read that far.
struct WKPageUIClientV19 {
    WKClientBase base;

    // Version 0.
    void* createNewPage_deprecatedForUseWithV0;
    void* showPage;
    void* close;
    void* takeFocus;
    void* focus;
    void* unfocus;
    void* runJavaScriptAlert_deprecatedForUseWithV0;
    void* runJavaScriptConfirm_deprecatedForUseWithV0;
    void* runJavaScriptPrompt_deprecatedForUseWithV0;
    void* setStatusText;
    void* mouseDidMoveOverElement_deprecatedForUseWithV0;
    void* missingPluginButtonClicked_deprecatedForUseWithV0;
    void* didNotHandleKeyEvent;
    void* didNotHandleWheelEvent;
    void* toolbarsAreVisible;
    void* setToolbarsAreVisible;
    void* menuBarIsVisible;
    void* setMenuBarIsVisible;
    void* statusBarIsVisible;
    void* setStatusBarIsVisible;
    void* isResizable;
    void* setIsResizable;
    void* getWindowFrame;
    void* setWindowFrame;
    void* runBeforeUnloadConfirmPanel_deprecatedForUseWithV6;
    void* didDraw;
    void* pageDidScroll;
    void* exceededDatabaseQuota;
    void* runOpenPanel;
    GeolocationRequestCallback decidePolicyForGeolocationPermissionRequest;
    void* headerHeight;
    void* footerHeight;
    void* drawHeader;
    void* drawFooter;
    void* printFrame;
    void* runModal;
    void* unused1;
    void* saveDataToFileInDownloadsFolder;
    void* shouldInterruptJavaScript_unavailable;

    // Version 1.
    void* createNewPage_deprecatedForUseWithV1;
    void* mouseDidMoveOverElement;
    NotificationRequestCallback decidePolicyForNotificationPermissionRequest;
    void* unavailablePluginButtonClicked_deprecatedForUseWithV1;

    // Version 2.
    void* showColorPicker;
    void* hideColorPicker;
    void* unavailablePluginButtonClicked;

    // Version 3.
    void* pinnedStateDidChange;

    // Version 4.
    void* unused2;
    void* unused3;
    void* unused4;
    void* isPlayingAudioDidChange;

    // Version 5.
    UserMediaRequestCallback decidePolicyForUserMediaPermissionRequest;
    void* didClickAutoFillButton;
    void* runJavaScriptAlert_deprecatedForUseWithV5;
    void* runJavaScriptConfirm_deprecatedForUseWithV5;
    void* runJavaScriptPrompt_deprecatedForUseWithV5;
    void* unused5;

    // Version 6.
    void* createNewPage;
    void* runJavaScriptAlert;
    void* runJavaScriptConfirm;
    void* runJavaScriptPrompt;
    void* unused6;

    // Version 7.
    void* runBeforeUnloadConfirmPanel;
    void* fullscreenMayReturnToInline;

    // Version 8.
    void* requestPointerLock;
    void* didLosePointerLock;

    // Version 9.
    void* handleAutoplayEvent;

    // Version 10.
    void* hasVideoInPictureInPictureDidChange;
    void* unused7;

    // Version 11.
    void* didResignInputElementStrongPasswordAppearance;

    // Version 12.
    void* requestStorageAccessConfirm;

    // Version 13.
    void* shouldAllowDeviceOrientationAndMotionAccess;

    // Version 14.
    void* runWebAuthenticationPanel;

    // Version 15.
    void* unused8;

    // Version 16.
    void* decidePolicyForMediaKeySystemPermissionRequest;

    // Version 17.
    QueryPermissionCallback queryPermission;

    // Version 18.
    void* lockScreenOrientation;
    void* unlockScreenOrientation;

    // Version 19.
    void* addMessageToConsole;
    void* tooltipDidChange;
};

struct WKGeolocationProviderV1 {
    WKClientBase base;
    void (*startUpdating)(WKGeolocationManagerRef, const void*);
    void (*stopUpdating)(WKGeolocationManagerRef, const void*);
    void (*setEnableHighAccuracy)(WKGeolocationManagerRef, bool, const void*);
};

struct WKNotificationProviderV0 {
    WKClientBase base;
    void (*show)(WKPageRef, WKNotificationRef, const void*);
    void (*cancel)(WKNotificationRef, const void*);
    void (*didDestroyNotification)(WKNotificationRef, const void*);
    void (*addNotificationManager)(WKNotificationManagerRef, const void*);
    void (*removeNotificationManager)(WKNotificationManagerRef, const void*);
    WKDictionaryRef (*notificationPermissions)(const void*);
    void (*clearNotifications)(WKArrayRef, const void*);
};

} // namespace harmony_permissions

#endif
