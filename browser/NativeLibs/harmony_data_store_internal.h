#ifndef HARMONY_DATA_STORE_INTERNAL_H
#define HARMONY_DATA_STORE_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "harmony_data_store.h"
#include "harmony_paths.h"
#include "harmony_text.h"

// WebKit's C API crosses this boundary as opaque pointers only: every entry
// point the profile needs takes and returns pointers, bools and scalars, so the
// module declares the signatures itself rather than compiling against the
// WebKit headers. That keeps the profile buildable from a checkout whose
// headers are not on the include path.
using WKTypeRef = const void*;
using WKStringRef = const void*;
using WKArrayRef = const void*;
using WKDataRef = const void*;
using WKSecurityOriginRef = const void*;
using WKWebsiteDataStoreRef = const void*;
using WKWebsiteDataStoreConfigurationRef = const void*;
using WKPageConfigurationRef = const void*;
using WKContextConfigurationRef = const void*;
using WKPageRef = const void*;
using WKSessionStateRef = const void*;
using WKHTTPCookieStoreRef = const void*;

using WKVoidCallback = void (*)(void* context);
using WKOriginsCallback = void (*)(WKArrayRef origins, void* context);
using WKSizeCallback = void (*)(uint64_t size, void* context);
using WKSessionStateFilter = bool (*)(WKPageRef, WKStringRef, WKTypeRef, void*);

namespace harmony::datastore {

// Every WebKit entry point the profile resolves out of WebKit2.dll.
struct Api {
    HMODULE module = nullptr;

    WKStringRef (*stringCreateWithUTF8CString)(const char*) = nullptr;
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) = nullptr;
    size_t (*stringGetUTF8CString)(WKStringRef, char*, size_t) = nullptr;
    void (*release)(WKTypeRef) = nullptr;
    WKTypeRef (*retain)(WKTypeRef) = nullptr;

    size_t (*arrayGetSize)(WKArrayRef) = nullptr;
    WKTypeRef (*arrayGetItemAtIndex)(WKArrayRef, size_t) = nullptr;

    WKSecurityOriginRef (*securityOriginCreateFromString)(WKStringRef) = nullptr;
    WKStringRef (*securityOriginCopyToString)(WKSecurityOriginRef) = nullptr;

