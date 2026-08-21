#ifndef HARMONY_TABS_EMBED_H
#define HARMONY_TABS_EMBED_H

#include <stddef.h>

// The seam another native module of this browser reaches WebKit through.
//
// WebKit2.dll is loaded once, into one process, and everything it owns lives on
// one thread. A second module that loaded it again, or drove a second run loop,
// would have a second engine in the same process. So the tabs registry owns the
// runtime and hands the rest of the browser what it needs to attach to it: the
// symbol table, the thread, the context, and the page behind a tab.
//
// Nothing here is safe to call from the host's frame thread unless it says so.
// A WKPageRef, a WKViewRef and the WKContextRef belong to the WebKit thread;
// reach them from inside hb_tabs_invoke_on_webkit_thread or from a hook below.

#ifdef __cplusplus
extern "C" {
#endif

// Resolves a symbol out of the loaded WebKit2.dll, or NULL when the runtime has
// not loaded or does not export it. Safe from any thread once the engine is
// ready; the module handle never changes for the life of the process.
void* hb_tabs_webkit_symbol(const char* name);

// The WKContextRef every tab's page is created in.
void* hb_tabs_context(void);

// The WKPageRef / WKViewRef behind a tab, or NULL when the id is unknown or the
// view has not been created yet.
void* hb_tabs_page(int tab_id);
void* hb_tabs_view(int tab_id);

// The tab a page belongs to, or 0 when the page is not one of ours.
int hb_tabs_id_for_page(void* page);

// Non-zero when the caller is already on the WebKit thread.
int hb_tabs_on_webkit_thread(void);

// Runs `fn(context)` on the WebKit thread. Returns immediately; the call is
// queued behind whatever the thread is already doing. Running it directly when
// the caller is already on that thread would reorder it ahead of that queue, so
// it is queued either way.
void hb_tabs_invoke_on_webkit_thread(void (*fn)(void*), void* context);

// Called on the WebKit thread with a tab's page, once just after it is created
// and once just before it is destroyed. This is where a module installs a
// navigation client, a find client or a download client of its own: the page is
// alive, and no load has been started on it yet.
typedef void (*hb_tabs_page_hook)(int tab_id, void* page, void* user_data);
void hb_tabs_add_page_observer(hb_tabs_page_hook on_created, hb_tabs_page_hook on_destroying, void* user_data);

// --- The shared WKPageUIClient ----------------------------------------------
//
// A page carries one UI client, so the browser has one, and the modules that
// own parts of it fill their fields in. The tabs registry owns createNewPage
// (a page that opens a window gets a tab) and close (a page that closes itself
// closes its tab), and answers the geolocation request rather than leaving it
// unanswered -- WebKit denies an unhandled camera or notification request but
// drops an unhandled geolocation one, and a page whose geolocation promise
// never settles is worse than one that was told no.
//
// The struct mirrors WKPageUIClientV19 field for field, which is the latest
// version the engine this browser ships against declares. WebKit copies a
// client struct whose version is the latest one WHOLE, and zeroes the client
// outright for a version it does not know, so the layout and the version have to
// name the same struct: a client left at an older version reaches none of the
// fields after it -- navigator.permissions.query() among them -- and a client
// claiming a newer version than the loaded WebKit2.dll knows installs nothing at
// all. Every member is pointer-sized, so the mirror is exact as long as the
// field count is; the implementation asserts its size. base.version is 19 and
// base.clientInfo is the tab id widened to a pointer.

typedef struct hb_wk_page_ui_client_base {
    int version;
    const void* clientInfo;
} hb_wk_page_ui_client_base;

typedef struct hb_wk_page_ui_client_v19 {
    hb_wk_page_ui_client_base base;

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
    void* decidePolicyForGeolocationPermissionRequest;
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
    void* decidePolicyForNotificationPermissionRequest;
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
    void* decidePolicyForUserMediaPermissionRequest;
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
    void* queryPermission;

    // Version 18.
    void* lockScreenOrientation;
    void* unlockScreenOrientation;

    // Version 19.
    void* addMessageToConsole;
    void* tooltipDidChange;
} hb_wk_page_ui_client_v19;

// Called on the WebKit thread with the client a new tab's page is about to be
// given, after the tabs registry has written its own fields. Write only the
// fields your module owns: overwriting createNewPage or close takes tab
// creation and closing away from the registry that draws them.
typedef void (*hb_tabs_ui_client_hook)(int tab_id, hb_wk_page_ui_client_v19* client, void* user_data);
void hb_tabs_add_ui_client_hook(hb_tabs_ui_client_hook hook, void* user_data);

// --- The page's load state ----------------------------------------------------
//
// WKPageStateClient is the only report WebKit makes that a page's title, URL,
// progress or back/forward availability moved without a navigation callback to
// hang it on. A page carries one, and both the tab strip and the address bar
// live on it, so the registry installs it and publishes what it hears rather
// than handing the struct out: a client struct passed round is a client struct
// whose second writer silently replaces its first, which is the fault this seam
// exists to make impossible.
//
// The hook is called on the WebKit thread, with the page still alive. `field`
// says WHICH of the page's properties WebKit reported, so an observer that
// republishes a single number for progress and a whole record for everything
// else can tell them apart. Every registered observer is called for every
// change; none of them can take the report away from another.

#define HB_TABS_PAGE_STATE_LOADING 0
#define HB_TABS_PAGE_STATE_TITLE 1
#define HB_TABS_PAGE_STATE_URL 2
#define HB_TABS_PAGE_STATE_PROGRESS 3
#define HB_TABS_PAGE_STATE_CAN_GO_BACK 4
#define HB_TABS_PAGE_STATE_CAN_GO_FORWARD 5

typedef void (*hb_tabs_page_state_hook)(int tab_id, void* page, int field, void* user_data);
void hb_tabs_add_page_state_observer(hb_tabs_page_state_hook hook, void* user_data);

// --- The shared WKPageNavigationClient ---------------------------------------
//
// A page carries one navigation client for the same reason it carries one UI
// client, and the registry owns it for the same reason: the two policy
// callbacks MUST be answered. A policy listener nobody answers is a load that
// never starts and never fails, so leaving those two fields to whichever module
// installed its client last is not a thing this browser can afford.
//
// So the registry answers them, and asks the download clients below whether the
// navigation is a file rather than a page. Everything else -- did-start,
// did-commit, did-fail, the authentication challenge -- is a report rather than
// a decision, and belongs to whichever module tracks it.
//
// The struct mirrors WKPageNavigationClientV3 field for field. Every member of
// that struct is pointer-sized, so the mirror is exact as long as the field
// count is; the implementation asserts its size. base.version is 3 and
// base.clientInfo is the tab id widened to a pointer.

typedef struct hb_wk_page_navigation_client_base {
    int version;
    const void* clientInfo;
} hb_wk_page_navigation_client_base;

typedef struct hb_wk_page_navigation_client_v3 {
    hb_wk_page_navigation_client_base base;

    // Version 0.
    void* decidePolicyForNavigationAction;
    void* decidePolicyForNavigationResponse;
    void* decidePolicyForPluginLoad;
    void* didStartProvisionalNavigation;
    void* didReceiveServerRedirectForProvisionalNavigation;
    void* didFailProvisionalNavigation;
    void* didCommitNavigation;
    void* didFinishNavigation;
    void* didFailNavigation;
    void* didFailProvisionalLoadInSubframe;
    void* didFinishDocumentLoad;
    void* didSameDocumentNavigation;
    void* renderingProgressDidChange;
    void* canAuthenticateAgainstProtectionSpace;
    void* didReceiveAuthenticationChallenge;
    void* webProcessDidCrash;
    void* copyWebCryptoMasterKey;
    void* didBeginNavigationGesture;
    void* willEndNavigationGesture;
    void* didEndNavigationGesture;
    void* didRemoveNavigationGestureSnapshot;

    // Version 1.
    void* webProcessDidTerminate;

    // Version 2.
    void* contentRuleListNotification;

    // Version 3.
    void* copySignedPublicKeyAndChallengeString;
    void* navigationActionDidBecomeDownload;
    void* navigationResponseDidBecomeDownload;
    void* contextMenuDidCreateDownload;
} hb_wk_page_navigation_client_v3;

// Called on the WebKit thread with the client a new tab's page is about to be
// given, after the registry has written its own fields. Write only the fields
// your module owns; the two decidePolicy fields are the registry's, and a
// module that overwrites one takes on answering every listener it is handed.
typedef void (*hb_tabs_navigation_client_hook)(int tab_id, hb_wk_page_navigation_client_v3* client, void* user_data);
void hb_tabs_add_navigation_client_hook(hb_tabs_navigation_client_hook hook, void* user_data);

// --- Downloads ---------------------------------------------------------------
//
// Asked, on the WebKit thread, whether a navigation is a file to be saved
// rather than a page to be shown. `subject` is a WKNavigationActionRef for the
// action hook and a WKNavigationResponseRef for the response hook. Answer
// non-zero to turn it into a download.
typedef int (*hb_tabs_download_policy_hook)(int tab_id, const void* subject, void* user_data);

// Called on the WebKit thread with the WKDownloadRef WebKit created, borrowed:
// retain it to keep it. This is where a download module takes the download
// over and installs its own download client on it.
typedef void (*hb_tabs_download_hook)(int tab_id, const void* download, void* user_data);

// Registered at any time: the registry consults the current list from inside
// the callback rather than baking it into each page's client, so a module that
// attaches after a tab exists still sees that tab's downloads.
void hb_tabs_add_download_client(
    hb_tabs_download_policy_hook should_download_action,
    hb_tabs_download_policy_hook should_download_response,
    hb_tabs_download_hook did_become_download,
    void* user_data
);

// --- The WebKit thread's own schedule ----------------------------------------

// Run on the WebKit thread once per run-loop cycle, before WebKit's own cycle.
// This is where a module drains work it queued from the host's frame thread and
// that has to happen against a WebKit object.
typedef void (*hb_tabs_cycle_hook)(void* user_data);
void hb_tabs_add_cycle_hook(hb_tabs_cycle_hook hook, void* user_data);

// Run on the WebKit thread as the engine stops, after every tab has been closed
// and before the context is released. The last moment a module may release a
// WebKit object it holds.
typedef void (*hb_tabs_teardown_hook)(void* user_data);
void hb_tabs_add_teardown_hook(hb_tabs_teardown_hook hook, void* user_data);

#ifdef __cplusplus
}
#endif

#endif
