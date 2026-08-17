#include "harmony_data_store_internal.h"

#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>

// Where the profile lives on disk, and everything the profile does to disk.
//
// The layout is fixed at prepare time and never recomputed, because a removal
// is a deletion by path: a layout that could answer differently between the
// scan and the sweep would be a layout that deletes the wrong tree.

namespace harmony::datastore {

namespace {

Layout g_layout;
bool g_layoutReady = false;
std::string g_profilePathUtf8;

std::wstring environmentVariable(const wchar_t* name)
{
    wchar_t buffer[32768] { };
    const DWORD length = GetEnvironmentVariableW(name, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer))
        return { };
    return std::wstring(buffer, length);
}

// Where Windows puts state a program keeps for one machine rather than one that
// roams with the user. A browser cache has no business roaming.
std::wstring localAppDataDirectory()
{
    if (std::wstring local = environmentVariable(L"LOCALAPPDATA"); !local.empty())
        return local;
    if (std::wstring profile = environmentVariable(L"USERPROFILE"); !profile.empty())
        return profile + L"\\AppData\\Local";
    return { };
}

std::wstring join(const std::wstring& directory, const wchar_t* child)
{
    if (directory.empty())
        return child;
    if (directory.back() == L'\\' || directory.back() == L'/')
        return directory + child;
    return directory + L'\\' + child;
}

bool isDotEntry(const wchar_t* name)
{
    if (name[0] != L'.')
        return false;
    return name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0');
}

// --- The worker -------------------------------------------------------------

std::mutex g_workMutex;
std::condition_variable g_workSignal;
std::deque<std::function<void()>> g_work;
std::thread g_worker;
bool g_workerRunning = false;
bool g_workerStopping = false;

void workerMain()
{
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock<std::mutex> lock(g_workMutex);
            g_workSignal.wait(lock, [] { return g_workerStopping || !g_work.empty(); });
            if (g_workerStopping && g_work.empty())
                return;
            job = std::move(g_work.front());
            g_work.pop_front();
        }
        job();
    }
}

} // namespace

// --- Layout -----------------------------------------------------------------

const Layout& layout()
{
    return g_layout;
}

const std::string& profilePathUtf8()
{
    return g_profilePathUtf8;
}

bool prepareLayout()
{
    if (g_layoutReady)
        return true;

    std::wstring base = localAppDataDirectory();
    if (base.empty()) {
        // A machine whose known folder cannot be read still has a temporary
        // directory, and a profile there is better than none: the browser keeps
        // a login for the length of the session rather than for none of it.
        wchar_t buffer[MAX_PATH + 1] { };
        const DWORD length = GetTempPathW(MAX_PATH, buffer);
        if (!length) {
            setError("neither %LOCALAPPDATA% nor a temporary directory could be resolved");
            return false;
        }
        base.assign(buffer, length);
    }

    Layout resolved;
    resolved.root = join(base, L"HarmonyBrowser");
    resolved.networkCache = join(resolved.root, L"NetworkCache");
    resolved.generalStorage = join(resolved.root, L"Storage");
    resolved.legacyIndexedDB = join(resolved.root, L"IndexedDB");
    resolved.legacyLocalStorage = join(resolved.root, L"LocalStorage");
    resolved.legacyWebSQL = join(resolved.root, L"WebSQL");
    resolved.legacyCacheStorage = join(resolved.root, L"CacheStorage");
    resolved.mediaKeys = join(resolved.root, L"MediaKeys");
    resolved.resourceLoadStatistics = join(resolved.root, L"ResourceLoadStatistics");
    resolved.serviceWorkerRegistrations = join(resolved.root, L"ServiceWorkers");
    resolved.resourceMonitorThrottler = join(resolved.root, L"ResourceMonitor");
    resolved.cookieFile = join(resolved.root, L"Cookies.db");
    resolved.sessionFile = join(resolved.root, L"Session.hbsession");
    resolved.sessionTempFile = join(resolved.root, L"Session.hbsession.new");
    resolved.originIndexFile = join(resolved.root, L"Origins.txt");
    resolved.saltFile = join(resolved.generalStorage, L"salt");

    if (!makeDirectories(resolved.root)) {
        setError("the profile directory " + narrow(resolved.root) + " could not be created");
        return false;
    }

    // Every other directory is WebKit's to fill. Creating them up front means a
    // scan of an untouched profile reads empty directories rather than failing
    // to find them, and it is the only moment the layout is written to disk.
    makeDirectories(resolved.networkCache);
    makeDirectories(resolved.generalStorage);
    makeDirectories(resolved.legacyIndexedDB);
    makeDirectories(resolved.legacyLocalStorage);
    makeDirectories(resolved.legacyWebSQL);
    makeDirectories(resolved.legacyCacheStorage);
    makeDirectories(resolved.mediaKeys);
    makeDirectories(resolved.resourceLoadStatistics);
    makeDirectories(resolved.serviceWorkerRegistrations);
    makeDirectories(resolved.resourceMonitorThrottler);

    g_layout = std::move(resolved);
    g_profilePathUtf8 = narrow(g_layout.root);
    g_layoutReady = true;
    return true;
}

// --- Filesystem -------------------------------------------------------------

