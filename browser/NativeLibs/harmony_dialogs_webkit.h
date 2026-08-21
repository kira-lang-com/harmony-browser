#ifndef HARMONY_DIALOGS_WEBKIT_H
#define HARMONY_DIALOGS_WEBKIT_H

// The slice of WebKit's C API the dialogs need, declared rather than included.
//
// WebKit2.dll is loaded by the tabs registry and every entry point comes through
// its symbol seam, so the build depends on no WebKit checkout. The declarations
// below are transcribed from the headers of the checkout this ships against.
//
// WKPageUIClientV19 is the layout the tabs registry's shared client is written
// through. Its first fields are WKPageUIClientV6 field for field, which is what
// makes writing through this layout safe whatever version that registry
// declares: a field is written only when the client's own version says WebKit
// will read that far, because API::Client copies interfaceSizes[version] bytes
// out of what it is handed and reads nothing past them.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace harmony_dialogs {

using WKTypeRef = const void*;
using WKTypeID = uint32_t;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKURLRequestRef = const void*;
using WKArrayRef = const void*;
using WKDataRef = const void*;
using WKDictionaryRef = const void*;
using WKErrorRef = const void*;
using WKPageRef = const void*;
using WKPageConfigurationRef = const void*;
using WKFrameRef = const void*;
using WKFrameInfoRef = const void*;
using WKSecurityOriginRef = const void*;
using WKNavigationActionRef = const void*;
using WKWindowFeaturesRef = const void*;
using WKHitTestResultRef = const void*;
using WKOpenPanelParametersRef = const void*;
using WKOpenPanelResultListenerRef = const void*;
using WKGeolocationPermissionRequestRef = const void*;
using WKNotificationPermissionRequestRef = const void*;
using WKUserMediaPermissionRequestRef = const void*;
using WKAuthenticationChallengeRef = const void*;
using WKAuthenticationDecisionListenerRef = const void*;
using WKProtectionSpaceRef = const void*;
using WKCredentialRef = const void*;
using WKCompletionListenerRef = const void*;
using WKColorPickerResultListenerRef = const void*;
using WKMediaKeySystemPermissionCallbackRef = const void*;
using WKQueryPermissionResultCallbackRef = const void*;
using WKPageRunJavaScriptAlertResultListenerRef = const void*;
using WKPageRunJavaScriptConfirmResultListenerRef = const void*;
using WKPageRunJavaScriptPromptResultListenerRef = const void*;
using WKPageRunBeforeUnloadConfirmPanelResultListenerRef = const void*;
using WKPageRequestStorageAccessConfirmResultListenerRef = const void*;

using WKEventModifiers = uint32_t;
using WKEventMouseButton = int32_t;
using WKFocusDirection = uint32_t;
using WKPluginUnavailabilityReason = uint32_t;
using WKAutoplayEvent = uint32_t;
using WKAutoplayEventFlags = uint32_t;
using WKScreenOrientationType = uint32_t;
using WKCredentialPersistence = uint32_t;
using WKProtectionSpaceAuthenticationScheme = uint32_t;
using WKNativeEventPtr = const MSG*;

struct WKPoint {
    double x;
    double y;
};

struct WKSize {
    double width;
    double height;
};

struct WKRect {
    WKPoint origin;
    WKSize size;
};

enum : uint32_t {
    kWKCredentialPersistenceNone = 0,
    kWKCredentialPersistenceForSession = 1,
    kWKCredentialPersistencePermanent = 2,
};

enum : uint32_t {
    kWKProtectionSpaceAuthenticationSchemeDefault = 1,
    kWKProtectionSpaceAuthenticationSchemeHTTPBasic = 2,
    kWKProtectionSpaceAuthenticationSchemeHTTPDigest = 3,
    kWKProtectionSpaceAuthenticationSchemeHTMLForm = 4,
    kWKProtectionSpaceAuthenticationSchemeNTLM = 5,
    kWKProtectionSpaceAuthenticationSchemeNegotiate = 6,
    kWKProtectionSpaceAuthenticationSchemeClientCertificateRequested = 7,
    kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested = 8,
    kWKProtectionSpaceAuthenticationSchemeOAuth = 9,
    kWKProtectionSpaceAuthenticationSchemeUnknown = 100,
};

