#include "harmony_data_store_internal.h"

#include "harmony_tabs_embed.h"

#include <cstdio>
#include <mutex>

// The engine, as the profile reaches it.
//
// The tabs registry owns WebKit2.dll: it decides which build is loaded and runs
// the one thread every WebKit object lives on. So the profile resolves its
// symbols through that registry rather than loading a module of its own, and
// falls back to the module handle only when a symbol is asked for before the
// registry has finished loading.

namespace harmony::datastore {

namespace {

Api g_api;
bool g_resolved = false;
bool g_usable = false;

std::mutex g_errorMutex;
std::string g_error;

void* engineSymbol(const char* name)
{
    if (void* fromRegistry = hb_tabs_webkit_symbol(name))
        return fromRegistry;
    if (HMODULE module = GetModuleHandleW(L"WebKit2.dll"))
        return reinterpret_cast<void*>(GetProcAddress(module, name));
    return nullptr;
}

// A symbol the profile cannot work without. Its absence is named in one list
// rather than one message per symbol, so a WebKit build that is wrong is wrong
// once.
template<typename Function>
bool require(const char* name, Function& function, std::string& missing)
{
    function = reinterpret_cast<Function>(engineSymbol(name));
    if (function)
        return true;

    if (!missing.empty())
        missing += ", ";
    missing += name;
    return false;
}

// A symbol the profile does more with when it is there. Every caller of one of
// these checks it before calling.
template<typename Function>
void optional(const char* name, Function& function)
{
    function = reinterpret_cast<Function>(engineSymbol(name));
}

bool resolveApi()
{
    Api api;
    api.module = GetModuleHandleW(L"WebKit2.dll");

    std::string missing;
    bool ok = true;
    ok &= require("WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString, missing);
    ok &= require("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize, missing);
    ok &= require("WKStringGetUTF8CString", api.stringGetUTF8CString, missing);
    ok &= require("WKRelease", api.release, missing);
    ok &= require("WKRetain", api.retain, missing);
    ok &= require("WKWebsiteDataStoreConfigurationCreate", api.dataStoreConfigurationCreate, missing);
    ok &= require("WKWebsiteDataStoreCreateWithConfiguration", api.dataStoreCreateWithConfiguration, missing);
    ok &= require("WKWebsiteDataStoreConfigurationSetGeneralStorageDirectory", api.setGeneralStorageDirectory, missing);
    ok &= require("WKWebsiteDataStoreConfigurationSetNetworkCacheDirectory", api.setNetworkCacheDirectory, missing);
    ok &= require("WKWebsiteDataStoreConfigurationSetCookieStorageFile", api.setCookieStorageFile, missing);
    ok &= require("WKPageConfigurationSetWebsiteDataStore", api.pageConfigurationSetWebsiteDataStore, missing);

    if (!ok) {
        setError("this WebKit build does not export " + missing);
        return false;
    }

    optional("WKArrayGetSize", api.arrayGetSize);
    optional("WKArrayGetItemAtIndex", api.arrayGetItemAtIndex);
    optional("WKSecurityOriginCreateFromString", api.securityOriginCreateFromString);
    optional("WKSecurityOriginCopyToString", api.securityOriginCopyToString);

    optional("WKWebsiteDataStoreConfigurationSetIndexedDBDatabaseDirectory", api.setIndexedDBDatabaseDirectory);
    optional("WKWebsiteDataStoreConfigurationSetLocalStorageDirectory", api.setLocalStorageDirectory);
    optional("WKWebsiteDataStoreConfigurationSetWebSQLDatabaseDirectory", api.setWebSQLDatabaseDirectory);
    optional("WKWebsiteDataStoreConfigurationSetCacheStorageDirectory", api.setCacheStorageDirectory);
    optional("WKWebsiteDataStoreConfigurationSetMediaKeysStorageDirectory", api.setMediaKeysStorageDirectory);
    optional("WKWebsiteDataStoreConfigurationSetResourceLoadStatisticsDirectory", api.setResourceLoadStatisticsDirectory);
    optional("WKWebsiteDataStoreConfigurationSetServiceWorkerRegistrationDirectory", api.setServiceWorkerRegistrationDirectory);
    optional("WKWebsiteDataStoreConfigurationSetResourceMonitorThrottlerDirectory", api.setResourceMonitorThrottlerDirectory);
    optional("WKWebsiteDataStoreConfigurationSetPerOriginStorageQuota", api.setPerOriginStorageQuota);
    optional("WKWebsiteDataStoreConfigurationSetOriginQuotaRatio", api.setOriginQuotaRatio);

    optional("WKWebsiteDataStoreCreateNonPersistentDataStore", api.dataStoreCreateNonPersistent);
    optional("WKWebsiteDataStoreGetHTTPCookieStore", api.dataStoreGetCookieStore);
    optional("WKHTTPCookieStoreDeleteAllCookies", api.cookieStoreDeleteAllCookies);

    optional("WKPageConfigurationGetWebsiteDataStore", api.pageConfigurationGetWebsiteDataStore);
    optional("WKContextConfigurationSetUsesWebProcessCache", api.contextConfigurationSetUsesWebProcessCache);
    optional("WKContextConfigurationSetPrewarmsProcessesAutomatically", api.contextConfigurationSetPrewarmsProcesses);

    optional("WKWebsiteDataStoreRemoveNetworkCache", api.removeNetworkCache);
    optional("WKWebsiteDataStoreRemoveMemoryCaches", api.removeMemoryCaches);
    optional("WKWebsiteDataStoreRemoveAllFetchCaches", api.removeAllFetchCaches);
    optional("WKWebsiteDataStoreRemoveFetchCacheForOrigin", api.removeFetchCacheForOrigin);
    optional("WKWebsiteDataStoreRemoveAllServiceWorkerRegistrations", api.removeAllServiceWorkerRegistrations);
    optional("WKWebsiteDataStoreRemoveAllIndexedDatabases", api.removeAllIndexedDatabases);
    optional("WKWebsiteDataStoreRemoveLocalStorage", api.removeLocalStorage);
    optional("WKWebsiteDataStoreRemoveITPDataForDomain", api.removeITPDataForDomain);
    optional("WKWebsiteDataStoreClearStorage", api.clearStorage);
    optional("WKWebsiteDataStoreStatisticsClearInMemoryAndPersistentStore", api.statisticsClearInMemoryAndPersistentStore);
    optional("WKWebsiteDataStoreStatisticsClearInMemoryAndPersistentStoreModifiedSinceHours", api.statisticsClearModifiedSinceHours);
    optional("WKWebsiteDataStoreClearPrivateClickMeasurementsThroughWebsiteDataRemoval", api.clearPrivateClickMeasurements);
    optional("WKWebsiteDataStoreSyncLocalStorage", api.syncLocalStorage);
    optional("WKWebsiteDataStoreFlushNetworkProcessIPC", api.flushNetworkProcessIPC);
    optional("WKWebsiteDataStoreTerminateNetworkProcess", api.terminateNetworkProcess);
    optional("WKWebsiteDataStoreGetFetchCacheOrigins", api.getFetchCacheOrigins);
    optional("WKWebsiteDataStoreGetFetchCacheSizeForOrigin", api.getFetchCacheSizeForOrigin);

    optional("WKPageCopySessionState", api.pageCopySessionState);
    optional("WKPageRestoreFromSessionState", api.pageRestoreFromSessionState);
    optional("WKSessionStateCopyData", api.sessionStateCopyData);
    optional("WKSessionStateCreateFromData", api.sessionStateCreateFromData);
    optional("WKDataCreate", api.dataCreate);
    optional("WKDataGetBytes", api.dataGetBytes);
    optional("WKDataGetSize", api.dataGetSize);

    g_api = api;
    clearError();
    return true;
}

} // namespace

Api& api()
{
    return g_api;
}

bool loadApi()
{
    if (!g_resolved) {
        // Resolution is retried until the engine answers: the profile is
        // prepared from inside the registry's own start-up, and asking a second
        // time costs a symbol lookup rather than a load.
        g_usable = resolveApi();
        g_resolved = g_usable;
    }
    return g_usable;
}

// --- Errors -----------------------------------------------------------------

void setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    // Said once per distinct failure. Preparing the profile is retried every
    // run-loop cycle until the engine answers, and a line per cycle would bury
    // the reason it is being retried.
    const bool repeated = g_error == message;
    g_error = message;
    if (repeated)
        return;

    // A profile that could not be created otherwise looks like a browser that
    // simply forgets everything between runs.
    std::fprintf(stderr, "harmony: data store: %s\n", g_error.c_str());
}

void clearError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error.clear();
}

std::string currentError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

// --- WK memory and text -----------------------------------------------------

void wkRelease(WKTypeRef object)
{
    if (object && g_usable && g_api.release)
        g_api.release(object);
}

WKStringRef wkString(const std::string& text)
{
    if (!g_usable || !g_api.stringCreateWithUTF8CString)
        return nullptr;
    return g_api.stringCreateWithUTF8CString(text.c_str());
}

std::string fromWKString(WKStringRef string)
{
    if (!string || !g_usable || !g_api.stringGetMaximumUTF8CStringSize)
        return { };

    const size_t capacity = g_api.stringGetMaximumUTF8CStringSize(string);
    if (capacity < 2)
        return { };

    std::string text(capacity, '\0');
    const size_t written = g_api.stringGetUTF8CString(string, &text[0], capacity);
    if (written < 2)
        return { };

    // The count includes the terminator WebKit wrote, which a std::string
    // carries for itself.
    text.resize(written - 1);
    return text;
}

} // namespace harmony::datastore
