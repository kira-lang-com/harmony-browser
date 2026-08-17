#include "harmony_data_store_internal.h"

#include <atomic>
#include <mutex>

// Deleting what the profile holds.
//
// WebKit's C API removes a data type for all time and never for a range, so a
// range-scoped clear cannot be asked of it: the profile deletes its own files
// under a cutoff instead. That is only safe with the network process stopped,
// because that process is where this port keeps storage open, so a removal that
// touches disk stops it first and lets the next load bring it back.
//
// A removal therefore runs as a sequence rather than a call: measure, flush,
// ask WebKit for what it can do exactly, sweep the rest off disk, measure
// again. Each step hands over to the next through the pump.

namespace harmony::datastore {

namespace {

enum class Phase {
    Idle,
    Measuring,
    Flushing,
    Engine,
    Sweeping,
};

struct Request {
    // Empty for a clear; the origin for a per-site removal.
    std::string origin;
    int types = 0;
    int range = HB_DATA_RANGE_ALL_TIME;
    // Unix seconds, decided once when the request starts. Zero means all time.
    // Deciding it per step would let the sweep and the ledger disagree about
    // where the range begins.
    double cutoff = 0.0;
};

Phase g_phase = Phase::Idle;
Request g_request;
int g_outstanding = 0;

// How long a step that waits on WebKit may wait. A completion handler that never
// arrives -- a network process that died mid-removal is one way -- would
// otherwise leave the panel saying "Clearing" for the rest of the run.
constexpr double kEngineStepSeconds = 20.0;
double g_stepDeadline = 0.0;

// Which step a completion handler belongs to. A handler that arrives after its
// step gave up would otherwise count against the step that followed it.
intptr_t g_stepToken = 0;

std::mutex g_queueMutex;
std::vector<Request> g_queue;

std::atomic<bool> g_pending { false };
std::atomic<int> g_generation { 0 };
std::atomic<bool> g_workFinished { false };

// Measured by the worker on either side of the removal, so the figure the panel
// shows is what the profile actually lost.
double g_bytesBefore = 0.0;
std::atomic<double> g_freed { 0.0 };

double rangeSeconds(int range)
{
    switch (range) {
    case HB_DATA_RANGE_LAST_HOUR: return 3600.0;
    case HB_DATA_RANGE_LAST_DAY: return 24.0 * 3600.0;
    case HB_DATA_RANGE_LAST_WEEK: return 7.0 * 24.0 * 3600.0;
    case HB_DATA_RANGE_LAST_FOUR_WEEKS: return 28.0 * 24.0 * 3600.0;
    default: return 0.0;
    }
}

unsigned rangeHours(int range)
{
    switch (range) {
    case HB_DATA_RANGE_LAST_HOUR: return 1;
    case HB_DATA_RANGE_LAST_DAY: return 24;
    case HB_DATA_RANGE_LAST_WEEK: return 24 * 7;
    case HB_DATA_RANGE_LAST_FOUR_WEEKS: return 24 * 28;
    default: return 0;
    }
}

double cutoffFor(const Request& request)
{
    const double span = rangeSeconds(request.range);
    if (span <= 0.0)
        return 0.0;
    const double cutoff = unixNow() - span;
    return cutoff > 0.0 ? cutoff : 0.0;
}

// Every tree a request may take bytes out of. Measuring the same list before
// and after is what makes the freed figure honest for both kinds of request.
std::vector<std::wstring> affectedTrees(const Request& request)
{
    if (!request.origin.empty())
        return originStorageDirectories(request.origin);

    const Layout& paths = layout();
    std::vector<std::wstring> trees;
    if (request.types & HB_DATA_TYPE_CACHE)
        trees.push_back(paths.networkCache);
    if (request.types & HB_DATA_TYPE_MEDIA_KEYS)
        trees.push_back(paths.mediaKeys);
    if (request.types & HB_DATA_TYPE_TRACKING)
        trees.push_back(paths.resourceLoadStatistics);
    if (request.types & HB_DATA_TYPE_COOKIES) {
        trees.push_back(paths.cookieFile);
        trees.push_back(paths.cookieFile + L"-wal");
        trees.push_back(paths.cookieFile + L"-shm");
        trees.push_back(paths.cookieFile + L"-journal");
    }

    constexpr int kStorageTypes[] = {
        HB_DATA_TYPE_LOCAL_STORAGE,
        HB_DATA_TYPE_INDEXED_DB,
        HB_DATA_TYPE_DOM_CACHE,
        HB_DATA_TYPE_SERVICE_WORKERS,
    };
    for (int type : kStorageTypes) {
        if (!(request.types & type))
            continue;
        for (const std::wstring& top : childDirectories(paths.generalStorage)) {
            for (const std::wstring& client : childDirectories(top)) {
                for (const std::wstring& child : storageSubdirectoriesForType(type))
                    trees.push_back(client + L'\\' + child);
            }
        }
    }

    if (request.types & HB_DATA_TYPE_LOCAL_STORAGE) {
        trees.push_back(paths.legacyLocalStorage);
        trees.push_back(paths.legacyWebSQL);
    }
    if (request.types & HB_DATA_TYPE_INDEXED_DB)
        trees.push_back(paths.legacyIndexedDB);
    if (request.types & HB_DATA_TYPE_DOM_CACHE)
        trees.push_back(paths.legacyCacheStorage);
    if (request.types & HB_DATA_TYPE_SERVICE_WORKERS)
        trees.push_back(paths.serviceWorkerRegistrations);
    return trees;
}

double measureTrees(const std::vector<std::wstring>& trees)
{
    double bytes = 0.0;
    for (const std::wstring& tree : trees)
        bytes += statTree(tree).bytes;
    return bytes;
}

void* stepContext()
{
    return reinterpret_cast<void*>(g_stepToken);
}

void finishStep(void* context)
{
    if (reinterpret_cast<intptr_t>(context) != g_stepToken)
        return;
    if (g_outstanding > 0)
        --g_outstanding;
}

// Opens a step: everything asked of WebKit from here on is counted against this
// token, and answers carrying an older one are ignored.
void beginStep()
{
    g_outstanding = 0;
    ++g_stepToken;
    g_stepDeadline = unixNow() + kEngineStepSeconds;
}

void askEngine(void (*call)(WKWebsiteDataStoreRef, void*, WKVoidCallback))
{
    if (!call || !persistentStore())
        return;
    ++g_outstanding;
    call(persistentStore(), stepContext(), finishStep);
}

void startMeasuring()
{
    g_phase = Phase::Measuring;
    g_workFinished.store(false);
    g_request.cutoff = cutoffFor(g_request);
    g_freed.store(0.0);

    const Request request = g_request;
    submitWork([request]() {
        g_bytesBefore = measureTrees(affectedTrees(request));
        g_workFinished.store(true);
    });
}

void startFlushing()
{
    g_phase = Phase::Flushing;
    beginStep();

    // Local storage lives in memory until it is synced, and the network process
    // may be holding writes it has not sent. Both have to reach disk or the
    // sweep deletes a file that is about to be written again.
    Api& wk = api();
    askEngine(wk.syncLocalStorage);
    askEngine(wk.flushNetworkProcessIPC);
}

// The removals WebKit does better than a file deletion could: they take the
// live processes with them rather than only the bytes.
void startEngineRemovals()
{
    g_phase = Phase::Engine;
    beginStep();

    Api& wk = api();
    const bool allTime = g_request.range == HB_DATA_RANGE_ALL_TIME;

    if (!g_request.origin.empty()) {
        if (wk.removeFetchCacheForOrigin && wk.securityOriginCreateFromString) {
            WKStringRef text = wkString(g_request.origin);
            WKSecurityOriginRef origin = text ? wk.securityOriginCreateFromString(text) : nullptr;
            wkRelease(text);
            if (origin) {
                ++g_outstanding;
                wk.removeFetchCacheForOrigin(persistentStore(), origin, stepContext(), finishStep);
                wkRelease(origin);
            }
        }
        if (wk.removeITPDataForDomain) {
            WKStringRef host = wkString(hostForOrigin(g_request.origin));
            if (host) {
                ++g_outstanding;
                wk.removeITPDataForDomain(persistentStore(), host, stepContext(), finishStep);
                wkRelease(host);
            }
        }
        return;
    }

    // The jar is one database with no per-cookie age this API can read, so a
    // range cannot be applied to it: asked for cookies, it clears them.
    if ((g_request.types & HB_DATA_TYPE_COOKIES) && wk.dataStoreGetCookieStore && wk.cookieStoreDeleteAllCookies) {
        if (WKHTTPCookieStoreRef jar = wk.dataStoreGetCookieStore(persistentStore())) {
            ++g_outstanding;
            wk.cookieStoreDeleteAllCookies(jar, stepContext(), finishStep);
        }
    }

    if (g_request.types & HB_DATA_TYPE_TRACKING) {
        if (allTime) {
            askEngine(wk.statisticsClearInMemoryAndPersistentStore);
        } else if (wk.statisticsClearModifiedSinceHours) {
            ++g_outstanding;
            wk.statisticsClearModifiedSinceHours(persistentStore(), rangeHours(g_request.range), stepContext(), finishStep);
        }
        askEngine(wk.clearPrivateClickMeasurements);
    }

    if (!allTime)
        return;

    if (g_request.types & HB_DATA_TYPE_CACHE) {
        askEngine(wk.removeNetworkCache);
        askEngine(wk.removeMemoryCaches);
    }
    if (g_request.types & HB_DATA_TYPE_LOCAL_STORAGE)
        askEngine(wk.removeLocalStorage);
    if (g_request.types & HB_DATA_TYPE_INDEXED_DB)
        askEngine(wk.removeAllIndexedDatabases);
    if (g_request.types & HB_DATA_TYPE_SERVICE_WORKERS)
        askEngine(wk.removeAllServiceWorkerRegistrations);
    if (g_request.types & HB_DATA_TYPE_DOM_CACHE)
        askEngine(wk.removeAllFetchCaches);
}

void startSweeping()
{
    g_phase = Phase::Sweeping;
    g_workFinished.store(false);

    // This port keeps storage open in the network process, so the files below
    // are only the profile's to delete once that process has let go of them.
    // The next load starts it again.
    if (api().terminateNetworkProcess && persistentStore())
        api().terminateNetworkProcess(persistentStore());

    const Request request = g_request;
    submitWork([request]() {
        const std::vector<std::wstring> trees = affectedTrees(request);
        for (const std::wstring& tree : trees)
            removeTree(tree, request.cutoff);

        const double after = measureTrees(trees);
        const double freed = g_bytesBefore - after;
        g_freed.store(freed > 0.0 ? freed : 0.0);
        g_workFinished.store(true);
    });
}

void finishRequest()
{
    if (!g_request.origin.empty()) {
        ledgerForget(g_request.origin);
    } else if (g_request.types & (HB_DATA_TYPE_LOCAL_STORAGE | HB_DATA_TYPE_INDEXED_DB
        | HB_DATA_TYPE_DOM_CACHE | HB_DATA_TYPE_SERVICE_WORKERS | HB_DATA_TYPE_COOKIES)) {
        // The site list names sites the profile holds data for. A site whose
        // data is gone has no place on it.
        if (g_request.cutoff <= 0.0)
            ledgerClear();
        else
            ledgerForgetSince(g_request.cutoff);
    }
    ledgerSave();

    g_phase = Phase::Idle;
    g_request = Request { };
    g_generation.fetch_add(1);
    recordsInvalidate();

    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty())
        g_pending.store(false);
}

bool takeNextRequest()
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty())
        return false;
    g_request = std::move(g_queue.front());
    g_queue.erase(g_queue.begin());
    return true;
}

