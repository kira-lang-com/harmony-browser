#include "harmony_settings_internal.h"

#include "harmony_data_store.h"
#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

#include <algorithm>
#include <vector>

// What a setting DOES to a page.
//
// The tabs registry owns the engine, the thread and the three clients a page
// carries, so nothing here installs one: this module registers a page observer,
// a run-loop cycle hook and a teardown hook with that registry, and does its
// work on the engine thread from inside them.
//
// A page's preferences and its user content controller are its own -- the
// registry builds one configuration per tab -- so every setting is applied per
// page rather than once to a shared object.

namespace harmony::settings {

namespace {

WebKitApi g_api;
bool g_usable { false };

std::atomic<bool> g_attached { false };

// Engine thread. What each tab was last told, so a tab is only spoken to when
// its address or the settings behind it have actually moved.
struct AppliedPage {
    int tabId { 0 };
    std::string url;
    int generation { -1 };
};
std::vector<AppliedPage> g_applied;

// Frame thread. The last generation a pass was queued for.
int g_queuedGeneration { -1 };

// Frame thread. Whether the startup behaviour has been enforced yet.
bool g_startupApplied { false };

// The document `navigator.doNotTrack` is answered from. It is defined on the
// prototype rather than assigned on the instance because that is where the
// property lives: a page that reads it off `Navigator.prototype` -- which every
// tracker library does -- must see the same answer as one that reads it off
// `navigator`.
constexpr const char* kDoNotTrackScript =
    "(function(){"
    "var value='1';"
    "var descriptor={get:function(){return value;},configurable:true,enumerable:true};"
    "try{Object.defineProperty(Navigator.prototype,'doNotTrack',descriptor);}catch(e){}"
    "try{Object.defineProperty(window,'doNotTrack',descriptor);}catch(e){}"
    "})();";

template<typename Function>
bool resolve(const char* name, Function& function, std::string& missing)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
    if (function)
        return true;

