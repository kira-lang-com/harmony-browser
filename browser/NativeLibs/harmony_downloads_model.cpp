#include "harmony_downloads.h"
#include "harmony_downloads_internal.h"

#include <deque>
#include <mutex>

namespace harmony_downloads {
namespace model {
namespace {

// A URL the host asked to be downloaded rather than shown, and when that ask
// stops standing. Without the deadline a "save link as" the user abandoned
// would turn an ordinary visit to the same address into a download later on.
struct ForcedURL {
    std::string url;
    ULONGLONG deadline { 0 };
};

constexpr ULONGLONG kForcedURLLifetimeMs = 60000;

std::mutex g_mutex;
std::vector<Record> g_records;
std::vector<ForcedURL> g_forced;
std::deque<int> g_cancels;
int g_nextId { 1 };
int g_revision { 0 };
bool g_loaded { false };

void ensureLoadedLocked()
{
    if (g_loaded)
        return;
    g_loaded = true;

    std::vector<Record> history = readHistory();
    for (Record& record : history) {
        record.id = g_nextId++;
        g_records.push_back(std::move(record));
    }
    ++g_revision;
}

Record* findLocked(int id)
{
    for (Record& record : g_records) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

const Record* findLockedConst(int id)
{
    for (const Record& record : g_records) {
        if (record.id == id)
            return &record;
    }
    return nullptr;
}

// The terminal records as the history file should hold them. Taken under the
// lock and written outside it, so a frame never waits behind a disk write.
std::vector<Record> historySnapshotLocked()
{
    std::vector<Record> snapshot;
    for (const Record& record : g_records) {
        if (!isTerminal(record.state))
            continue;
        Record copy = record;
        copy.download = nullptr;
        snapshot.push_back(std::move(copy));
    }
    return snapshot;
}

}

int add(std::string url, WKDownloadRef download, std::string fileName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();

    Record record;
    record.id = g_nextId++;
    record.state = State::Starting;
    record.download = download;
    record.url = std::move(url);
    record.fileName = std::move(fileName);

    const int id = record.id;
    g_records.insert(g_records.begin(), std::move(record));
    ++g_revision;
    return id;
}

int revision()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    return g_revision;
}

int count()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    return static_cast<int>(g_records.size());
}

int idAt(int index)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    if (index < 0 || index >= static_cast<int>(g_records.size()))
        return 0;
    return g_records[static_cast<size_t>(index)].id;
}

int activeCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    int active = 0;
    for (const Record& record : g_records) {
        if (!isTerminal(record.state))
            ++active;
    }
    return active;
}

int stateOf(int id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    const Record* record = findLockedConst(id);
    return record ? static_cast<int>(record->state) : HB_DOWNLOAD_STATE_FAILED;
}

long long receivedBytes(int id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    const Record* record = findLockedConst(id);
    return record ? record->received : 0;
}

long long totalBytes(int id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    const Record* record = findLockedConst(id);
    return record ? record->total : -1;
}

std::string textOf(int id, int field)
{
    unsigned long long completedAt = 0;
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoadedLocked();
        const Record* record = findLockedConst(id);
        if (!record)
            return { };

        switch (field) {
        case HB_DOWNLOAD_TEXT_URL: text = record->url; break;
        case HB_DOWNLOAD_TEXT_FILE_NAME: text = record->fileName; break;
        case HB_DOWNLOAD_TEXT_PATH: text = record->path; break;
        case HB_DOWNLOAD_TEXT_MIME_TYPE: text = record->mimeType; break;
        case HB_DOWNLOAD_TEXT_ERROR: text = record->error; break;
        case HB_DOWNLOAD_TEXT_COMPLETED: completedAt = record->completedAt; break;
        default: break;
        }
    }

    // Formatting a date asks the locale, which is not something to do with the
    // records locked.
    if (completedAt)
        return formatTimestamp(completedAt);
    return text;
}

std::wstring nativePathOf(int id, bool finishedOnly)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();
    const Record* record = findLockedConst(id);
    if (!record)
        return { };
    if (finishedOnly && record->state != State::Finished)
        return { };
    return record->nativePath;
}

void setURL(int id, std::string url)
{
    if (url.empty())
        return;

    std::lock_guard<std::mutex> lock(g_mutex);
    Record* record = findLocked(id);
    if (!record || record->url == url)
        return;

    record->url = std::move(url);
    ++g_revision;
}

