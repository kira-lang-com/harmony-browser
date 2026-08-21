#include "harmony_data_store_internal.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>

// What the profile holds, per site and per type.
//
// WebKit can name the origins holding a DOM cache and nothing else, so the site
// list is built from three sources that agree on one name for a site: the
// ledger of origins this browser has visited, the origins WebKit reports for
// its fetch caches, and the directories on disk that the salted name of an
// origin resolves to.
//
// The scan itself is disk-bound and runs on the module's worker. Only the step
// that asks WebKit runs on the engine thread, and only for as long as WebKit's
// own callback takes.

namespace harmony::datastore {

namespace {

enum class Phase {
    Idle,
    // Waiting on WKWebsiteDataStoreGetFetchCacheOrigins.
    AskingEngine,
    // Waiting on the worker's walk of the profile tree.
    Scanning,
};

Phase g_phase = Phase::Idle;

// Engine thread. The origins WebKit answered with, and the sizes it reported
// for them, filled in before the disk scan starts.
std::vector<std::string> g_domCacheOrigins;
std::vector<double> g_domCacheSizes;
size_t g_domCacheAt = 0;

std::atomic<bool> g_requested { false };
std::atomic<bool> g_pending { false };
std::atomic<int> g_generation { 0 };
std::atomic<bool> g_scanFinished { false };

std::mutex g_publishedMutex;
std::vector<Record> g_published;
double g_publishedTotal = 0.0;
double g_typeSizes[8] { };

// The frame thread's latched copy: taken once per hb_data_store_record_count
// and read by every accessor after it, so a set published mid-frame cannot make
// one row's origin belong to another row's size.
thread_local std::vector<Record> t_latched;
thread_local double t_latchedTotal = 0.0;
thread_local std::string t_text;

int typeIndex(int type)
{
    switch (type) {
    case HB_DATA_TYPE_COOKIES: return 0;
    case HB_DATA_TYPE_CACHE: return 1;
    case HB_DATA_TYPE_LOCAL_STORAGE: return 2;
    case HB_DATA_TYPE_INDEXED_DB: return 3;
    case HB_DATA_TYPE_SERVICE_WORKERS: return 4;
    case HB_DATA_TYPE_DOM_CACHE: return 5;
    case HB_DATA_TYPE_MEDIA_KEYS: return 6;
    case HB_DATA_TYPE_TRACKING: return 7;
    default: return -1;
    }
}

// The cookie jar is one SQLite database, and SQLite's journal and write-ahead
// log are as much of the jar as the database file is.
double cookieJarBytes()
{
    const std::wstring& jar = layout().cookieFile;
    double bytes = statTree(jar).bytes;
    bytes += statTree(jar + L"-wal").bytes;
    bytes += statTree(jar + L"-shm").bytes;
    bytes += statTree(jar + L"-journal").bytes;
    return bytes;
}

constexpr int kStorageTypes[] = {
    HB_DATA_TYPE_LOCAL_STORAGE,
    HB_DATA_TYPE_INDEXED_DB,
    HB_DATA_TYPE_DOM_CACHE,
    HB_DATA_TYPE_SERVICE_WORKERS,
};

// Every client directory in the profile, keyed by the salted name of the origin
// that owns it.
//
// The tree is walked once and looked up per origin from here, rather than being
// re-listed per origin: a profile with a few hundred sites in it has a few
// hundred top-level directories, and asking each site to find itself among them
// would make the scan cost the square of the profile's size.
using ClientDirectories = std::unordered_map<std::wstring, std::vector<std::wstring>>;

ClientDirectories readClientDirectories()
{
    ClientDirectories byName;
    for (const std::wstring& top : childDirectories(layout().generalStorage)) {
        for (const std::wstring& client : childDirectories(top)) {
            const auto slash = client.find_last_of(L'\\');
            if (slash == std::wstring::npos)
                continue;
            byName[client.substr(slash + 1)].push_back(client);
        }
    }
    return byName;
}

// One origin's own storage, first-party and partitioned. The directory the
// origin is the TOP of is not measured here: it also holds what other sites
// stored while this one was in front, and that is not this site's weight.
void measureOrigin(Record& record, const ClientDirectories& byName)
{
    const std::string encoded = saltedName(record.origin);
    if (encoded.empty())
        return;

    const auto directories = byName.find(widen(encoded));
    if (directories == byName.end())
        return;

    for (const std::wstring& directory : directories->second) {
        for (int type : kStorageTypes) {
            for (const std::wstring& child : storageSubdirectoriesForType(type)) {
                const TreeStat stat = statTree(directory + L'\\' + child);
                if (!stat.exists || stat.bytes <= 0.0)
                    continue;
                record.types |= type;
                record.bytes += stat.bytes;
                if (stat.newest > record.lastUsed)
                    record.lastUsed = stat.newest;
            }
        }
    }
}

// The profile-wide figure behind each checkbox in the clear-data panel.
void measureTypes(double sizes[8], const ClientDirectories& byName)
{
    const Layout& paths = layout();
    sizes[typeIndex(HB_DATA_TYPE_COOKIES)] = cookieJarBytes();
    sizes[typeIndex(HB_DATA_TYPE_CACHE)] = statTree(paths.networkCache).bytes;
    sizes[typeIndex(HB_DATA_TYPE_MEDIA_KEYS)] = statTree(paths.mediaKeys).bytes;
    sizes[typeIndex(HB_DATA_TYPE_TRACKING)] = statTree(paths.resourceLoadStatistics).bytes;

    double local = statTree(paths.legacyLocalStorage).bytes + statTree(paths.legacyWebSQL).bytes;
    double indexed = statTree(paths.legacyIndexedDB).bytes;
    double domCache = statTree(paths.legacyCacheStorage).bytes;
    double workers = statTree(paths.serviceWorkerRegistrations).bytes;

    for (const auto& entry : byName) {
        for (const std::wstring& client : entry.second) {
            local += statTree(client + L"\\LocalStorage").bytes;
            local += statTree(client + L"\\SessionStorage").bytes;
            indexed += statTree(client + L"\\IndexedDB").bytes;
            domCache += statTree(client + L"\\CacheStorage").bytes;
            domCache += statTree(client + L"\\BackgroundFetchStorage").bytes;
            workers += statTree(client + L"\\ServiceWorkers").bytes;
        }
    }

    sizes[typeIndex(HB_DATA_TYPE_LOCAL_STORAGE)] = local;
    sizes[typeIndex(HB_DATA_TYPE_INDEXED_DB)] = indexed;
    sizes[typeIndex(HB_DATA_TYPE_DOM_CACHE)] = domCache;
    sizes[typeIndex(HB_DATA_TYPE_SERVICE_WORKERS)] = workers;
}

// Worker thread. Everything from here to publication is disk and arithmetic.
void scan(std::vector<std::string> domCacheOrigins, std::vector<double> domCacheSizes)
{
    const ClientDirectories byName = readClientDirectories();
    std::vector<Record> records;

    for (const LedgerEntry& entry : ledgerSnapshot()) {
        Record record;
        record.origin = entry.origin;
        record.host = hostForOrigin(entry.origin);
        record.lastUsed = entry.lastSeen;
        measureOrigin(record, byName);
        records.push_back(std::move(record));
    }

    for (size_t at = 0; at < domCacheOrigins.size(); ++at) {
        const std::string& origin = domCacheOrigins[at];
        const double bytes = at < domCacheSizes.size() ? domCacheSizes[at] : 0.0;

        auto existing = std::find_if(records.begin(), records.end(), [&origin](const Record& record) {
            return record.origin == origin;
        });
        if (existing == records.end()) {
            Record record;
            record.origin = origin;
            record.host = hostForOrigin(origin);
            measureOrigin(record, byName);
            records.push_back(std::move(record));
            existing = records.end() - 1;
        }
        existing->types |= HB_DATA_TYPE_DOM_CACHE;
        // WebKit's figure is the authority for a DOM cache: the directory it
        // lives in is shared with the rest of the origin's storage.
        if (bytes > 0.0)
            existing->bytes += bytes;
    }

    // A site the browser has been to but that stored nothing is not a site with
    // data to clear, and listing it would make the panel a history instead.
    records.erase(
        std::remove_if(records.begin(), records.end(), [](const Record& record) {
            return record.types == 0;
        }),
        records.end()
    );

    std::sort(records.begin(), records.end(), [](const Record& left, const Record& right) {
        if (left.bytes != right.bytes)
            return left.bytes > right.bytes;
        return left.host < right.host;
    });

    double total = 0.0;
    for (const Record& record : records)
        total += record.bytes;

    double sizes[8] { };
    measureTypes(sizes, byName);

    {
        std::lock_guard<std::mutex> lock(g_publishedMutex);
        g_published = std::move(records);
        g_publishedTotal = total;
        for (int at = 0; at < 8; ++at)
            g_typeSizes[at] = sizes[at];
    }

    g_generation.fetch_add(1);
    g_scanFinished.store(true);
}

void startDiskScan()
{
    g_phase = Phase::Scanning;
    g_scanFinished.store(false);

    std::vector<std::string> origins = std::move(g_domCacheOrigins);
    std::vector<double> sizes = std::move(g_domCacheSizes);
    g_domCacheOrigins.clear();
    g_domCacheSizes.clear();
    g_domCacheAt = 0;

    submitWork([origins = std::move(origins), sizes = std::move(sizes)]() mutable {
        scan(std::move(origins), std::move(sizes));
    });
}

void askNextDomCacheSize();

void domCacheSizeAnswered(uint64_t size, void*)
{
    g_domCacheSizes.push_back(static_cast<double>(size));
    ++g_domCacheAt;
    askNextDomCacheSize();
}

void askNextDomCacheSize()
{
    Api& wk = api();
    while (g_domCacheAt < g_domCacheOrigins.size()) {
        if (!wk.getFetchCacheSizeForOrigin) {
            g_domCacheSizes.push_back(0.0);
            ++g_domCacheAt;
            continue;
        }

        WKStringRef origin = wkString(g_domCacheOrigins[g_domCacheAt]);
        if (!origin) {
            g_domCacheSizes.push_back(0.0);
            ++g_domCacheAt;
            continue;
        }

        wk.getFetchCacheSizeForOrigin(persistentStore(), origin, nullptr, domCacheSizeAnswered);
        wkRelease(origin);
        return;
    }
    startDiskScan();
}

void domCacheOriginsAnswered(WKArrayRef origins, void*)
{
    Api& wk = api();
    g_domCacheOrigins.clear();
    g_domCacheSizes.clear();
    g_domCacheAt = 0;

    if (origins && wk.arrayGetSize && wk.arrayGetItemAtIndex && wk.securityOriginCopyToString) {
        const size_t count = wk.arrayGetSize(origins);
        for (size_t at = 0; at < count; ++at) {
            WKSecurityOriginRef origin = wk.arrayGetItemAtIndex(origins, at);
            if (!origin)
                continue;
            WKStringRef text = wk.securityOriginCopyToString(origin);
            std::string spelling = fromWKString(text);
            wkRelease(text);
            if (!spelling.empty())
                g_domCacheOrigins.push_back(std::move(spelling));
        }
    }

    askNextDomCacheSize();
}

} // namespace

void recordsPump()
{
    switch (g_phase) {
    case Phase::Idle:
        if (!g_requested.exchange(false))
            return;
        g_pending.store(true);
        if (api().getFetchCacheOrigins && persistentStore()) {
            g_phase = Phase::AskingEngine;
            api().getFetchCacheOrigins(persistentStore(), nullptr, domCacheOriginsAnswered);
            return;
        }
        startDiskScan();
        return;

    case Phase::AskingEngine:
        // WebKit answers on this thread's run loop; nothing to drive here.
        return;

    case Phase::Scanning:
        if (!g_scanFinished.exchange(false))
            return;
        g_phase = Phase::Idle;
        g_pending.store(false);
        return;
    }
}

void recordsInvalidate()
{
    // What is on screen names data that has just been deleted, so the set is
    // rebuilt rather than left to be corrected by the next thing the user does.
    g_requested.store(true);
}

} // namespace harmony::datastore