    if (!missing.empty())
        missing += ", ";
    missing += name;
    return false;
}

bool resolveApi()
{
    // The tabs registry owns the runtime, so its symbol seam is what answers
    // here: this module has no engine of its own to load.
    if (!hb_tabs_webkit_symbol("WKRelease"))
        return false;

    WebKitApi api;
    std::string missing;
    bool ok = true;
    ok &= resolve("WKPageCopyPageConfiguration", api.pageCopyPageConfiguration, missing);
    ok &= resolve("WKPageConfigurationGetPreferences", api.pageConfigurationGetPreferences, missing);
    ok &= resolve("WKPageConfigurationGetUserContentController", api.pageConfigurationGetUserContentController, missing);
    ok &= resolve("WKPreferencesSetJavaScriptEnabled", api.preferencesSetJavaScriptEnabled, missing);
    ok &= resolve("WKPreferencesSetLoadsImagesAutomatically", api.preferencesSetLoadsImagesAutomatically, missing);
    ok &= resolve("WKPreferencesSetJavaScriptCanOpenWindowsAutomatically", api.preferencesSetJavaScriptCanOpenWindowsAutomatically, missing);
    ok &= resolve("WKUserScriptCreateWithSource", api.userScriptCreateWithSource, missing);
    ok &= resolve("WKUserContentControllerAddUserScript", api.userContentControllerAddUserScript, missing);
    ok &= resolve("WKUserContentControllerRemoveAllUserScripts", api.userContentControllerRemoveAllUserScripts, missing);
    ok &= resolve("WKPageGetPageZoomFactor", api.pageGetPageZoomFactor, missing);
    ok &= resolve("WKPageSetPageZoomFactor", api.pageSetPageZoomFactor, missing);
    ok &= resolve("WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString, missing);
    ok &= resolve("WKRetain", api.retain, missing);
    ok &= resolve("WKRelease", api.release, missing);

    if (!ok) {
        setError("this WebKit build does not export " + missing);
        return false;
    }

    g_api = api;
    return true;
}

// The values one pass applies, copied out from under the lock so no WebKit call
// is made while it is held.
struct Snapshot {
    bool javaScript { true };
    bool images { true };
    bool popups { false };
    bool doNotTrack { false };
    double zoomFactor { 1.0 };
    int generation { 0 };
};

Snapshot readSnapshot()
{
    ensureLoaded();

    Snapshot snapshot;
    snapshot.generation = g_engineGeneration.load();
    std::lock_guard<std::mutex> lock(g_mutex);
    snapshot.javaScript = g_values.javaScript;
    snapshot.images = g_values.images;
    snapshot.popups = g_values.popups;
    snapshot.doNotTrack = g_values.doNotTrack;
    snapshot.zoomFactor = static_cast<double>(g_values.zoomPercent) / 100.0;
    return snapshot;
}

AppliedPage* findApplied(int tabId)
{
    for (auto& entry : g_applied) {
        if (entry.tabId == tabId)
            return &entry;
    }
    return nullptr;
}

void applyPreferences(WKPreferencesRef preferences, const Snapshot& snapshot)
{
    if (!preferences)
        return;
    g_api.preferencesSetJavaScriptEnabled(preferences, snapshot.javaScript);
    g_api.preferencesSetLoadsImagesAutomatically(preferences, snapshot.images);
    g_api.preferencesSetJavaScriptCanOpenWindowsAutomatically(preferences, snapshot.popups);
}

// The tracking preference is a script rather than a header because this port's C
// API exposes no way to add one to a request: `WKWebsitePoliciesSetCustomHeaderFields`
// is present in the headers and does nothing in the implementation. What a page
// can actually observe -- and what every consent library reads -- is
// `navigator.doNotTrack`, and that is answered here.
//
// Every script on the controller is this one, because the registry builds one
// user content controller per tab and nothing else in this browser adds a script
// to it. So clearing the list is exactly "take the previous answer down".
void applyDoNotTrack(WKUserContentControllerRef controller, const Snapshot& snapshot)
{
    if (!controller)
        return;

    g_api.userContentControllerRemoveAllUserScripts(controller);
    if (!snapshot.doNotTrack)
        return;

    WKStringRef source = g_api.stringCreateWithUTF8CString(kDoNotTrackScript);
    if (!source)
        return;

    // At document start and in every frame: a script that ran at document end
    // would answer after the page had already asked.
    WKUserScriptRef script = g_api.userScriptCreateWithSource(source, kInjectAtDocumentStart, false);
    releaseObject(source);
    if (!script)
        return;

    g_api.userContentControllerAddUserScript(controller, script);
    releaseObject(script);
}

// The level a site with no remembered zoom is read at.
//
// A page the input system has applied a per-site level to is already at that
// level, so it is left alone: this only ever moves a page that is still at the
// engine's own scale, which is precisely the case the default is for.
void applyDefaultZoom(WKPageRef page, const Snapshot& snapshot)
{
    if (snapshot.zoomFactor == 1.0)
        return;
    if (g_api.pageGetPageZoomFactor(page) != 1.0)
        return;
    g_api.pageSetPageZoomFactor(page, snapshot.zoomFactor);
}

void applyPage(int tabId, WKPageRef page, const Snapshot& snapshot)
{
    if (!page)
        return;

    WKPageConfigurationRef configuration = g_api.pageCopyPageConfiguration(page);
    if (configuration) {
        // Both accessors answer with the page's own objects, borrowed from the
        // configuration copy; each is retained for as long as it is used so
        // releasing the copy cannot take it away underneath the calls.
        WKPreferencesRef preferences = g_api.pageConfigurationGetPreferences(configuration);
        if (preferences) {
            (void)g_api.retain(preferences);
            applyPreferences(preferences, snapshot);
            releaseObject(preferences);
        }

        WKUserContentControllerRef controller = g_api.pageConfigurationGetUserContentController(configuration);
        if (controller) {
            (void)g_api.retain(controller);
            applyDoNotTrack(controller, snapshot);
            releaseObject(controller);
        }

        releaseObject(configuration);
    }

    applyDefaultZoom(page, snapshot);

    AppliedPage* entry = findApplied(tabId);
    if (!entry) {
        g_applied.push_back(AppliedPage { tabId, hb_tabs_url(tabId), snapshot.generation });
        return;
    }
    entry->url = hb_tabs_url(tabId);
    entry->generation = snapshot.generation;
}

// --- The registry's seams -----------------------------------------------------

void pageCreated(int tabId, void* page, void*)
{
    if (!webKitApi())
        return;
    applyPage(tabId, page, readSnapshot());
}

void pageDestroying(int tabId, void*, void*)
{
    const auto position = std::find_if(g_applied.begin(), g_applied.end(), [tabId](const AppliedPage& entry) {
        return entry.tabId == tabId;
    });
    if (position != g_applied.end())
        g_applied.erase(position);
}

// Runs on the WebKit thread, once per run-loop cycle, before WebKit's own.
//
// A page is spoken to when its address moved -- a fresh document starts at the
// engine's own scale, so the default zoom has to be put back -- or when a
// setting moved under it.
void engineCycle(void*)
{
    if (!webKitApi())
        return;

    const Snapshot snapshot = readSnapshot();
    const int count = hb_tabs_count();

    std::vector<AppliedPage> live;
    live.reserve(static_cast<size_t>(count));

    for (int index = 0; index < count; ++index) {
        const int tabId = hb_tabs_id_at(index);
        if (tabId <= 0)
            continue;

        WKPageRef page = hb_tabs_page(tabId);
        if (!page)
            continue;

        const std::string url = hb_tabs_url(tabId);
        const AppliedPage* previous = findApplied(tabId);
        if (!previous || previous->url != url || previous->generation != snapshot.generation)
            applyPage(tabId, page, snapshot);

        if (const AppliedPage* entry = findApplied(tabId))
            live.push_back(*entry);
    }

    g_applied = std::move(live);
}

void engineTeardown(void*)
{
    g_applied.clear();
}

} // namespace

const WebKitApi* webKitApi()
{
    // Only success is remembered. The first ask can arrive before the registry
    // has finished loading the engine, and latching that would leave every
    // setting inert for the rest of the run because it was asked one cycle too
    // early.
    if (!g_usable)
        g_usable = resolveApi();
    return g_usable ? &g_api : nullptr;
}

void releaseObject(WKTypeRef object)
{
    if (object && g_usable && g_api.release)
        g_api.release(object);
}

void attachToEngine()
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
    hb_tabs_add_cycle_hook(engineCycle, nullptr);
    hb_tabs_add_teardown_hook(engineTeardown, nullptr);
}