void enqueue(Request&& request)
{
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(std::move(request));
    }
    g_pending.store(true);
}

} // namespace

void removalPump()
{
    switch (g_phase) {
    case Phase::Idle:
        if (!takeNextRequest())
            return;
        startMeasuring();
        return;

    case Phase::Measuring:
        if (!g_workFinished.exchange(false))
            return;
        startFlushing();
        // A build exporting neither flush answers immediately, and waiting a
        // whole cycle for that would be a cycle of nothing.
        if (g_outstanding == 0)
            startEngineRemovals();
        return;

    case Phase::Flushing:
        if (g_outstanding > 0 && unixNow() < g_stepDeadline)
            return;
        startEngineRemovals();
        return;

    case Phase::Engine:
        if (g_outstanding > 0 && unixNow() < g_stepDeadline)
            return;
        startSweeping();
        return;

    case Phase::Sweeping:
        if (!g_workFinished.exchange(false))
            return;
        finishRequest();
        return;
    }
}

} // namespace harmony::datastore

using namespace harmony::datastore;

extern "C" void hb_data_store_remove_origin(const char* origin)
{
    if (!origin || !*origin)
        return;

    Request request;
    request.origin = origin;
    request.types = HB_DATA_TYPE_ALL;
    request.range = HB_DATA_RANGE_ALL_TIME;
    enqueue(std::move(request));
}

extern "C" void hb_data_store_clear(int types, int range)
{
    const int wanted = types & HB_DATA_TYPE_ALL;
    if (!wanted)
        return;

    Request request;
    request.types = wanted;
    request.range = range < HB_DATA_RANGE_LAST_HOUR || range > HB_DATA_RANGE_ALL_TIME
        ? HB_DATA_RANGE_ALL_TIME
        : range;
    enqueue(std::move(request));
}

extern "C" int hb_data_store_clear_pending(void)
{
    return g_pending.load() ? 1 : 0;
}

extern "C" int hb_data_store_clear_generation(void)
{
    return g_generation.load();
}

extern "C" double hb_data_store_cleared_bytes(void)
{
    return g_freed.load();
}
