#include "harmony_data_store_internal.h"

#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

#include <algorithm>
#include <mutex>

// The profile itself: the two stores, which tab browses in which, and the pump
// that drives every WebKit-side step the module takes.
//
// The persistent store is created once, with every storage location set inside
// the profile root. The ephemeral store is created when the first private tab
// asks for one and released when the last one closes, so a private session ends
// with the tabs that were in it rather than with the process.

namespace harmony::datastore {

namespace {

WKWebsiteDataStoreRef g_persistent = nullptr;
WKWebsiteDataStoreRef g_ephemeral = nullptr;
bool g_prepared = false;
bool g_observersInstalled = false;

// WebKit thread. Set by the private-tab open queued ahead of the registry's own
// open command, consumed by the page configuration that open produces.
bool g_nextTabPrivate = false;

std::mutex g_privateMutex;
std::vector<int> g_privateTabs;

void setDirectory(
    void (*setter)(WKWebsiteDataStoreConfigurationRef, WKStringRef),
    WKWebsiteDataStoreConfigurationRef configuration,
    const std::wstring& path
)
{
    if (!setter || path.empty())
        return;

    WKStringRef value = wkString(narrow(path));
    if (!value)
        return;
    setter(configuration, value);
    wkRelease(value);
}

WKWebsiteDataStoreRef createPersistentStore()
{
    Api& wk = api();
    WKWebsiteDataStoreConfigurationRef configuration = wk.dataStoreConfigurationCreate();
    if (!configuration) {
        setError("WKWebsiteDataStoreConfigurationCreate failed");
        return nullptr;
    }

    const Layout& paths = layout();
    setDirectory(wk.setGeneralStorageDirectory, configuration, paths.generalStorage);
    setDirectory(wk.setNetworkCacheDirectory, configuration, paths.networkCache);
    setDirectory(wk.setIndexedDBDatabaseDirectory, configuration, paths.legacyIndexedDB);
    setDirectory(wk.setLocalStorageDirectory, configuration, paths.legacyLocalStorage);
    setDirectory(wk.setWebSQLDatabaseDirectory, configuration, paths.legacyWebSQL);
    setDirectory(wk.setCacheStorageDirectory, configuration, paths.legacyCacheStorage);
    setDirectory(wk.setMediaKeysStorageDirectory, configuration, paths.mediaKeys);
    setDirectory(wk.setResourceLoadStatisticsDirectory, configuration, paths.resourceLoadStatistics);
    setDirectory(wk.setServiceWorkerRegistrationDirectory, configuration, paths.serviceWorkerRegistrations);
    setDirectory(wk.setResourceMonitorThrottlerDirectory, configuration, paths.resourceMonitorThrottler);
    setDirectory(wk.setCookieStorageFile, configuration, paths.cookieFile);

    // A site may fill a fifth of what the profile's volume has free before it
    // is asked to make room, which is the ratio WebKit's own embedders use. The
    // fixed per-origin quota is the floor under it for a small disk.
    if (wk.setOriginQuotaRatio)
        wk.setOriginQuotaRatio(configuration, 0.2);
    if (wk.setPerOriginStorageQuota)
        wk.setPerOriginStorageQuota(configuration, static_cast<uint64_t>(1) << 30);

    WKWebsiteDataStoreRef store = wk.dataStoreCreateWithConfiguration(configuration);
    wkRelease(configuration);
    if (!store)
        setError("WKWebsiteDataStoreCreateWithConfiguration failed");
    return store;
}

// WebKit thread. A tab whose page has gone stops being private, and the private
// session ends with the last of them.
void pageDestroying(int tabId, void*, void*)
{
    forgetTab(tabId);
}

// WebKit thread, once per run-loop cycle.
void engineCycle(void*)
{
    pump();
}

// WebKit thread, as the engine stops. A WK object's reference count belongs to
// the thread that owns the object, and this is the last moment that thread has.
void engineTeardown(void*)
{
    wkRelease(g_ephemeral);
    g_ephemeral = nullptr;
    wkRelease(g_persistent);
    g_persistent = nullptr;
}

void installObservers()
{
    if (g_observersInstalled)
        return;
    g_observersInstalled = true;
    hb_tabs_add_page_observer(nullptr, pageDestroying, nullptr);
    hb_tabs_add_cycle_hook(engineCycle, nullptr);
    hb_tabs_add_teardown_hook(engineTeardown, nullptr);
}

} // namespace

// --- Stores -----------------------------------------------------------------

WKWebsiteDataStoreRef persistentStore()
{
    return g_persistent;
}

WKWebsiteDataStoreRef ephemeralStore()
{
    if (g_ephemeral)
        return g_ephemeral;
    if (!api().dataStoreCreateNonPersistent) {
        setError("this WebKit build exports no non-persistent data store, so private tabs would not be private");
        return nullptr;
    }

    g_ephemeral = api().dataStoreCreateNonPersistent();
    if (!g_ephemeral)
        setError("WKWebsiteDataStoreCreateNonPersistentDataStore failed");
    return g_ephemeral;
}

void releaseEphemeralStoreIfUnused()
{
    if (!g_ephemeral || privateTabCount() > 0)
        return;

    // The pages that were in it hold their own references, so this only drops
    // the module's: the session goes when the last private page does.
    wkRelease(g_ephemeral);
    g_ephemeral = nullptr;
}

// --- Private tabs -----------------------------------------------------------

void markNextTabPrivate()
{
    g_nextTabPrivate = true;
}

void forgetTab(int tabId)
{
    bool wasPrivate = false;
    {
        std::lock_guard<std::mutex> lock(g_privateMutex);
        const auto at = std::find(g_privateTabs.begin(), g_privateTabs.end(), tabId);
        if (at != g_privateTabs.end()) {
            g_privateTabs.erase(at);
            wasPrivate = true;
        }
    }
    if (wasPrivate)
        releaseEphemeralStoreIfUnused();
}

bool tabIsPrivate(int tabId)
{
    std::lock_guard<std::mutex> lock(g_privateMutex);
    return std::find(g_privateTabs.begin(), g_privateTabs.end(), tabId) != g_privateTabs.end();
}

int privateTabCount()
{
    std::lock_guard<std::mutex> lock(g_privateMutex);
    return static_cast<int>(g_privateTabs.size());
}

void pump()
{
    if (!g_prepared) {
        // The engine may not have finished loading when the registry first
        // asked, and a profile that gave up then would be a browser that
        // forgets everything for the rest of the run. Retried about once a
        // second rather than on every cycle, because the answer cannot change
        // between two turns of a run loop.
        static int cyclesUntilRetry = 0;
        if (cyclesUntilRetry > 0) {
            --cyclesUntilRetry;
            return;
        }
        if (!hb_data_store_prepare())
            cyclesUntilRetry = 120;
        return;
    }

    removalPump();
    recordsPump();
    sessionPump();
}

} // namespace harmony::datastore

