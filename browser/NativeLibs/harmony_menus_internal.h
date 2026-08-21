#ifndef HARMONY_MENUS_INTERNAL_H
#define HARMONY_MENUS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "harmony_menus.h"
#include "harmony_text.h"

// The module's own state, shared between its translation units.
//
// Everything below runs on the WebKit thread unless it says otherwise: the menu
// is built inside a callback WebKit makes, and the selection is answered from
// inside the nested message loop the popup menu runs.

namespace harmony_menus {

using harmony::text::narrow;
using harmony::text::widen;

// Every WK object is an opaque pointer in C, so one alias serves them all. The
// entry points are mirrored by hand and resolved out of the engine the tabs
// registry loaded, so building this browser needs no WebKit checkout beside it.
using WKTypeRef = const void*;
using WKPageRef = const void*;
using WKFrameRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKErrorRef = const void*;
using WKArrayRef = const void*;
using WKMutableArrayRef = const void*;
using WKHitTestResultRef = const void*;
using WKContextMenuItemRef = const void*;
using WKInspectorRef = const void*;
using WKPageConfigurationRef = const void*;
using WKPreferencesRef = const void*;

using WKPageGetSourceForFrameFunction = void (*)(WKStringRef, WKErrorRef, void*);

// --- The tags this browser's own items carry ----------------------------------
//
// WebKit routes an item whose tag is at or above its application base straight
// back to the client that made it, without a round trip through the web process.
// Every item built here therefore carries one of these, and every one of WebKit's
// own is left with the tag WebKit gave it.
constexpr uint32_t kApplicationTagBase = 10000;

enum Tag : uint32_t {
    TagOpenLinkInNewTab = kApplicationTagBase + 1,
    TagOpenLinkInBackgroundTab,
    TagCopyLinkAddress,
    TagSaveLinkAs,

    TagOpenImageInNewTab,
    TagSaveImageAs,
    TagCopyImageAddress,

    TagOpenMediaInNewTab,
    TagSaveMediaAs,
    TagCopyMediaAddress,

    TagBack,
    TagForward,
    TagReload,
    TagStop,

    // Cut, Copy and Paste stay WebKit's own proposals: only the web process
    // knows whether there is a selection to cut and whether the field will take
    // a paste. Select All is not proposed on this port at all.
    TagSelectAll,

    TagCopyPageAddress,
    TagViewPageSource,
    TagInspectElement,
};

// --- WebKit's own tags this module answers for --------------------------------
//
// Each of these is a proposal whose job one of the items above already does. A
// menu carrying both would offer the same action twice under two names, so the
// proposal is dropped and this browser's own item stands in its place.
bool isSupersededProposal(uint32_t tag);

// --- The engine ---------------------------------------------------------------

struct WebKitApi {
    void (*pageSetPageContextMenuClient)(WKPageRef, const void*) { nullptr };

    WKURLRef (*hitTestResultCopyAbsoluteLinkURL)(WKHitTestResultRef) { nullptr };
    WKURLRef (*hitTestResultCopyAbsoluteImageURL)(WKHitTestResultRef) { nullptr };
    WKURLRef (*hitTestResultCopyAbsoluteMediaURL)(WKHitTestResultRef) { nullptr };
    bool (*hitTestResultIsContentEditable)(WKHitTestResultRef) { nullptr };

    WKContextMenuItemRef (*contextMenuItemCreateAsAction)(uint32_t, WKStringRef, bool) { nullptr };
    WKContextMenuItemRef (*contextMenuItemSeparatorItem)() { nullptr };
    uint32_t (*contextMenuItemGetTag)(WKContextMenuItemRef) { nullptr };

    WKMutableArrayRef (*mutableArrayCreate)() { nullptr };
    void (*arrayAppendItem)(WKMutableArrayRef, WKTypeRef) { nullptr };
    size_t (*arrayGetSize)(WKArrayRef) { nullptr };
    WKTypeRef (*arrayGetItemAtIndex)(WKArrayRef, size_t) { nullptr };

    void (*pageExecuteCommand)(WKPageRef, WKStringRef) { nullptr };
    WKFrameRef (*pageGetMainFrame)(WKPageRef) { nullptr };
    void (*pageGetSourceForFrame)(WKPageRef, WKFrameRef, void*, WKPageGetSourceForFrameFunction) { nullptr };
    bool (*pageCanGoBack)(WKPageRef) { nullptr };
    bool (*pageCanGoForward)(WKPageRef) { nullptr };

    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CStringNonStrict)(WKStringRef, char*, size_t) { nullptr };
    WKStringRef (*urlCopyString)(WKURLRef) { nullptr };

    WKTypeRef (*retain)(WKTypeRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };

    // Optional. A build without an inspector still browses; it just does not
    // offer to open one, and the item is left off the menu rather than shown
    // doing nothing.
    WKInspectorRef (*pageGetInspector)(WKPageRef) { nullptr };
    void (*inspectorShow)(WKInspectorRef) { nullptr };
    WKPageConfigurationRef (*pageCopyPageConfiguration)(WKPageRef) { nullptr };
    WKPreferencesRef (*pageConfigurationGetPreferences)(WKPageConfigurationRef) { nullptr };
    void (*preferencesSetDeveloperExtrasEnabled)(WKPreferencesRef, bool) { nullptr };
};

const WebKitApi* webKitApi();

void releaseObject(WKTypeRef object);
WKStringRef createString(const std::string& text);
std::string textOfString(WKStringRef value);
std::string textOfURL(WKURLRef url);

// Whether this build can open an inspector on a page.
bool inspectorIsAvailable();

// --- What the menu was raised on ----------------------------------------------
//
// The hit test is handed to the client that BUILDS the menu and not to the one
// that answers it, so what a selected item needs to know is remembered here
// while the menu is up. There is one menu on screen at a time, so there is one
// of these.
struct MenuContext {
    int tabId { 0 };
    WKPageRef page { nullptr };

    std::string linkURL;
    std::string imageURL;
    std::string mediaURL;
    std::string pageURL;
    bool editable { false };
    bool canGoBack { false };
    bool canGoForward { false };
};

extern MenuContext g_context;

// --- Building -----------------------------------------------------------------

// The menu this browser hands back: its own items first, then whatever WebKit
// proposed that none of them replaces. The returned array carries one reference,
// which WebKit adopts.
WKMutableArrayRef buildMenu(WKArrayRef proposedMenu);

// --- Answering ----------------------------------------------------------------

// One item of this browser's own, selected. Everything the engine can do is done
// here; everything a system above the engine owns is queued.
void runTag(uint32_t tag);

// Puts UTF-8 text on the Windows clipboard.
void copyToClipboard(const std::string& text);

// --- Page source --------------------------------------------------------------

// Asks the page for the source of its main frame, writes it beside the other
// temporary files this machine keeps, and queues the file to be opened in a new
// tab. WebKit answers asynchronously, so this returns before the tab is asked
// for.
void showPageSource(WKPageRef page);

// Removes the source dumps this run wrote.
void discardPageSources();

// --- The queue ----------------------------------------------------------------

void queueCommand(int kind, std::string text, int tabId);

void setError(const std::string& message);
std::string currentError();

} // namespace harmony_menus

#endif