// What a WinCairo network process accepts as "the user decided to trust this
// server anyway": a credential whose user name is exactly this.
constexpr char kAcceptServerTrustUser[] = "accept server trust";

typedef void (*WKPageUIClientCallback)(WKPageRef, const void*);
typedef WKPageRef (*WKPageCreateNewPageCallback)(WKPageRef, WKPageConfigurationRef, WKNavigationActionRef, WKWindowFeaturesRef, const void*);
typedef void (*WKPageRunBeforeUnloadConfirmPanelCallback)(WKPageRef, WKStringRef, WKFrameRef, WKPageRunBeforeUnloadConfirmPanelResultListenerRef, const void*);
typedef void (*WKPageRunJavaScriptAlertCallback)(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptAlertResultListenerRef, const void*);
typedef void (*WKPageRunJavaScriptConfirmCallback)(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptConfirmResultListenerRef, const void*);
typedef void (*WKPageRunJavaScriptPromptCallback)(WKPageRef, WKStringRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, WKPageRunJavaScriptPromptResultListenerRef, const void*);
typedef void (*WKPageRequestStorageAccessConfirmCallback)(WKPageRef, WKFrameRef, WKStringRef, WKStringRef, WKPageRequestStorageAccessConfirmResultListenerRef, const void*);
typedef void (*WKPageTakeFocusCallback)(WKPageRef, WKFocusDirection, const void*);
typedef void (*WKPageFocusCallback)(WKPageRef, const void*);
typedef void (*WKPageUnfocusCallback)(WKPageRef, const void*);
typedef void (*WKPageSetStatusTextCallback)(WKPageRef, WKStringRef, const void*);
typedef void (*WKPageMouseDidMoveOverElementCallback)(WKPageRef, WKHitTestResultRef, WKEventModifiers, WKTypeRef, const void*);
typedef void (*WKPageTooltipDidChangeCallback)(WKPageRef, WKStringRef, const void*);
typedef void (*WKPageDidNotHandleKeyEventCallback)(WKPageRef, WKNativeEventPtr, const void*);
typedef void (*WKPageDidNotHandleWheelEventCallback)(WKPageRef, WKNativeEventPtr, const void*);
typedef bool (*WKPageGetToolbarsAreVisibleCallback)(WKPageRef, const void*);
typedef void (*WKPageSetToolbarsAreVisibleCallback)(WKPageRef, bool, const void*);
typedef bool (*WKPageGetMenuBarIsVisibleCallback)(WKPageRef, const void*);
typedef void (*WKPageSetMenuBarIsVisibleCallback)(WKPageRef, bool, const void*);
typedef bool (*WKPageGetStatusBarIsVisibleCallback)(WKPageRef, const void*);
typedef void (*WKPageSetStatusBarIsVisibleCallback)(WKPageRef, bool, const void*);
typedef bool (*WKPageGetIsResizableCallback)(WKPageRef, const void*);
typedef void (*WKPageSetIsResizableCallback)(WKPageRef, bool, const void*);
typedef WKRect (*WKPageGetWindowFrameCallback)(WKPageRef, const void*);
typedef void (*WKPageSetWindowFrameCallback)(WKPageRef, WKRect, const void*);
typedef unsigned long long (*WKPageExceededDatabaseQuotaCallback)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKStringRef, WKStringRef, unsigned long long, unsigned long long, unsigned long long, unsigned long long, const void*);
typedef void (*WKPageRunOpenPanelCallback)(WKPageRef, WKFrameRef, WKOpenPanelParametersRef, WKOpenPanelResultListenerRef, const void*);
typedef void (*WKPageDecidePolicyForGeolocationPermissionRequestCallback)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKGeolocationPermissionRequestRef, const void*);
typedef float (*WKPageHeaderHeightCallback)(WKPageRef, WKFrameRef, const void*);
typedef float (*WKPageFooterHeightCallback)(WKPageRef, WKFrameRef, const void*);
typedef void (*WKPageDrawHeaderCallback)(WKPageRef, WKFrameRef, WKRect, const void*);
typedef void (*WKPageDrawFooterCallback)(WKPageRef, WKFrameRef, WKRect, const void*);
typedef void (*WKPagePrintFrameCallback)(WKPageRef, WKFrameRef, const void*);
typedef void (*WKPageSaveDataToFileInDownloadsFolderCallback)(WKPageRef, WKStringRef, WKStringRef, WKURLRef, WKDataRef, const void*);
typedef void (*WKPageDecidePolicyForNotificationPermissionRequestCallback)(WKPageRef, WKSecurityOriginRef, WKNotificationPermissionRequestRef, const void*);
typedef void (*WKPageShowColorPickerCallback)(WKPageRef, WKStringRef, WKColorPickerResultListenerRef, const void*);
typedef void (*WKPageHideColorPickerCallback)(WKPageRef, const void*);
typedef void (*WKPageUnavailablePluginButtonClickedCallback)(WKPageRef, WKPluginUnavailabilityReason, WKDictionaryRef, const void*);
typedef void (*WKPagePinnedStateDidChangeCallback)(WKPageRef, const void*);
typedef void (*WKPageIsPlayingAudioDidChangeCallback)(WKPageRef, const void*);
typedef void (*WKPageDecidePolicyForUserMediaPermissionRequestCallback)(WKPageRef, WKFrameRef, WKSecurityOriginRef, WKSecurityOriginRef, WKUserMediaPermissionRequestRef, const void*);
typedef void (*WKPageDidClickAutoFillButtonCallback)(WKPageRef, WKTypeRef, const void*);
typedef void (*WKHandleAutoplayEventCallback)(WKPageRef, WKAutoplayEvent, WKAutoplayEventFlags, const void*);
typedef void (*WKFullscreenMayReturnToInlineCallback)(WKPageRef, const void*);
typedef void (*WKRequestPointerLockCallback)(WKPageRef, WKCompletionListenerRef, const void*);
typedef void (*WKDidLosePointerLockCallback)(WKPageRef, const void*);
typedef void (*WKHasVideoInPictureInPictureDidChangeCallback)(WKPageRef, bool, const void*);
typedef void (*WKPageDidResignInputElementStrongPasswordAppearanceCallback)(WKPageRef, WKTypeRef, const void*);
typedef bool (*WKPageShouldAllowDeviceOrientationAndMotionAccessCallback)(WKPageRef, WKSecurityOriginRef, WKFrameInfoRef, const void*);
typedef void (*WKPageRunWebAuthenticationPanelCallback)(void);
typedef void (*WKPageDecidePolicyForMediaKeySystemPermissionRequestCallback)(WKPageRef, WKSecurityOriginRef, WKStringRef, WKMediaKeySystemPermissionCallbackRef);
typedef void (*WKQueryPermissionCallback)(WKStringRef, WKSecurityOriginRef, WKQueryPermissionResultCallbackRef);
typedef void (*WKLockScreenOrientationCallback)(WKPageRef, WKScreenOrientationType);
typedef void (*WKUnlockScreenOrientationCallback)(WKPageRef);
typedef WKPageRef (*WKPageCreateNewPageCallbackV0)(WKPageRef, WKDictionaryRef, WKEventModifiers, WKEventMouseButton, const void*);
typedef void (*WKPageMouseDidMoveOverElementCallbackV0)(WKPageRef, WKEventModifiers, WKTypeRef, const void*);
typedef void (*WKPageMissingPluginButtonClickedCallbackV0)(WKPageRef, WKStringRef, WKStringRef, WKStringRef, const void*);
typedef void (*WKPageUnavailablePluginButtonClickedCallbackV1)(WKPageRef, WKPluginUnavailabilityReason, WKStringRef, WKStringRef, WKStringRef, const void*);
typedef void (*WKPageRunJavaScriptAlertCallbackV0)(WKPageRef, WKStringRef, WKFrameRef, const void*);
typedef bool (*WKPageRunJavaScriptConfirmCallbackV0)(WKPageRef, WKStringRef, WKFrameRef, const void*);
typedef WKStringRef (*WKPageRunJavaScriptPromptCallbackV0)(WKPageRef, WKStringRef, WKStringRef, WKFrameRef, const void*);
typedef WKPageRef (*WKPageCreateNewPageCallbackV1)(WKPageRef, WKURLRequestRef, WKDictionaryRef, WKEventModifiers, WKEventMouseButton, const void*);
typedef void (*WKPageRunJavaScriptAlertCallbackV5)(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, const void*);
typedef bool (*WKPageRunJavaScriptConfirmCallbackV5)(WKPageRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, const void*);
typedef WKStringRef (*WKPageRunJavaScriptPromptCallbackV5)(WKPageRef, WKStringRef, WKStringRef, WKFrameRef, WKSecurityOriginRef, const void*);
typedef bool (*WKPageRunBeforeUnloadConfirmPanelCallbackV6)(WKPageRef, WKStringRef, WKFrameRef, const void*);
typedef void (*WKPageAddMessageToConsoleCallback)(WKPageRef, WKStringRef, const void*);