// --- The C surface this module's own lifetime is driven through -------------

using namespace harmony::datastore;

extern "C" int hb_data_store_prepare(void)
{
    if (g_prepared)
        return 1;

    // First, and whether or not the rest succeeds: the cycle hook is what
    // retries this, so a profile that could not be prepared on the engine's
    // first turn still gets a second one.
    installObservers();

    if (!loadApi())
        return 0;
    if (!prepareLayout())
        return 0;

    g_persistent = createPersistentStore();
    if (!g_persistent)
        return 0;

    ledgerLoad();
    sessionLoad();

    g_prepared = true;
    clearError();
    return 1;
}

extern "C" int hb_data_store_ready(void)
{
    return g_prepared ? 1 : 0;
}

extern "C" void hb_data_store_shutdown(void)
{
    if (!g_prepared)
        return;

    // The pages are still open here, which is why this is the host's call and
    // not the registry's teardown hook: capturing a tab's history needs its
    // page, and teardown runs after every tab has been closed.
    sessionShutdown();
    ledgerSave();
    g_prepared = false;

    // Last, so the session write queued above reaches disk before the process
    // stops having a thread to write it on. The stores are released by the
    // registry's teardown hook, on the thread that owns them.
    stopWorker();
}

extern "C" void* hb_data_store_persistent(void)
{
    return const_cast<void*>(g_persistent);
}

extern "C" void* hb_data_store_ephemeral(void)
{
    if (!g_prepared)
        return nullptr;
    return const_cast<void*>(ephemeralStore());
}

extern "C" void hb_data_store_apply_to_page_configuration(void* page_configuration, int tab_id)
{
    if (!page_configuration || !hb_data_store_prepare())
        return;

    const WKPageConfigurationRef configuration = page_configuration;
    Api& wk = api();

    // A page WebKit created for window.open carries the opener's store already.
    // Reading it is what keeps a popup from a private tab private.
    bool inheritedPrivate = false;
    if (wk.pageConfigurationGetWebsiteDataStore && g_ephemeral)
        inheritedPrivate = wk.pageConfigurationGetWebsiteDataStore(configuration) == g_ephemeral;

    const bool wanted = g_nextTabPrivate;
    g_nextTabPrivate = false;

    if (inheritedPrivate || wanted) {
        WKWebsiteDataStoreRef store = ephemeralStore();
        if (store) {
            if (tab_id > 0) {
                std::lock_guard<std::mutex> lock(g_privateMutex);
                if (std::find(g_privateTabs.begin(), g_privateTabs.end(), tab_id) == g_privateTabs.end())
                    g_privateTabs.push_back(tab_id);
            }
            wk.pageConfigurationSetWebsiteDataStore(configuration, store);
            return;
        }
        // Without an ephemeral store there is no private browsing to give, and
        // a tab that quietly wrote to the profile instead would be worse than
        // one that says so through hb_data_store_error.
    }

    if (g_persistent)
        wk.pageConfigurationSetWebsiteDataStore(configuration, g_persistent);
}

extern "C" void hb_data_store_apply_to_context_configuration(void* context_configuration)
{
    if (!context_configuration || !loadApi())
        return;

    const WKContextConfigurationRef configuration = context_configuration;
    Api& wk = api();

    // A process kept warm after its page closed still holds that page's memory
    // cache, so a browser that offers private browsing must not keep one.
    if (wk.contextConfigurationSetUsesWebProcessCache)
        wk.contextConfigurationSetUsesWebProcessCache(configuration, false);
    if (wk.contextConfigurationSetPrewarmsProcesses)
        wk.contextConfigurationSetPrewarmsProcesses(configuration, false);
}

extern "C" const char* hb_data_store_profile_path(void)
{
    return profilePathUtf8().c_str();
}

extern "C" const char* hb_data_store_error(void)
{
    static thread_local std::string storage;
    storage = currentError();
    return storage.c_str();
}

extern "C" int hb_data_store_open_private_tab(const char* url)
{
    // The mark and the open go into the registry's one command queue in this
    // order and are drained in it, so the tab this opens is the tab the mark
    // applies to however busy the engine is.
    hb_tabs_invoke_on_webkit_thread([](void*) { markNextTabPrivate(); }, nullptr);
    return hb_tabs_open(url && *url ? url : "", 1);
}

extern "C" int hb_data_store_tab_is_private(int tab_id)
{
    return tabIsPrivate(tab_id) ? 1 : 0;
}

extern "C" int hb_data_store_private_tab_count(void)
{
    return privateTabCount();
}
