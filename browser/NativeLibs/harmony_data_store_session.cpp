#include "harmony_data_store_internal.h"

#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

// The open tabs and their history, across a restart.
//
// The list a browser reopens with is the registry's published list, which any
// thread may read. The history behind each tab is not: WKPageCopySessionState
// takes a page, and pages belong to the WebKit thread. So the two are kept
// apart -- the engine thread refreshes the history blobs as the tabs change,
// and the file is written from the published list plus the newest blob for each
// tab. That is what lets the last write happen at shutdown, after the frame
// thread has stopped calling the pump but before the registry destroys a page.
//
// Private tabs are never written. A session that remembered them would be a
// private session that outlived the process.

namespace harmony::datastore {

namespace {

constexpr char kMagic[8] = { 'H', 'B', 'S', 'E', 'S', 'S', 'N', '\0' };
constexpr uint32_t kVersion = 1;

// How long the tab list has to stand still before the session is written. A
// page reports its title, its address and its progress separately, so a single
// navigation moves the registry's revision several times.
constexpr double kSettleSeconds = 1.2;

// How long an adopted entry waits for its tab's page before the history it was
// carrying is dropped. The tab is already showing the right address by then;
// only its back list is lost.
constexpr double kAdoptSeconds = 15.0;

struct Entry {
    std::string url;
    std::string title;
    bool pinned = false;
    std::vector<uint8_t> state;
};

struct Adoption {
    int index = 0;
    int tabId = 0;
    double deadline = 0.0;
    std::vector<uint8_t> state;
};

std::mutex g_restoreMutex;
std::vector<Entry> g_restore;
int g_restoreActive = -1;

std::mutex g_capturedMutex;
std::unordered_map<int, std::vector<uint8_t>> g_captured;

// Engine thread.
std::vector<Adoption> g_adoptions;
int g_lastRevision = -1;
double g_dirtySince = 0.0;

std::atomic<bool> g_loaded { false };
std::atomic<int> g_adoptionsPending { 0 };

void appendU32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

void appendBytes(std::vector<uint8_t>& out, const void* bytes, size_t size)
{
    appendU32(out, static_cast<uint32_t>(size));
    const uint8_t* source = static_cast<const uint8_t*>(bytes);
    out.insert(out.end(), source, source + size);
}

bool readU32(const std::vector<uint8_t>& bytes, size_t& at, uint32_t& value)
{
    if (at + 4 > bytes.size())
        return false;
    value = static_cast<uint32_t>(bytes[at])
        | (static_cast<uint32_t>(bytes[at + 1]) << 8)
        | (static_cast<uint32_t>(bytes[at + 2]) << 16)
        | (static_cast<uint32_t>(bytes[at + 3]) << 24);
    at += 4;
    return true;
}

bool readBlock(const std::vector<uint8_t>& bytes, size_t& at, std::vector<uint8_t>& out)
{
    uint32_t size = 0;
    if (!readU32(bytes, at, size))
        return false;
    if (at + size > bytes.size())
        return false;
    out.assign(bytes.begin() + static_cast<ptrdiff_t>(at), bytes.begin() + static_cast<ptrdiff_t>(at + size));
    at += size;
    return true;
}

std::vector<uint8_t> readFile(const std::wstring& path)
{
    std::vector<uint8_t> bytes;
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return bytes;

    uint8_t buffer[16384];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
        bytes.insert(bytes.end(), buffer, buffer + read);
    CloseHandle(file);
    return bytes;
}

// Written beside the session and moved over it, so a crash mid-write leaves the
// session that was there rather than half of a new one.
void writeFileAtomically(const std::wstring& path, const std::wstring& temporary, const std::vector<uint8_t>& bytes)
{
    HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE) {
        setError("the session could not be opened for writing");
        return;
    }

    DWORD written = 0;
    const bool ok = bytes.empty()
        || WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0;
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);

    if (!ok || !flushed || written != bytes.size()) {
        DeleteFileW(temporary.c_str());
        setError("the session could not be written whole");
        return;
    }

    // The move is what makes the new session the session. A move that failed
    // leaves the previous one in place, and the half-written sibling would
    // otherwise stay on disk for the rest of the profile's life.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        setError("the session could not replace the one already saved");
    }
}

// Engine thread. One tab's back/forward list, as WebKit serialises it.
bool everyValueAllowed(WKPageRef, WKStringRef, WKTypeRef, void*)
{
    return true;
}

