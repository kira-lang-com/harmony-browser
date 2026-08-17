#ifndef HARMONY_NAVIGATION_WEBKIT_H
#define HARMONY_NAVIGATION_WEBKIT_H

#include <cstddef>
#include <cstdint>
#include <string>

// The slice of WebKit's C API the navigation model calls, declared here rather
// than included from the engine's headers.
//
// The engine is loaded at run time and its checkout is not required to build
// this package, exactly as the WebKit host beside this file does it. So every
// type is an opaque pointer.
//
// The navigation client itself is not declared here: a page carries one, the
// tabs registry owns it, and this model writes its own fields into the struct
// that registry hands out. What is left here are the callback signatures those
// fields hold, and the entry points the model calls to read a page.
namespace harmony_navigation {

using WKTypeRef = const void*;
using WKPageRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKErrorRef = const void*;
using WKNavigationRef = const void*;
using WKBackForwardListRef = const void*;
using WKBackForwardListItemRef = const void*;

// --- The navigation callbacks this model owns ----------------------------------
//
// The two decidePolicy callbacks and the three download callbacks are the tabs
// registry's: a policy listener nobody answers is a load that neither starts nor
// fails, and the registry is what guarantees every one of them is answered. What
// is left is a report rather than a decision, and belongs here.

using WKPageNavigationDidStartProvisionalNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
using WKPageNavigationDidReceiveServerRedirectForProvisionalNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
using WKPageNavigationDidFailProvisionalNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*);
using WKPageNavigationDidCommitNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
using WKPageNavigationDidFinishNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
using WKPageNavigationDidFailNavigationCallback = void (*)(WKPageRef, WKNavigationRef, WKErrorRef, WKTypeRef, const void*);
using WKPageNavigationWebProcessDidCrashCallback = void (*)(WKPageRef, const void*);
using WKPageNavigationDidFinishDocumentLoadCallback = void (*)(WKPageRef, WKNavigationRef, WKTypeRef, const void*);
using WKPageNavigationDidSameDocumentNavigationCallback = void (*)(WKPageRef, WKNavigationRef, uint32_t, WKTypeRef, const void*);

// --- The resolved engine ------------------------------------------------------

struct WebKitApi {
    WKStringRef (*pageCopyTitle)(WKPageRef) { nullptr };
    WKURLRef (*pageCopyActiveURL)(WKPageRef) { nullptr };
    WKURLRef (*pageCopyCommittedURL)(WKPageRef) { nullptr };
    WKURLRef (*pageCopyProvisionalURL)(WKPageRef) { nullptr };
    double (*pageGetEstimatedProgress)(WKPageRef) { nullptr };
    bool (*pageCanGoBack)(WKPageRef) { nullptr };
    bool (*pageCanGoForward)(WKPageRef) { nullptr };

    void (*pageLoadURL)(WKPageRef, WKURLRef) { nullptr };
    void (*pageReload)(WKPageRef) { nullptr };
    void (*pageReloadFromOrigin)(WKPageRef) { nullptr };
    void (*pageStopLoading)(WKPageRef) { nullptr };
    void (*pageGoBack)(WKPageRef) { nullptr };
    void (*pageGoForward)(WKPageRef) { nullptr };
    void (*pageGoToBackForwardListItem)(WKPageRef, WKBackForwardListItemRef) { nullptr };

    WKBackForwardListRef (*pageGetBackForwardList)(WKPageRef) { nullptr };
    unsigned (*backForwardListGetBackListCount)(WKBackForwardListRef) { nullptr };
    unsigned (*backForwardListGetForwardListCount)(WKBackForwardListRef) { nullptr };
    WKBackForwardListItemRef (*backForwardListGetItemAtIndex)(WKBackForwardListRef, int) { nullptr };
    WKURLRef (*backForwardListItemCopyURL)(WKBackForwardListItemRef) { nullptr };
    WKStringRef (*backForwardListItemCopyTitle)(WKBackForwardListItemRef) { nullptr };

    WKURLRef (*urlCreateWithUTF8CString)(const char*) { nullptr };
    WKStringRef (*urlCopyString)(WKURLRef) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CStringNonStrict)(WKStringRef, char*, size_t) { nullptr };

    int (*errorGetErrorCode)(WKErrorRef) { nullptr };
    WKStringRef (*errorCopyLocalizedDescription)(WKErrorRef) { nullptr };

    void (*release)(WKTypeRef) { nullptr };
};

// The engine, resolved from the WebKit module the host already loaded, or null
// when it has not loaded one. Call from the WebKit thread only.
const WebKitApi* webKitApi();

// Releases an object this layer holds a reference to, tolerating both a null
// object and an engine that never resolved.
void releaseWebKitObject(WKTypeRef object);

// The UTF-8 text of a `WKStringRef`, which the caller still owns.
std::string textOfString(WKStringRef value);

// The UTF-8 text of a `WKURLRef`, which the caller still owns.
std::string textOfURL(WKURLRef url);

} // namespace harmony_navigation

#endif