using namespace harmony::datastore;

extern "C" void hb_data_store_request_records(void)
{
    g_requested.store(true);
    g_pending.store(true);
}

extern "C" int hb_data_store_records_pending(void)
{
    return g_pending.load() ? 1 : 0;
}

extern "C" int hb_data_store_records_generation(void)
{
    return g_generation.load();
}

extern "C" int hb_data_store_record_count(void)
{
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    t_latched = g_published;
    t_latchedTotal = g_publishedTotal;
    return static_cast<int>(t_latched.size());
}

extern "C" const char* hb_data_store_record_origin(int index)
{
    if (index < 0 || index >= static_cast<int>(t_latched.size()))
        return "";
    t_text = t_latched[static_cast<size_t>(index)].origin;
    return t_text.c_str();
}

extern "C" const char* hb_data_store_record_host(int index)
{
    if (index < 0 || index >= static_cast<int>(t_latched.size()))
        return "";
    t_text = t_latched[static_cast<size_t>(index)].host;
    return t_text.c_str();
}

extern "C" double hb_data_store_record_size(int index)
{
    if (index < 0 || index >= static_cast<int>(t_latched.size()))
        return 0.0;
    return t_latched[static_cast<size_t>(index)].bytes;
}

extern "C" int hb_data_store_record_types(int index)
{
    if (index < 0 || index >= static_cast<int>(t_latched.size()))
        return 0;
    return t_latched[static_cast<size_t>(index)].types;
}

extern "C" double hb_data_store_record_last_used(int index)
{
    if (index < 0 || index >= static_cast<int>(t_latched.size()))
        return 0.0;
    return t_latched[static_cast<size_t>(index)].lastUsed;
}

extern "C" double hb_data_store_records_total_size(void)
{
    return t_latchedTotal;
}

extern "C" double hb_data_store_now(void)
{
    return unixNow();
}

extern "C" double hb_data_store_type_size(int type)
{
    const int at = typeIndex(type);
    if (at < 0)
        return 0.0;
    std::lock_guard<std::mutex> lock(g_publishedMutex);
    return g_typeSizes[at];
}