std::vector<std::wstring> childDirectories(const std::wstring& directory)
{
    std::vector<std::wstring> children;
    if (directory.empty())
        return children;

    WIN32_FIND_DATAW entry { };
    HANDLE find = FindFirstFileW(join(directory, L"*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return children;

    do {
        if (isDotEntry(entry.cFileName))
            continue;
        if (!(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            continue;
        children.push_back(join(directory, entry.cFileName));
    } while (FindNextFileW(find, &entry));

    FindClose(find);
    return children;
}

double unixTimeOf(const FILETIME& time)
{
    ULARGE_INTEGER value;
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    if (!value.QuadPart)
        return 0.0;

    // FILETIME counts 100-nanosecond intervals from 1601-01-01; the Unix epoch
    // is 11644473600 seconds later.
    constexpr double kHundredNanosecondsPerSecond = 10000000.0;
    constexpr double kEpochDelta = 11644473600.0;
    return static_cast<double>(value.QuadPart) / kHundredNanosecondsPerSecond - kEpochDelta;
}

double unixNow()
{
    FILETIME now { };
    GetSystemTimeAsFileTime(&now);
    return unixTimeOf(now);
}

TreeStat statTree(const std::wstring& path)
{
    TreeStat stat;
    if (path.empty())
        return stat;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return stat;

    stat.exists = true;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA data { };
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            ULARGE_INTEGER size;
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            stat.bytes = static_cast<double>(size.QuadPart);
            stat.newest = unixTimeOf(data.ftLastWriteTime);
        }
        return stat;
    }

    WIN32_FIND_DATAW entry { };
    HANDLE find = FindFirstFileW(join(path, L"*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE)
        return stat;

    do {
        if (isDotEntry(entry.cFileName))
            continue;

        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            const TreeStat child = statTree(join(path, entry.cFileName));
            stat.bytes += child.bytes;
            if (child.newest > stat.newest)
                stat.newest = child.newest;
            continue;
        }

        ULARGE_INTEGER size;
        size.LowPart = entry.nFileSizeLow;
        size.HighPart = entry.nFileSizeHigh;
        stat.bytes += static_cast<double>(size.QuadPart);

        const double written = unixTimeOf(entry.ftLastWriteTime);
        if (written > stat.newest)
            stat.newest = written;
    } while (FindNextFileW(find, &entry));

    FindClose(find);
    return stat;
}

double removeFileIfNewer(const std::wstring& path, double since)
{
    WIN32_FILE_ATTRIBUTE_DATA data { };
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        return 0.0;
    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        return 0.0;
    if (since > 0.0 && unixTimeOf(data.ftLastWriteTime) < since)
        return 0.0;

    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;

    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
    if (!DeleteFileW(path.c_str()))
        return 0.0;
    return static_cast<double>(size.QuadPart);
}

double removeTree(const std::wstring& path, double since)
{
    if (path.empty())
        return 0.0;

    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return 0.0;
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY))
        return removeFileIfNewer(path, since);

    double freed = 0.0;
    bool emptied = true;

    WIN32_FIND_DATAW entry { };
    HANDLE find = FindFirstFileW(join(path, L"*").c_str(), &entry);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (isDotEntry(entry.cFileName))
                continue;

            const std::wstring child = join(path, entry.cFileName);
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                freed += removeTree(child, since);
                if (directoryExists(child))
                    emptied = false;
                continue;
            }

            const double removed = removeFileIfNewer(child, since);
            freed += removed;
            if (removed <= 0.0 && pathExists(child))
                emptied = false;
        } while (FindNextFileW(find, &entry));
        FindClose(find);
    }

    // A directory goes only when nothing in it survived the cutoff, so a
    // time-scoped removal leaves the sites it spared where WebKit expects them.
    if (emptied)
        RemoveDirectoryW(path.c_str());
    return freed;
}

std::vector<std::wstring> storageSubdirectoriesForType(int type)
{
    switch (type) {
    case HB_DATA_TYPE_LOCAL_STORAGE:
        return { L"LocalStorage", L"SessionStorage" };
    case HB_DATA_TYPE_INDEXED_DB:
        return { L"IndexedDB" };
    case HB_DATA_TYPE_DOM_CACHE:
        return { L"CacheStorage", L"BackgroundFetchStorage" };
    case HB_DATA_TYPE_SERVICE_WORKERS:
        return { L"ServiceWorkers" };
    default:
        return { };
    }
}

// --- The worker -------------------------------------------------------------

void submitWork(std::function<void()> job)
{
    if (!job)
        return;

    std::lock_guard<std::mutex> lock(g_workMutex);
    if (g_workerStopping)
        return;
    if (!g_workerRunning) {
        g_workerRunning = true;
        g_worker = std::thread(workerMain);
    }
    g_work.push_back(std::move(job));
    g_workSignal.notify_one();
}

void stopWorker()
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_workMutex);
        if (!g_workerRunning)
            return;
        g_workerStopping = true;
        worker = std::move(g_worker);
    }
    g_workSignal.notify_all();
    if (worker.joinable())
        worker.join();

    std::lock_guard<std::mutex> lock(g_workMutex);
    g_workerRunning = false;
}

} // namespace harmony::datastore
