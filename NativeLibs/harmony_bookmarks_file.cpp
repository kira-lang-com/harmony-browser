#include "harmony_bookmarks_internal.h"

#include "harmony_data_store.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <thread>

// Where the two models live on disk, and everything they do to it.
//
// Under the profile the data store owns, because a browser with two state
// directories is a browser that clears one of them and keeps the other. The path
// is asked of that module rather than recomputed here: the two would otherwise
// have to agree about a known folder, an environment variable and a fallback,
// and a disagreement would show up as a browser that forgets everything between
// runs on exactly the machines where the fallback was taken.
//
// Both files are written whole, to a sibling, and moved over the real name. A
// history half-written by a process that was killed mid-write is a history that
// cannot be read back at all, and the move is what makes the new file the file.
//
// Neither write happens on the thread that noticed the change. Encoding runs
// under the model lock on the WebKit thread, and the bytes go to a worker of
// this module's own: a page must not wait for a disk while it commits.

namespace harmony::bookmarks {

namespace {

constexpr wchar_t kHistoryFileName[] = L"History.hbhistory";
constexpr wchar_t kBookmarksFileName[] = L"Bookmarks.hbbookmarks";
constexpr wchar_t kTemporarySuffix[] = L".new";

constexpr char kHistoryHeader[] = "harmony-history 1";
constexpr char kBookmarksHeader[] = "harmony-bookmarks 1";

// How long a model has to stand still before it is written. A navigation moves
// the history twice -- once when it commits and once when the title arrives --
// and dragging a bookmark across a folder moves the tree once per frame it is
// held over.
constexpr double kSettleSeconds = 1.5;

std::wstring g_root;
bool g_loaded = false;

double g_historyDirtySince = 0.0;
double g_bookmarksDirtySince = 0.0;

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

// --- Bytes ------------------------------------------------------------------

std::string readWholeFile(const std::wstring& path)
{
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
        return { };

    std::string contents;
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) || !read)
            break;
        contents.append(buffer, read);
    }
    CloseHandle(file);
    return contents;
}

void writeFileAtomically(const std::wstring& path, const std::string& contents, const char* what)
{
    const std::wstring temporary = path + kTemporarySuffix;
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
        setError(std::string("the ") + what + " could not be opened for writing");
        return;
    }

    bool ok = true;
    size_t written = 0;
    while (written < contents.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(std::min<size_t>(contents.size() - written, 1u << 20));
        if (!WriteFile(file, contents.data() + written, remaining, &chunk, nullptr) || !chunk) {
            ok = false;
            break;
        }
        written += chunk;
    }
    const bool flushed = FlushFileBuffers(file) != 0;
    CloseHandle(file);

    if (!ok || !flushed) {
        DeleteFileW(temporary.c_str());
        setError(std::string("the ") + what + " could not be written whole");
        return;
    }

    // The move is what makes the new file the file. A move that failed leaves
    // the previous one in place, and the half-written sibling would otherwise
    // stay on disk for the rest of the profile's life.
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        setError(std::string("the ") + what + " could not replace the one already saved");
        return;
    }
    clearError();
}

// --- Fields -----------------------------------------------------------------
//
// One record per line, one field per tab. A title carries whatever the document
// called itself, so the two characters that would end a field or a line are
// escaped rather than stripped: a page named "a\tb" must come back named "a\tb".

std::string escapeField(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '\t': escaped += "\\t"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string unescapeField(const std::string& value)
{
    std::string plain;
    plain.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            plain += value[index];
            continue;
        }
        ++index;
        switch (value[index]) {
        case 't': plain += '\t'; break;
        case 'n': plain += '\n'; break;
        case 'r': plain += '\r'; break;
        default: plain += value[index]; break;
        }
    }
    return plain;
}

std::vector<std::string> splitOn(const std::string& text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    for (char character : text) {
        if (character == separator) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        if (separator != '\n' || character != '\r')
            current += character;
    }
    parts.push_back(current);
    return parts;
}

long long parseSigned(const std::string& value)
{
    long long parsed = 0;
    bool negative = false;
    size_t index = 0;
    if (index < value.size() && (value[index] == '-' || value[index] == '+')) {
        negative = value[index] == '-';
        ++index;
    }
    for (; index < value.size(); ++index) {
        if (value[index] < '0' || value[index] > '9')
            break;
        parsed = parsed * 10 + (value[index] - '0');
    }
    return negative ? -parsed : parsed;
}

// --- The two files ----------------------------------------------------------

std::wstring historyPath()
{
    return g_root.empty() ? std::wstring() : g_root + L"\\" + kHistoryFileName;
}