struct WKPageUIClientBase {
    int version;
    const void* clientInfo;
};

struct WKPageUIClientV19 {
    WKPageUIClientBase base;

    // Version 0.
    WKPageCreateNewPageCallbackV0 createNewPage_deprecatedForUseWithV0;
    WKPageUIClientCallback showPage;
    WKPageUIClientCallback close;
    WKPageTakeFocusCallback takeFocus;
    WKPageFocusCallback focus;
    WKPageUnfocusCallback unfocus;
    WKPageRunJavaScriptAlertCallbackV0 runJavaScriptAlert_deprecatedForUseWithV0;
    WKPageRunJavaScriptConfirmCallbackV0 runJavaScriptConfirm_deprecatedForUseWithV0;
    WKPageRunJavaScriptPromptCallbackV0 runJavaScriptPrompt_deprecatedForUseWithV0;
    WKPageSetStatusTextCallback setStatusText;
    WKPageMouseDidMoveOverElementCallbackV0 mouseDidMoveOverElement_deprecatedForUseWithV0;
    WKPageMissingPluginButtonClickedCallbackV0 missingPluginButtonClicked_deprecatedForUseWithV0;
    WKPageDidNotHandleKeyEventCallback didNotHandleKeyEvent;
    WKPageDidNotHandleWheelEventCallback didNotHandleWheelEvent;
    WKPageGetToolbarsAreVisibleCallback toolbarsAreVisible;
    WKPageSetToolbarsAreVisibleCallback setToolbarsAreVisible;
    WKPageGetMenuBarIsVisibleCallback menuBarIsVisible;
    WKPageSetMenuBarIsVisibleCallback setMenuBarIsVisible;
    WKPageGetStatusBarIsVisibleCallback statusBarIsVisible;
    WKPageSetStatusBarIsVisibleCallback setStatusBarIsVisible;
    WKPageGetIsResizableCallback isResizable;
    WKPageSetIsResizableCallback setIsResizable;
    WKPageGetWindowFrameCallback getWindowFrame;
    WKPageSetWindowFrameCallback setWindowFrame;
    WKPageRunBeforeUnloadConfirmPanelCallbackV6 runBeforeUnloadConfirmPanel_deprecatedForUseWithV6;
    WKPageUIClientCallback didDraw;
    WKPageUIClientCallback pageDidScroll;
    WKPageExceededDatabaseQuotaCallback exceededDatabaseQuota;
    WKPageRunOpenPanelCallback runOpenPanel;
    WKPageDecidePolicyForGeolocationPermissionRequestCallback decidePolicyForGeolocationPermissionRequest;
    WKPageHeaderHeightCallback headerHeight;
    WKPageFooterHeightCallback footerHeight;
    WKPageDrawHeaderCallback drawHeader;
    WKPageDrawFooterCallback drawFooter;
    WKPagePrintFrameCallback printFrame;
    WKPageUIClientCallback runModal;
    void* unused1;
    WKPageSaveDataToFileInDownloadsFolderCallback saveDataToFileInDownloadsFolder;
    void* shouldInterruptJavaScript_unavailable;