void setDestination(
    int id,
    std::string fileName,
    std::string path,
    std::wstring nativePath,
    std::string mimeType
) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Record* record = findLocked(id);
    if (!record)
        return;

    record->fileName = std::move(fileName);
    record->path = std::move(path);
    record->nativePath = std::move(nativePath);
    record->mimeType = std::move(mimeType);
    if (record->state == State::Starting)
        record->state = State::Running;

    ++g_revision;
}

void setProgress(int id, long long received, long long total)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    Record* record = findLocked(id);
    if (!record || isTerminal(record->state))
        return;

    record->received = received;
    record->total = total;

    // The first byte written is what says a download that had only been asked
    // for is now running.
    if (record->state == State::Starting)
        record->state = State::Running;

    ++g_revision;
}

WKDownloadRef finish(int id, State state, std::string error)
{
    WKDownloadRef stale = nullptr;
    std::vector<Record> snapshot;
    bool changed = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoadedLocked();

        Record* record = findLocked(id);
        if (record && !isTerminal(record->state)) {
            record->state = state;
            record->error = std::move(error);
            record->completedAt = nowTicks();
            if (record->total < 0 && state == State::Finished)
                record->total = record->received;

            stale = record->download;
            record->download = nullptr;

            ++g_revision;
            snapshot = historySnapshotLocked();
            changed = true;
        }
    }

    if (changed)
        writeHistory(snapshot);
    return stale;
}

std::vector<WKDownloadRef> takeLiveDownloads()
{
    std::vector<WKDownloadRef> live;
    std::vector<Record> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Record& record : g_records) {
            if (!record.download)
                continue;
            live.push_back(record.download);
            record.download = nullptr;
        }
        g_cancels.clear();
        g_forced.clear();
        snapshot = historySnapshotLocked();
        ++g_revision;
    }

    writeHistory(snapshot);
    return live;
}

void remove(int id)
{
    std::vector<Record> snapshot;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoadedLocked();

        for (size_t index = 0; index < g_records.size(); ++index) {
            if (g_records[index].id != id)
                continue;
            // A running download is cancelled, not forgotten: dropping the
            // record would leave WebKit writing a file nothing is watching.
            if (!isTerminal(g_records[index].state))
                return;

            g_records.erase(g_records.begin() + static_cast<std::ptrdiff_t>(index));
            ++g_revision;
            changed = true;
            break;
        }
        if (changed)
            snapshot = historySnapshotLocked();
    }

    if (changed)
        writeHistory(snapshot);
}

void clearTerminal()
{
    std::vector<Record> snapshot;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        ensureLoadedLocked();

        std::vector<Record> kept;
        kept.reserve(g_records.size());
        for (Record& record : g_records) {
            if (isTerminal(record.state))
                continue;
            kept.push_back(std::move(record));
        }
        if (kept.size() == g_records.size())
            return;

        g_records = std::move(kept);
        ++g_revision;
        snapshot = historySnapshotLocked();
    }

    writeHistory(snapshot);
}

void queueCancel(int id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    ensureLoadedLocked();

    const Record* record = findLockedConst(id);
    if (!record || isTerminal(record->state))
        return;

    for (int queued : g_cancels) {
        if (queued == id)
            return;
    }
    g_cancels.push_back(id);
}

bool nextCancel(int& id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_cancels.empty())
        return false;

    id = g_cancels.front();
    g_cancels.pop_front();
    return true;
}

WKDownloadRef liveDownload(int id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const Record* record = findLockedConst(id);
    if (!record || isTerminal(record->state))
        return nullptr;
    return record->download;
}

void forceURL(std::string url)
{
    if (url.empty())
        return;

    ForcedURL forced;
    forced.url = std::move(url);
    forced.deadline = GetTickCount64() + kForcedURLLifetimeMs;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_forced.push_back(std::move(forced));
}

bool consumeForcedURL(const std::string& url)
{
    if (url.empty())
        return false;

    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_mutex);

    bool matched = false;
    std::vector<ForcedURL> kept;
    kept.reserve(g_forced.size());
    for (ForcedURL& forced : g_forced) {
        if (!matched && forced.url == url) {
            matched = true;
            continue;
        }
        if (forced.deadline > now)
            kept.push_back(std::move(forced));
    }
    g_forced = std::move(kept);
    return matched;
}

}
}
