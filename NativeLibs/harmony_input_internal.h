#ifndef HARMONY_INPUT_INTERNAL_H
#define HARMONY_INPUT_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "harmony_input.h"
#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

// The module's own state, shared between its translation units.
//
// Two threads run through this file. The host's FRAME thread owns the window
// the chrome draws in and drains the event queue; the ENGINE thread owns every
// WKPage and every page's child window. Each declaration below says which one
// touches it, and anything both touch is either atomic or stated with the mutex
// that guards it.

namespace harmony_input {

// Every WK object is an opaque pointer, so one alias serves them all. The
// entry points are mirrored by hand and resolved out of the engine the tab
// registry loaded, so building this browser needs no WebKit checkout beside it.
using WKTypeRef = const void*;
using WKPageRef = const void*;
using WKFrameRef = const void*;
using WKStringRef = const void*;
using WKDataRef = const void*;
using WKViewRef = const void*;

// `WKPrintInfo`, by value, exactly as the C API passes it.
struct PrintInfo {
    float pageSetupScaleFactor { 1.0f };
    float availablePaperWidth { 0.0f };
    float availablePaperHeight { 0.0f };
};

// `WKPageFindClientV0`, field for field.
struct FindClientV0 {
    int version { 0 };
    const void* clientInfo { nullptr };
    void (*didFindString)(WKPageRef, WKStringRef, unsigned, const void*) { nullptr };
    void (*didFailToFindString)(WKPageRef, WKStringRef, const void*) { nullptr };
    void (*didCountStringMatches)(WKPageRef, WKStringRef, unsigned, const void*) { nullptr };
};

// `WKFindOptions`: case-insensitive, wrapping, with every match highlighted and
// the selected one indicated, which is what a find bar means by "find".
constexpr unsigned kFindOptionsForward = (1u << 0) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7);
constexpr unsigned kFindOptionsBackward = kFindOptionsForward | (1u << 3);

// How many matches are worth counting. Past this a page says "many" rather
// than making the web process walk a document to a number nobody reads.
constexpr unsigned kFindMaxMatchCount = 1000;

struct WebKitApi {
    HWND (*viewGetWindow)(WKViewRef) { nullptr };

    double (*pageGetPageZoomFactor)(WKPageRef) { nullptr };
    void (*pageSetPageZoomFactor)(WKPageRef, double) { nullptr };

    WKFrameRef (*pageGetMainFrame)(WKPageRef) { nullptr };
    void (*pageFindString)(WKPageRef, WKStringRef, unsigned, unsigned) { nullptr };
    void (*pageHideFindUI)(WKPageRef) { nullptr };
    void (*pageCountStringMatches)(WKPageRef, WKStringRef, unsigned, unsigned) { nullptr };
    void (*pageSetPageFindClient)(WKPageRef, const void*) { nullptr };

    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CStringNonStrict)(WKStringRef, char*, size_t) { nullptr };

    void (*release)(WKTypeRef) { nullptr };
};

// Engine thread. The entry points, resolved once out of the loaded engine, or
// null while the engine has not finished starting.
const WebKitApi* webKitApi();

void releaseObject(WKTypeRef object);
WKStringRef createString(const std::string& text);

// --- Shared state -----------------------------------------------------------

extern std::atomic<HWND> g_hostWindow;
extern std::atomic<DWORD> g_frameThreadId;
extern std::atomic<DWORD> g_engineThreadId;
extern std::atomic<bool> g_hooksReady;

// The chrome field keys are routed to, one of HB_INPUT_TARGET_*.
extern std::atomic<int> g_textTarget;

// Whether the find bar is up. The hooks read it: Escape closes the find
// session before it stops a load, and Enter in the find field is "next match"
// rather than a submitted address.
extern std::atomic<bool> g_findOpen;

// The tab the host is showing, refreshed every frame from the registry's
// published list so the engine thread never has to be asked.
extern std::atomic<int> g_activeTabId;

void setError(const std::string& message);
std::string currentError();

void bumpRevision();

// --- Events -----------------------------------------------------------------

struct Event {
    int kind { HB_INPUT_EVENT_NONE };
    int argument { 0 };
    int target { HB_INPUT_TARGET_NONE };
};

void queueEvent(int kind, int argument = 0, int target = HB_INPUT_TARGET_NONE);
bool popEvent(Event& event);

// --- The page's windows -----------------------------------------------------
//
// One child window per tab, recorded on the engine thread as each page is
// created. The frame thread reads the map to answer which window holds the
// keyboard and to hand it back.

void recordTabWindow(int tabId, HWND window);
void forgetTabWindow(int tabId);
HWND tabWindow(int tabId);
bool isTabWindow(HWND window);

// --- Hooks ------------------------------------------------------------------

// Frame thread. Installs the hook on this thread and asks the engine thread to
// install its own.
void installHooks();
void removeHooks();

// Engine thread. Installs the hook the page's windows are pumped under.
void installEngineHook();
void removeEngineHook();

// How many pointer presses have landed on the chrome's window.
int pressSerial();

// --- Focus ------------------------------------------------------------------

int focusOwner();
void focusChrome();
void focusContent();
void cycleFocus(int direction);

// Engine thread. Fills the focus fields of a page's UI client, so the page can
// ask for the focus and give it back.
void installFocusClient(int tabId, hb_wk_page_ui_client_v19* client);

// Frame thread. Notices a focus change the user made with the pointer and
// queues it, so the chrome can draw which side has the keyboard.
void serviceFocus();

// --- Zoom -------------------------------------------------------------------

void zoomStep(int direction);
void zoomReset();
void zoomSet(int percent);
int zoomPercent();
std::string zoomOrigin();

// Frame thread. Applies the stored level to any tab that has navigated to an
// origin whose level differs from the one the page is showing at.
void serviceZoom();

void loadZoomLevels();
void saveZoomLevels();

// --- Find -------------------------------------------------------------------

void findOpen();
void findClose();
void findSearch(const std::string& text, bool backwards);
int findMatchCount();
int findMatchIndex();
bool findSearched();

// Engine thread. Installs the find client on a page, and takes it off again.
void attachFindClient(int tabId, WKPageRef page);
void detachFindClient(int tabId, WKPageRef page);

// --- Printing ---------------------------------------------------------------

void printActiveTab();
void stopPrinting();

// --- Profile ----------------------------------------------------------------

// %LOCALAPPDATA%\HarmonyBrowser, created if it is not there, or an empty path
// when the folder cannot be reached.
std::wstring profileDirectory();

// The scheme and authority of a URL -- "https://example.com" -- which is what a
// per-site setting is keyed by. Empty for a URL that has no authority.
std::string originOfURL(const std::string& url);

} // namespace harmony_input

#endif