    // Version 1.
    WKPageCreateNewPageCallbackV1 createNewPage_deprecatedForUseWithV1;
    WKPageMouseDidMoveOverElementCallback mouseDidMoveOverElement;
    WKPageDecidePolicyForNotificationPermissionRequestCallback decidePolicyForNotificationPermissionRequest;
    WKPageUnavailablePluginButtonClickedCallbackV1 unavailablePluginButtonClicked_deprecatedForUseWithV1;

    // Version 2.
    WKPageShowColorPickerCallback showColorPicker;
    WKPageHideColorPickerCallback hideColorPicker;
    WKPageUnavailablePluginButtonClickedCallback unavailablePluginButtonClicked;

    // Version 3.
    WKPagePinnedStateDidChangeCallback pinnedStateDidChange;

    // Version 4.
    void* unused2;
    void* unused3;
    void* unused4;
    WKPageIsPlayingAudioDidChangeCallback isPlayingAudioDidChange;

    // Version 5.
    WKPageDecidePolicyForUserMediaPermissionRequestCallback decidePolicyForUserMediaPermissionRequest;
    WKPageDidClickAutoFillButtonCallback didClickAutoFillButton;
    WKPageRunJavaScriptAlertCallbackV5 runJavaScriptAlert_deprecatedForUseWithV5;
    WKPageRunJavaScriptConfirmCallbackV5 runJavaScriptConfirm_deprecatedForUseWithV5;
    WKPageRunJavaScriptPromptCallbackV5 runJavaScriptPrompt_deprecatedForUseWithV5;
    void* unused5;