std::wstring bookmarksPath()
{
    return g_root.empty() ? std::wstring() : g_root + L"\\" + kBookmarksFileName;
}

std::string encodeHistoryLocked()
{
    std::string contents = kHistoryHeader;
    contents += '\n';

    for (const HistoryEntry& entry : historyEntriesLocked()) {
        if (entry.visits.empty())
            continue;

        contents += escapeField(entry.url);
        contents += '\t';
        contents += escapeField(entry.title);
        contents += '\t';
        for (size_t index = 0; index < entry.visits.size(); ++index) {
            if (index)
                contents += ',';
            contents += std::to_string(static_cast<long long>(entry.visits[index]));
        }
        contents += '\n';
    }
    return contents;
}

void decodeHistoryLocked(const std::string& contents)
{
    const std::vector<std::string> lines = splitOn(contents, '\n');
    if (lines.empty() || lines.front() != kHistoryHeader)
        return;

    std::vector<HistoryEntry>& entries = historyEntriesLocked();
    entries.clear();

    for (size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty())
            continue;

        const std::vector<std::string> fields = splitOn(lines[index], '\t');
        if (fields.size() < 3)
            continue;

        HistoryEntry entry;
        entry.url = unescapeField(fields[0]);
        if (!isRecordableURL(entry.url))
            continue;

        entry.title = unescapeField(fields[1]);
        entry.host = hostOfURL(entry.url);
        entry.foldedURL = foldCase(entry.url);
        const auto separator = entry.foldedURL.find("://");
        if (separator != std::string::npos)
            entry.foldedURL.erase(0, separator + 3);
        if (entry.foldedURL.rfind("www.", 0) == 0)
            entry.foldedURL.erase(0, 4);
        entry.foldedTitle = foldCase(entry.title);

        for (const std::string& stamp : splitOn(fields[2], ',')) {
            if (stamp.empty())
                continue;
            const double at = static_cast<double>(parseSigned(stamp));
            if (at > 0.0)
                entry.visits.push_back(at);
        }
        if (entry.visits.empty())
            continue;

        // A file edited by hand, or written by a run that was interrupted, can
        // hold visits out of order. Every read below assumes the last is the
        // most recent, so the order is restored here rather than defended
        // against at each of them.
        std::sort(entry.visits.begin(), entry.visits.end());
        if (entry.visits.size() > kMaxVisitsPerEntry) {
            entry.visits.erase(
                entry.visits.begin(),
                entry.visits.begin() + static_cast<ptrdiff_t>(entry.visits.size() - kMaxVisitsPerEntry)
            );
        }

        entries.push_back(std::move(entry));
        if (entries.size() >= kMaxHistoryEntries)
            break;
    }
}

void encodeNodeLocked(int id, std::string& contents)
{
    const BookmarkNode* node = bookmarkNodeLocked(id);
    if (!node)
        return;

    contents += std::to_string(node->id);
    contents += '\t';
    contents += std::to_string(node->parent);
    contents += '\t';
    contents += node->folder ? "1" : "0";
    contents += '\t';
    contents += std::to_string(static_cast<long long>(node->added));
    contents += '\t';
    contents += escapeField(node->title);
    contents += '\t';
    contents += escapeField(node->url);
    contents += '\n';

    const std::vector<int> children = node->children;
    for (int child : children)
        encodeNodeLocked(child, contents);
}

std::string encodeBookmarksLocked()
{
    bookmarksEnsureRootsLocked();

    std::string contents = kBookmarksHeader;
    contents += '\n';
    // Depth first, parents before children, siblings in order. That is exactly
    // the order the reader rebuilds in, so the file IS the tree rather than a
    // set of nodes plus a rule for sorting them.
    encodeNodeLocked(HB_BOOKMARK_ROOT_BAR, contents);
    encodeNodeLocked(HB_BOOKMARK_ROOT_OTHER, contents);
    return contents;
}