std::vector<uint8_t> captureHistory(WKPageRef page)
{
    std::vector<uint8_t> bytes;
    Api& wk = api();
    if (!page || !wk.pageCopySessionState || !wk.sessionStateCopyData || !wk.dataGetBytes || !wk.dataGetSize)
        return bytes;

    WKTypeRef state = wk.pageCopySessionState(page, nullptr, everyValueAllowed);
    if (!state)
        return bytes;

    WKDataRef data = wk.sessionStateCopyData(state);
    if (data) {
        const unsigned char* source = wk.dataGetBytes(data);
        const size_t size = wk.dataGetSize(data);
        if (source && size)
            bytes.assign(source, source + size);
        wkRelease(data);
    }
    wkRelease(state);
    return bytes;
}

// Engine thread. Refreshes the stored history of every open tab, so the next
// write -- whoever makes it -- has something current to write.
void captureOpenTabs()
{
    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int id = hb_tabs_id_at(index);
        if (id <= 0 || tabIsPrivate(id))
            continue;

        void* page = hb_tabs_page(id);
        if (!page)
            continue;

        std::vector<uint8_t> history = captureHistory(page);
        if (history.empty())
            continue;

        std::lock_guard<std::mutex> lock(g_capturedMutex);
        g_captured[id] = std::move(history);
    }
}

// Any thread. The published list, plus whatever history was last captured for
// each tab in it.
std::vector<uint8_t> encodeSession()
{
    std::vector<uint8_t> out;
    out.insert(out.end(), kMagic, kMagic + sizeof(kMagic));
    appendU32(out, kVersion);

    const int count = hb_tabs_count();
    const int activeId = hb_tabs_active_id();

    std::vector<int> ids;
    ids.reserve(static_cast<size_t>(count > 0 ? count : 0));
    int activeIndex = -1;
    for (int index = 0; index < count; ++index) {
        const int id = hb_tabs_id_at(index);
        if (id <= 0 || tabIsPrivate(id))
            continue;
        if (id == activeId)
            activeIndex = static_cast<int>(ids.size());
        ids.push_back(id);
    }

    appendU32(out, static_cast<uint32_t>(ids.size()));
    appendU32(out, static_cast<uint32_t>(activeIndex));

    std::lock_guard<std::mutex> lock(g_capturedMutex);
    for (int id : ids) {
        const char* url = hb_tabs_url(id);
        const std::string address = url ? url : "";
        const char* title = hb_tabs_title(id);
        const std::string label = title ? title : "";

        appendBytes(out, address.data(), address.size());
        appendBytes(out, label.data(), label.size());
        out.push_back(hb_tabs_is_pinned(id) ? 1 : 0);

        const auto history = g_captured.find(id);
        if (history == g_captured.end())
            appendU32(out, 0);
        else
            appendBytes(out, history->second.data(), history->second.size());
    }
    return out;
}

void writeSession()
{
    std::vector<uint8_t> bytes = encodeSession();
    const std::wstring path = layout().sessionFile;
    const std::wstring temporary = layout().sessionTempFile;
    submitWork([path, temporary, bytes = std::move(bytes)]() {
        writeFileAtomically(path, temporary, bytes);
    });
}

// Engine thread. Every site the browser is on is a site it holds data for.
void noteOpenOrigins()
{
    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int id = hb_tabs_id_at(index);
        if (id <= 0 || tabIsPrivate(id))
            continue;
        const char* url = hb_tabs_url(id);
        if (!url || !*url)
            continue;
        ledgerNote(originForURL(url));
    }
}

// Engine thread. Hands a restored back/forward list to the page standing in for
// it, once that page exists.
void driveAdoptions()
{
    if (g_adoptions.empty())
        return;

    Api& wk = api();
    const double now = unixNow();

    for (size_t at = 0; at < g_adoptions.size();) {
        Adoption& adoption = g_adoptions[at];
        void* page = hb_tabs_page(adoption.tabId);
        const bool expired = now > adoption.deadline;

        if (!page && !expired) {
            ++at;
            continue;
        }

        if (page && !adoption.state.empty() && wk.sessionStateCreateFromData && wk.dataCreate && wk.pageRestoreFromSessionState) {
            WKDataRef data = wk.dataCreate(adoption.state.data(), adoption.state.size());
            if (data) {
                WKSessionStateRef state = wk.sessionStateCreateFromData(data);
                if (state) {
                    wk.pageRestoreFromSessionState(page, state);
                    wkRelease(state);
                }
                wkRelease(data);
            }
        }

        g_adoptions.erase(g_adoptions.begin() + static_cast<ptrdiff_t>(at));
    }

    g_adoptionsPending.store(static_cast<int>(g_adoptions.size()));
}

} // namespace