    // Version 6.
    WKPageCreateNewPageCallback createNewPage;
    WKPageRunJavaScriptAlertCallback runJavaScriptAlert;
    WKPageRunJavaScriptConfirmCallback runJavaScriptConfirm;
    WKPageRunJavaScriptPromptCallback runJavaScriptPrompt;
    void* unused6;

    // Version 7.
    WKPageRunBeforeUnloadConfirmPanelCallback runBeforeUnloadConfirmPanel;
    WKFullscreenMayReturnToInlineCallback fullscreenMayReturnToInline;

    // Version 8.
    WKRequestPointerLockCallback requestPointerLock;
    WKDidLosePointerLockCallback didLosePointerLock;

    // Version 9.
    WKHandleAutoplayEventCallback handleAutoplayEvent;

    // Version 10.
    WKHasVideoInPictureInPictureDidChangeCallback hasVideoInPictureInPictureDidChange;
    void* unused7;

    // Version 11.
    WKPageDidResignInputElementStrongPasswordAppearanceCallback didResignInputElementStrongPasswordAppearance;

    // Version 12.
    WKPageRequestStorageAccessConfirmCallback requestStorageAccessConfirm;

    // Version 13.
    WKPageShouldAllowDeviceOrientationAndMotionAccessCallback shouldAllowDeviceOrientationAndMotionAccess;

    // Version 14.
    WKPageRunWebAuthenticationPanelCallback runWebAuthenticationPanel;

    // Version 15.
    void* unused8;

    // Version 16.
    WKPageDecidePolicyForMediaKeySystemPermissionRequestCallback decidePolicyForMediaKeySystemPermissionRequest;

    // Version 17.
    WKQueryPermissionCallback queryPermission;

    // Version 18.
    WKLockScreenOrientationCallback lockScreenOrientation;
    WKUnlockScreenOrientationCallback unlockScreenOrientation;

    // Version 19.
    WKPageAddMessageToConsoleCallback addMessageToConsole;
    WKPageTooltipDidChangeCallback tooltipDidChange;
};

// Every entry point resolved out of the WebKit runtime the tabs registry loaded.
struct WebKitApi {
    void (*pageForceRepaint)(WKPageRef, void*, void (*)(WKTypeRef, void*)) { nullptr };

    void (*alertListenerCall)(WKPageRunJavaScriptAlertResultListenerRef) { nullptr };
    void (*confirmListenerCall)(WKPageRunJavaScriptConfirmResultListenerRef, bool) { nullptr };
    void (*promptListenerCall)(WKPageRunJavaScriptPromptResultListenerRef, WKStringRef) { nullptr };
    void (*beforeUnloadListenerCall)(WKPageRunBeforeUnloadConfirmPanelResultListenerRef, bool) { nullptr };