void applyToPage(int tabId, WKPageRef page)
{
    if (!webKitApi())
        return;
    applyPage(tabId, page, readSnapshot());
}

void serviceEngine()
{
    const int generation = g_engineGeneration.load();
    if (generation == g_queuedGeneration)
        return;
    g_queuedGeneration = generation;

    // The cycle hook is what carries the pass out; asking for a turn rather than
    // waiting for the engine's next idle one means a switch a person just
    // flicked reaches the page in this frame rather than in the next.
    hb_tabs_invoke_on_webkit_thread([](void*) { engineCycle(nullptr); }, nullptr);
}

// The tabs the last run was closed with are the data store's to hand out and the
// host's to open. A browser told to start on a new tab hands them back before
// anyone can ask for them, which is exactly what "do not restore" means: the
// entries are dropped, the session file is left alone, and this run writes its
// own tabs over it as usual.
void serviceStartup()
{
    if (g_startupApplied)
        return;
    // The list is read off disk while the engine starts, before the registry
    // reports itself ready, so there is nothing to drop until it is.
    if (!hb_tabs_ready())
        return;

    g_startupApplied = true;
    ensureLoaded();

    int behaviour = HB_SETTINGS_STARTUP_RESTORE_SESSION;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        behaviour = g_values.startup;
    }
    if (behaviour == HB_SETTINGS_STARTUP_NEW_TAB)
        hb_data_store_session_restore_finished();
}

void detachFromEngine()
{
    if (!g_attached.load())
        return;
    hb_tabs_invoke_on_webkit_thread(engineTeardown, nullptr);
}

} // namespace harmony::settings