void decodeBookmarksLocked(const std::string& contents)
{
    const std::vector<std::string> lines = splitOn(contents, '\n');
    if (lines.empty() || lines.front() != kBookmarksHeader)
        return;

    std::vector<BookmarkNode>& nodes = bookmarkNodesLocked();
    nodes.clear();
    bookmarksEnsureRootsLocked();

    int highest = HB_BOOKMARK_ROOT_OTHER;
    for (size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty())
            continue;

        const std::vector<std::string> fields = splitOn(lines[index], '\t');
        if (fields.size() < 6)
            continue;

        const int id = static_cast<int>(parseSigned(fields[0]));
        const int parent = static_cast<int>(parseSigned(fields[1]));
        if (id <= 0)
            continue;

        const std::string title = unescapeField(fields[4]);
        const std::string url = unescapeField(fields[5]);

        // The roots are already there, and are the one pair of nodes the file
        // updates rather than creates: a second node with a root's id would be
        // a bar that draws twice.
        if (id == HB_BOOKMARK_ROOT_BAR || id == HB_BOOKMARK_ROOT_OTHER) {
            if (BookmarkNode* root = bookmarkNodeLocked(id)) {
                if (!title.empty()) {
                    root->title = title;
                    root->foldedTitle = foldCase(title);
                }
            }
            continue;
        }

        BookmarkNode* target = bookmarkNodeLocked(parent);
        if (!target || !target->folder)
            continue;

        BookmarkNode node;
        node.id = id;
        node.parent = parent;
        node.folder = parseSigned(fields[2]) != 0;
        node.added = static_cast<double>(parseSigned(fields[3]));
        node.title = title;
        node.foldedTitle = foldCase(title);
        if (!node.folder) {
            if (url.empty())
                continue;
            node.url = url;
            node.host = hostOfURL(url);
            node.foldedURL = foldCase(url);
        }

        nodes.push_back(std::move(node));
        // The parent is looked up again: pushing the child may have moved the
        // storage every node lives in, and the pointer taken before it did is
        // no longer the parent's.
        if (BookmarkNode* parentNode = bookmarkNodeLocked(parent))
            parentNode->children.push_back(id);
        if (id > highest)
            highest = id;
    }

    bookmarksNextIdLocked() = highest + 1;
}

} // namespace

// --- Loading ----------------------------------------------------------------

bool filesReady()
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return g_loaded;
}

bool filesLoad()
{
    std::lock_guard<std::mutex> lock(modelMutex());
    if (g_loaded)
        return true;

    // The profile answers with nothing until the data store has prepared its
    // layout, which happens as the engine's context is created. So the load is
    // retried from the run-loop cycle rather than failing once and leaving the
    // browser with no memory for the rest of the run.
    const char* profile = hb_data_store_profile_path();
    if (!profile || !*profile)
        return false;

    g_root = widen(std::string(profile));
    if (g_root.empty() || !makeDirectories(g_root)) {
        setError("the profile directory could not be reached, so history and bookmarks will not be kept");
        return false;
    }

    decodeHistoryLocked(readWholeFile(historyPath()));
    decodeBookmarksLocked(readWholeFile(bookmarksPath()));
    bookmarksEnsureRootsLocked();

    g_loaded = true;
    g_historyDirtySince = 0.0;
    g_bookmarksDirtySince = 0.0;
    bumpRevisionLocked();
    return true;
}

// --- Writing ----------------------------------------------------------------

void markHistoryDirtyLocked()
{
    g_historyDirtySince = unixNow();
}

void markBookmarksDirtyLocked()
{
    g_bookmarksDirtySince = unixNow();
}

void filesPump()
{
    if (!filesLoad())
        return;

    std::lock_guard<std::mutex> lock(modelMutex());
    const double now = unixNow();

    if (g_historyDirtySince > 0.0 && now - g_historyDirtySince >= kSettleSeconds) {
        g_historyDirtySince = 0.0;
        const std::wstring path = historyPath();
        submitWork([path, contents = encodeHistoryLocked()]() {
            writeFileAtomically(path, contents, "browsing history");
        });
    }

    if (g_bookmarksDirtySince > 0.0 && now - g_bookmarksDirtySince >= kSettleSeconds) {
        g_bookmarksDirtySince = 0.0;
        const std::wstring path = bookmarksPath();
        submitWork([path, contents = encodeBookmarksLocked()]() {
            writeFileAtomically(path, contents, "bookmarks");
        });
    }
}

void filesFlush()
{
    if (!filesLoad())
        return;

    std::wstring historyFile;
    std::wstring bookmarksFile;
    std::string history;
    std::string bookmarks;
    {
        std::lock_guard<std::mutex> lock(modelMutex());
        if (g_historyDirtySince > 0.0) {
            g_historyDirtySince = 0.0;
            historyFile = historyPath();
            history = encodeHistoryLocked();
        }
        if (g_bookmarksDirtySince > 0.0) {
            g_bookmarksDirtySince = 0.0;
            bookmarksFile = bookmarksPath();
            bookmarks = encodeBookmarksLocked();
        }
    }

    if (!historyFile.empty())
        writeFileAtomically(historyFile, history, "browsing history");
    if (!bookmarksFile.empty())
        writeFileAtomically(bookmarksFile, bookmarks, "bookmarks");
}

void filesStop()
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

} // namespace harmony::bookmarks