void sessionLoad()
{
    if (g_loaded.exchange(true))
        return;

    const std::vector<uint8_t> bytes = readFile(layout().sessionFile);
    if (bytes.size() < sizeof(kMagic) + 12)
        return;
    if (std::memcmp(bytes.data(), kMagic, sizeof(kMagic)) != 0)
        return;

    size_t at = sizeof(kMagic);
    uint32_t version = 0;
    uint32_t count = 0;
    uint32_t active = 0;
    if (!readU32(bytes, at, version) || version != kVersion)
        return;
    if (!readU32(bytes, at, count) || !readU32(bytes, at, active))
        return;

    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t index = 0; index < count; ++index) {
        std::vector<uint8_t> url;
        std::vector<uint8_t> title;
        if (!readBlock(bytes, at, url) || !readBlock(bytes, at, title))
            return;
        if (at >= bytes.size())
            return;

        Entry entry;
        entry.url.assign(url.begin(), url.end());
        entry.title.assign(title.begin(), title.end());
        entry.pinned = bytes[at++] != 0;
        if (!readBlock(bytes, at, entry.state))
            return;
        if (!entry.url.empty())
            entries.push_back(std::move(entry));
    }

    std::lock_guard<std::mutex> lock(g_restoreMutex);
    g_restore = std::move(entries);
    g_restoreActive = static_cast<int>(static_cast<int32_t>(active));
    if (g_restoreActive >= static_cast<int>(g_restore.size()))
        g_restoreActive = -1;
}

void sessionPump()
{
    driveAdoptions();

    const int revision = hb_tabs_revision();
    if (revision != g_lastRevision) {
        g_lastRevision = revision;
        g_dirtySince = unixNow();
        noteOpenOrigins();
        return;
    }

    if (g_dirtySince <= 0.0)
        return;
    if (unixNow() - g_dirtySince < kSettleSeconds)
        return;

    g_dirtySince = 0.0;
    captureOpenTabs();
    writeSession();
    ledgerSave();
}

void sessionShutdown()
{
    // The pages are still alive here only when the caller is the engine thread.
    // From the frame thread this writes the list and the newest history the
    // pump had already captured, which is what the file is designed to allow.
    if (hb_tabs_on_webkit_thread())
        captureOpenTabs();
    writeSession();
}

} // namespace harmony::datastore

using namespace harmony::datastore;

extern "C" int hb_data_store_session_restore_count(void)
{
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    return static_cast<int>(g_restore.size());
}

extern "C" const char* hb_data_store_session_restore_url(int index)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    if (index < 0 || index >= static_cast<int>(g_restore.size()))
        storage.clear();
    else
        storage = g_restore[static_cast<size_t>(index)].url;
    return storage.c_str();
}

extern "C" const char* hb_data_store_session_restore_title(int index)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    if (index < 0 || index >= static_cast<int>(g_restore.size()))
        storage.clear();
    else
        storage = g_restore[static_cast<size_t>(index)].title;
    return storage.c_str();
}

extern "C" int hb_data_store_session_restore_pinned(int index)
{
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    if (index < 0 || index >= static_cast<int>(g_restore.size()))
        return 0;
    return g_restore[static_cast<size_t>(index)].pinned ? 1 : 0;
}

extern "C" int hb_data_store_session_restore_active_index(void)
{
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    return g_restoreActive;
}

extern "C" void hb_data_store_session_adopt(int index, int tab_id)
{
    if (tab_id <= 0)
        return;

    Adoption adoption;
    adoption.index = index;
    adoption.tabId = tab_id;
    adoption.deadline = unixNow() + kAdoptSeconds;
    {
        std::lock_guard<std::mutex> lock(g_restoreMutex);
        if (index < 0 || index >= static_cast<int>(g_restore.size()))
            return;
        adoption.state = g_restore[static_cast<size_t>(index)].state;
    }
    if (adoption.state.empty())
        return;

    // The adoption is queued onto the engine thread rather than pushed from
    // here: the list it joins is read by the pump, and the pages it waits for
    // are that thread's.
    Adoption* owned = new Adoption(std::move(adoption));
    hb_tabs_invoke_on_webkit_thread([](void* context) {
        Adoption* queued = static_cast<Adoption*>(context);
        g_adoptions.push_back(std::move(*queued));
        g_adoptionsPending.store(static_cast<int>(g_adoptions.size()));
        delete queued;
    }, owned);
}

extern "C" int hb_data_store_session_restore_pending(void)
{
    return g_adoptionsPending.load();
}

extern "C" void hb_data_store_session_restore_finished(void)
{
    std::lock_guard<std::mutex> lock(g_restoreMutex);
    g_restore.clear();
    g_restoreActive = -1;
}