    WKWebsiteDataStoreConfigurationRef (*dataStoreConfigurationCreate)() = nullptr;
    void (*setNetworkCacheDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setGeneralStorageDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setIndexedDBDatabaseDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setLocalStorageDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setWebSQLDatabaseDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setCacheStorageDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setMediaKeysStorageDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setResourceLoadStatisticsDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setServiceWorkerRegistrationDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setResourceMonitorThrottlerDirectory)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setCookieStorageFile)(WKWebsiteDataStoreConfigurationRef, WKStringRef) = nullptr;
    void (*setPerOriginStorageQuota)(WKWebsiteDataStoreConfigurationRef, uint64_t) = nullptr;
    void (*setOriginQuotaRatio)(WKWebsiteDataStoreConfigurationRef, double) = nullptr;

    WKWebsiteDataStoreRef (*dataStoreCreateWithConfiguration)(WKWebsiteDataStoreConfigurationRef) = nullptr;
    WKWebsiteDataStoreRef (*dataStoreCreateNonPersistent)() = nullptr;
    WKHTTPCookieStoreRef (*dataStoreGetCookieStore)(WKWebsiteDataStoreRef) = nullptr;
    void (*cookieStoreDeleteAllCookies)(WKHTTPCookieStoreRef, void*, WKVoidCallback) = nullptr;

    WKWebsiteDataStoreRef (*pageConfigurationGetWebsiteDataStore)(WKPageConfigurationRef) = nullptr;
    void (*pageConfigurationSetWebsiteDataStore)(WKPageConfigurationRef, WKWebsiteDataStoreRef) = nullptr;
    void (*contextConfigurationSetUsesWebProcessCache)(WKContextConfigurationRef, bool) = nullptr;
    void (*contextConfigurationSetPrewarmsProcesses)(WKContextConfigurationRef, bool) = nullptr;

    void (*removeNetworkCache)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeMemoryCaches)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeAllFetchCaches)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeFetchCacheForOrigin)(WKWebsiteDataStoreRef, WKSecurityOriginRef, void*, WKVoidCallback) = nullptr;
    void (*removeAllServiceWorkerRegistrations)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeAllIndexedDatabases)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeLocalStorage)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*removeITPDataForDomain)(WKWebsiteDataStoreRef, WKStringRef, void*, WKVoidCallback) = nullptr;
    void (*clearStorage)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*statisticsClearInMemoryAndPersistentStore)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*statisticsClearModifiedSinceHours)(WKWebsiteDataStoreRef, unsigned, void*, WKVoidCallback) = nullptr;
    void (*clearPrivateClickMeasurements)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*syncLocalStorage)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*flushNetworkProcessIPC)(WKWebsiteDataStoreRef, void*, WKVoidCallback) = nullptr;
    void (*terminateNetworkProcess)(WKWebsiteDataStoreRef) = nullptr;
    void (*getFetchCacheOrigins)(WKWebsiteDataStoreRef, void*, WKOriginsCallback) = nullptr;
    void (*getFetchCacheSizeForOrigin)(WKWebsiteDataStoreRef, WKStringRef, void*, WKSizeCallback) = nullptr;

    WKTypeRef (*pageCopySessionState)(WKPageRef, void*, WKSessionStateFilter) = nullptr;
    void (*pageRestoreFromSessionState)(WKPageRef, WKTypeRef) = nullptr;
    WKDataRef (*sessionStateCopyData)(WKSessionStateRef) = nullptr;
    WKSessionStateRef (*sessionStateCreateFromData)(WKDataRef) = nullptr;
    WKDataRef (*dataCreate)(const unsigned char*, size_t) = nullptr;
    const unsigned char* (*dataGetBytes)(WKDataRef) = nullptr;
    size_t (*dataGetSize)(WKDataRef) = nullptr;
};

// The one resolved table, valid once loadApi() has answered true.
Api& api();
bool loadApi();

void setError(const std::string& message);
void clearError();
std::string currentError();

// WKRelease that tolerates a null object and an unloaded engine.
void wkRelease(WKTypeRef object);
// A +1 WKStringRef built from UTF-8, or null.
WKStringRef wkString(const std::string& text);
// A WKStringRef's contents as UTF-8. Does not consume the reference.
std::string fromWKString(WKStringRef string);

// --- Profile layout ---------------------------------------------------------

// Every directory and file the profile owns, resolved once at prepare time.
struct Layout {
    std::wstring root;
    std::wstring networkCache;
    std::wstring generalStorage;
    std::wstring legacyIndexedDB;
    std::wstring legacyLocalStorage;
    std::wstring legacyWebSQL;
    std::wstring legacyCacheStorage;
    std::wstring mediaKeys;
    std::wstring resourceLoadStatistics;
    std::wstring serviceWorkerRegistrations;
    std::wstring resourceMonitorThrottler;
    std::wstring cookieFile;
    std::wstring sessionFile;
    std::wstring sessionTempFile;
    std::wstring originIndexFile;
    std::wstring saltFile;
};

const Layout& layout();
// Builds the layout and creates every directory in it. Answers false only when
// the profile root itself could not be created.
bool prepareLayout();
const std::string& profilePathUtf8();

using harmony::text::narrow;
using harmony::text::widen;

// --- Filesystem -------------------------------------------------------------

using harmony::paths::directoryExists;
using harmony::paths::makeDirectories;
using harmony::paths::pathExists;

std::vector<std::wstring> childDirectories(const std::wstring& directory);

// Total bytes and newest write time (Unix seconds) under a subtree.
struct TreeStat {
    double bytes = 0;
    double newest = 0;
    bool exists = false;
};
TreeStat statTree(const std::wstring& path);