    bool (*openPanelAllowsMultipleFiles)(WKOpenPanelParametersRef) { nullptr };
    bool (*openPanelAllowsDirectories)(WKOpenPanelParametersRef) { nullptr };
    WKArrayRef (*openPanelCopyAcceptedMIMETypes)(WKOpenPanelParametersRef) { nullptr };
    WKArrayRef (*openPanelCopyAcceptedFileExtensions)(WKOpenPanelParametersRef) { nullptr };
    void (*openPanelChooseFiles)(WKOpenPanelResultListenerRef, WKArrayRef, WKArrayRef) { nullptr };
    void (*openPanelCancel)(WKOpenPanelResultListenerRef) { nullptr };

    WKAuthenticationDecisionListenerRef (*challengeGetDecisionListener)(WKAuthenticationChallengeRef) { nullptr };
    WKProtectionSpaceRef (*challengeGetProtectionSpace)(WKAuthenticationChallengeRef) { nullptr };
    int (*challengeGetPreviousFailureCount)(WKAuthenticationChallengeRef) { nullptr };
    void (*decisionUseCredential)(WKAuthenticationDecisionListenerRef, WKCredentialRef) { nullptr };

    WKStringRef (*protectionSpaceCopyHost)(WKProtectionSpaceRef) { nullptr };
    int (*protectionSpaceGetPort)(WKProtectionSpaceRef) { nullptr };
    WKStringRef (*protectionSpaceCopyRealm)(WKProtectionSpaceRef) { nullptr };
    bool (*protectionSpaceGetIsProxy)(WKProtectionSpaceRef) { nullptr };
    WKProtectionSpaceAuthenticationScheme (*protectionSpaceGetAuthenticationScheme)(WKProtectionSpaceRef) { nullptr };
    WKArrayRef (*protectionSpaceCopyCertificateChain)(WKProtectionSpaceRef) { nullptr };
    int (*protectionSpaceGetCertificateVerificationError)(WKProtectionSpaceRef) { nullptr };
    WKStringRef (*protectionSpaceCopyCertificateVerificationErrorDescription)(WKProtectionSpaceRef) { nullptr };

    WKCredentialRef (*credentialCreate)(WKStringRef, WKStringRef, WKCredentialPersistence) { nullptr };

    WKStringRef (*securityOriginCopyProtocol)(WKSecurityOriginRef) { nullptr };
    WKStringRef (*securityOriginCopyHost)(WKSecurityOriginRef) { nullptr };
    unsigned short (*securityOriginGetPort)(WKSecurityOriginRef) { nullptr };

    bool (*frameIsMainFrame)(WKFrameRef) { nullptr };

    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CString)(WKStringRef, char*, size_t) { nullptr };
    WKURLRef (*urlCreateWithUTF8CString)(const char*) { nullptr };
    WKArrayRef (*arrayCreateAdoptingValues)(WKTypeRef*, size_t) { nullptr };
    WKTypeRef (*arrayGetItemAtIndex)(WKArrayRef, size_t) { nullptr };
    size_t (*arrayGetSize)(WKArrayRef) { nullptr };
    const unsigned char* (*dataGetBytes)(WKDataRef) { nullptr };
    size_t (*dataGetSize)(WKDataRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };
    WKTypeRef (*retain)(WKTypeRef) { nullptr };
};

// --- Entry points, implemented in harmony_dialogs_webkit.cpp -----------------

// Resolves the table above through the tabs registry's symbol seam, once. False
// when the runtime has not loaded yet or is missing an entry point, which is the
// one state in which nothing else here may be called.
bool resolveWebKitApi();

// The resolved table. Valid only after `resolveWebKitApi` has answered true.
const WebKitApi& wk();

// Records a diagnostic `hb_dialogs_error` reports.
void setDialogsError(const std::string& text);

// A WK string as UTF-8, and a UTF-8 string as a +1 WK string.
std::string stringFromWK(WKStringRef value);
WKStringRef makeString(const std::string& text);

// Releases a +1 reference, tolerating null.
void releaseWK(WKTypeRef value);

