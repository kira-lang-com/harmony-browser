#ifndef HARMONY_SETTINGS_INTERNAL_H
#define HARMONY_SETTINGS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "harmony_settings.h"
#include "harmony_text.h"

// The settings module's own state, shared between its translation units.
//
// One record, one lock. Every value a person can change is in `Values`, every
// read copies what it needs out from under the lock, and no WebKit call and no
// disk write ever happens while the lock is held: a preference change that
// waited on the engine thread would be a preference change that stalled a frame.

namespace harmony::settings {

using harmony::text::narrow;
using harmony::text::widen;

// Every WK object is an opaque pointer in C, so one alias serves them all. The
// entry points are resolved out of the engine the tabs registry loaded, so this
// module needs no WebKit checkout to build.
using WKTypeRef = const void*;
using WKPageRef = const void*;
using WKPageConfigurationRef = const void*;
using WKPreferencesRef = const void*;
using WKUserContentControllerRef = const void*;
using WKUserScriptRef = const void*;
using WKStringRef = const void*;

// `_WKUserScriptInjectionTime`.
constexpr int kInjectAtDocumentStart = 0;

struct WebKitApi {
    WKPageConfigurationRef (*pageCopyPageConfiguration)(WKPageRef) { nullptr };
    WKPreferencesRef (*pageConfigurationGetPreferences)(WKPageConfigurationRef) { nullptr };
    WKUserContentControllerRef (*pageConfigurationGetUserContentController)(WKPageConfigurationRef) { nullptr };

    void (*preferencesSetJavaScriptEnabled)(WKPreferencesRef, bool) { nullptr };
    void (*preferencesSetLoadsImagesAutomatically)(WKPreferencesRef, bool) { nullptr };
    void (*preferencesSetJavaScriptCanOpenWindowsAutomatically)(WKPreferencesRef, bool) { nullptr };

    WKUserScriptRef (*userScriptCreateWithSource)(WKStringRef, int, bool) { nullptr };
    void (*userContentControllerAddUserScript)(WKUserContentControllerRef, WKUserScriptRef) { nullptr };
    void (*userContentControllerRemoveAllUserScripts)(WKUserContentControllerRef) { nullptr };

    double (*pageGetPageZoomFactor)(WKPageRef) { nullptr };
    void (*pageSetPageZoomFactor)(WKPageRef, double) { nullptr };

    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    WKTypeRef (*retain)(WKTypeRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };
};

// Engine thread. The entry points, resolved once out of the loaded engine, or
// null while the engine has not finished starting.
const WebKitApi* webKitApi();

void releaseObject(WKTypeRef object);

// --- The values --------------------------------------------------------------

struct SearchEngine {
    std::string name;
    std::string query;
};

// The browser's own defaults: what a profile that has never been configured
// behaves like, and what a value cleared to nothing falls back to.
constexpr const char* kDefaultHomeURL = "https://www.google.com/";
constexpr int kDefaultZoomPercent = 100;

// The ladder a zoom level is held to, the same one the input system steps along:
// a level outside it is a level no other surface of this browser can show.
constexpr int kMinimumZoomPercent = 25;
constexpr int kMaximumZoomPercent = 500;

// The token a query template carries where the typed phrase goes.
constexpr const char* kSearchTermsToken = "{searchTerms}";

struct Values {
    std::string homeURL { kDefaultHomeURL };
    int startup { HB_SETTINGS_STARTUP_RESTORE_SESSION };

    std::vector<SearchEngine> engines;
    int defaultEngine { 0 };

    int zoomPercent { kDefaultZoomPercent };

    bool javaScript { true };
    bool images { true };
    bool popups { false };
    bool doNotTrack { false };

    std::string downloadDirectory;
};

// The lock every field above is read and written under.
extern std::mutex g_mutex;
extern Values g_values;

// Bumped on every change a surface would draw differently.
extern std::atomic<int> g_revision;

// Bumped whenever a value the ENGINE has to be told about moves. The cycle hook
// compares it against what it last applied, so a run of edits costs one pass
// over the open pages rather than one per keystroke.
extern std::atomic<int> g_engineGeneration;

// Reads the file on the first call and prepares the profile directory. Every
// public entry point of this module goes through it, so no caller can read a
// value the file has not been consulted for.
void ensureLoaded();

// Marks the file as needing a write, and remembers when. A run of edits settles
// before it costs a write.
void markDirty();

// Takes the lock, runs `mutate`, and marks everything the change implies.
template<typename Mutate>
void withValues(Mutate mutate)
{
    ensureLoaded();
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        mutate(g_values);
    }
    markDirty();
    g_revision.fetch_add(1);
    g_engineGeneration.fetch_add(1);
}

void setError(const std::string& message);
std::string currentError();

// --- The file ----------------------------------------------------------------

// `<profile>\Settings.txt`, or empty when the profile directory could not be
// reached. Shares the data store's layout: this browser keeps one directory.
const std::wstring& settingsFilePath();
const std::string& settingsFilePathUtf8();

// Reads the file into `g_values`, leaving the defaults in place for anything it
// does not mention.
void loadSettings();

// Writes the file when it is dirty and has settled. Called from the frame hook.
void serviceSave();

// Writes immediately, whatever the settle timer says.
void flushSave();

// --- Search engines ----------------------------------------------------------

// The engines a profile that has never been configured starts with.
std::vector<SearchEngine> defaultEngines();

// Whether a template can carry a phrase at all.
bool isUsableQueryTemplate(const std::string& query);

// `text` with everything outside RFC 3986's unreserved set percent-encoded.
std::string percentEncoded(const std::string& text);

// The chosen engine's template with `query` encoded into it.
std::string searchURL(const std::string& query);

// --- The engine --------------------------------------------------------------

// Registers this module's page observer and cycle hook with the tabs registry,
// and applies the current settings to every page that already exists. Idempotent.
void attachToEngine();

// Engine thread. Applies every preference this module owns to one page.
void applyToPage(int tabId, WKPageRef page);

// Frame thread. Queues a pass over every open page when a value moved.
void serviceEngine();

// Frame thread. Applies the startup behaviour once, before the session can be
// restored.
void serviceStartup();

// Frame thread. Releases the engine-side state at shutdown.
void detachFromEngine();

// --- The folder picker -------------------------------------------------------

// Remembers the window a picker is raised over.
void setPickerOwner(HWND window);

void openFolderPicker();
bool folderPickerIsOpen();
void stopFolderPicker();

} // namespace harmony::settings

#endif