// Removes a subtree and answers the bytes it freed. `since` is a Unix-seconds
// cutoff: entries written before it survive, and a directory is removed only
// once it is empty. A cutoff of zero removes the subtree whole.
double removeTree(const std::wstring& path, double since);
double removeFileIfNewer(const std::wstring& path, double since);

double unixNow();
double unixTimeOf(const FILETIME& time);

// The storage subdirectories WebKit writes under one origin's directory, one
// per public data type. An unrecognised type answers with an empty list.
std::vector<std::wstring> storageSubdirectoriesForType(int type);

// --- Background work --------------------------------------------------------
//
// Walking and deleting a profile tree is disk-bound, and the WebKit thread it
// would otherwise run on is the thread pages render from. So it runs here, on
// one worker of the module's own, and reports back through a flag the pump
// reads.

void submitWork(std::function<void()> job);
void stopWorker();

// --- Origins ----------------------------------------------------------------

// "https://example.com" or "https://example.com:8443": the spelling WebKit
// hashes into a storage directory name. Answers an empty string for a URL with
// no usable origin.
std::string originForURL(const std::string& url);
std::string hostForOrigin(const std::string& origin);

// The profile's 8-byte storage salt, read from disk or created the way WebKit
// would have created it.
const std::vector<uint8_t>& storageSalt();
// base64url(SHA-256(origin || salt)), the directory name WebKit gives an origin.
std::string saltedName(const std::string& origin);
// Every directory the profile holds for an origin: the one it is the top origin
// of -- which holds what third parties stored while it was in front -- and the
// partitioned directories it has under other top-level origins. This is what a
// removal deletes.
std::vector<std::wstring> originStorageDirectories(const std::string& origin);

// --- Origin ledger ----------------------------------------------------------
//
// Which origins this browser has been to. WebKit can name the origins holding a
// DOM cache and nothing else, so without this the site list would be a list of
// salted directory names. The ledger is what lets it be a list of sites.

struct LedgerEntry {
    std::string origin;
    double firstSeen = 0;
    double lastSeen = 0;
};

void ledgerLoad();
void ledgerSave();
void ledgerNote(const std::string& origin);
void ledgerForget(const std::string& origin);
// Forgets every origin visited at or after `since`, which is the set a
// range-scoped clear has just deleted the data of.
void ledgerForgetSince(double since);
void ledgerClear();
std::vector<LedgerEntry> ledgerSnapshot();

// --- Stores -----------------------------------------------------------------

// Engine thread. Services queued requests, drives the WebKit-side steps of the
// removal in flight, keeps the origin ledger fed from the tab list, and writes
// the session once a change to it has settled. Registered as the tab registry's
// cycle hook when the profile is prepared.
void pump();

WKWebsiteDataStoreRef persistentStore();
// Creates the ephemeral store if no private tab holds one yet.
WKWebsiteDataStoreRef ephemeralStore();
void releaseEphemeralStoreIfUnused();

// Engine thread. The next tab the registry creates browses privately.
void markNextTabPrivate();
void forgetTab(int tabId);
bool tabIsPrivate(int tabId);
int privateTabCount();

// --- Records ----------------------------------------------------------------

struct Record {
    std::string origin;
    std::string host;
    double bytes = 0;
    double lastUsed = 0;
    int types = 0;
};

// Engine thread. Called by the pump when a record request is outstanding.
void recordsPump();
// Engine thread. Called when a removal finishes, so the published set stops
// naming data that is gone.
void recordsInvalidate();

// --- Removal ----------------------------------------------------------------

// Engine thread. Drives the removal in flight one step per call.
void removalPump();

// --- Session ----------------------------------------------------------------

void sessionLoad();
// Engine thread. Notices tab-list changes, feeds the ledger, restores adopted
// entries, and writes the session once a change has settled.
void sessionPump();
// Engine or frame thread. Captures what it can reach and writes the session.
void sessionShutdown();

} // namespace harmony::datastore

#endif // HARMONY_DATA_STORE_INTERNAL_H