// The strings in a WK array of WK strings.
std::vector<std::string> stringsFromArray(WKArrayRef array);

// A security origin as "scheme://host" with the port when it is not the
// scheme's own. Empty for a null or opaque origin.
std::string originText(WKSecurityOriginRef origin);

// HB_DIALOG_FLAG_SUBFRAME when the frame is not the page's main one.
int frameFlags(WKFrameRef frame);

// --- The parked requests, implemented in harmony_dialogs.cpp ----------------

enum class Kind {
    Alert = 1,
    Confirm = 2,
    Prompt = 3,
    BeforeUnload = 4,
    Authenticate = 5,
    Certificate = 6,
    // Answered by a Windows common dialog rather than by the host's own UI, so
    // it never becomes the request the host is asked to show.
    FilePicker = 7,
};

// One question a page is suspended on.
//
// The listener is retained for exactly as long as the request lives: WebKit
// lends it for the duration of the callback, and the answer arrives long after
// that callback has returned.
//
// `blocking` names a question that has no listener to answer later: a client
// older than the version that introduced one takes the answer as the callback's
// return value, so that callback stays on the WebKit thread until a person has
// answered it. Everything else parks and returns.
struct Request {
    int id { 0 };
    Kind kind { Kind::Alert };
    WKPageRef page { nullptr };
    WKTypeRef listener { nullptr };
    WKAuthenticationChallengeRef challenge { nullptr };
    bool blocking { false };

    std::string message;
    std::string origin;
    std::string defaultValue;
    std::string detail;
    int flags { 0 };
    int code { 0 };

    // The certificate this request is about, kept so accepting it is remembered
    // against the exact certificate rather than against the host name.
    std::string certificateHost;
    std::string certificatePem;

    bool answered { false };
    bool accepted { false };
    bool remember { false };
    std::string answerText;
    std::string answerSecret;
    std::vector<std::string> answerPaths;
};

// Parks a request, taking the reference its listener and challenge need to
// outlive the callback that delivered them. Returns the new request's id.
int enqueueRequest(Request request);

// Runs the WebKit thread's half of a question that has no listener: stays here,
// pumping that thread's messages so WebKit keeps answering everything else,
// until a person answers this request or the page it belongs to goes away.
// Returns whether they accepted, and writes what they typed into `answerText`.
// WebKit thread only.
bool waitForBlockingAnswer(int requestId, std::string& answerText);

// Asks the WebKit thread to hand WebKit every answer the frame thread has
// posted. Safe from either thread.
void requestWebKitService();

// Remembers a page, so a question can be shown against it and a page that goes
// away can be made to answer everything it still owes. WebKit thread only.
void attachPage(WKPageRef page);
void detachPage(WKPageRef page);

// Whether this exact certificate has already been accepted for this host.
bool certificateIsTrusted(const std::string& host, const std::string& pem);

// The window the host draws in, as `hb_dialogs_frame` last recorded it. Null
// before the first frame; the file picker opens unowned in that case.
HWND hostWindow();

// --- The page clients, implemented in harmony_dialogs_page.cpp ---------------

// Joins the tabs registry: the UI client hook that writes the JavaScript
// dialogs, the before-unload confirmation and the file picker into the client
// every page shares, and the page observer that keeps `attachPage` and
// `detachPage` in step with the pages themselves.
void attachToTabRegistry();

// --- The file picker, implemented in harmony_dialogs_filepicker.cpp ----------

struct FilePickerRequest {
    int id { 0 };
    HWND owner { nullptr };
    bool allowsMultiple { false };
    bool allowsDirectories { false };
    std::vector<std::string> mimeTypes;
    std::vector<std::string> extensions;
};

// Queues a picker, starting the picker thread on first use. The result comes
// back through `filePickerCompleted`.
void filePickerSubmit(FilePickerRequest request);

// Stops the picker thread, cancelling anything still queued.
void filePickerShutdown();

// Called by the picker thread with the chosen paths, empty when cancelled.
// Defined in harmony_dialogs.cpp.
void filePickerCompleted(int requestId, std::vector<std::string> paths);

} // namespace harmony_dialogs

#endif
