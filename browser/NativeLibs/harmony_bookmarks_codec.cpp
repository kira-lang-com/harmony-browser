#include "harmony_bookmarks_internal.h"

#include <algorithm>

// What the two files hold, and how a model turns into one and back.
//
// One record per line, one field per tab. A title carries whatever the document
// called itself, so the two characters that would end a field or a line are
// escaped rather than stripped: a page named "a\tb" must come back named "a\tb".
//
// Both models are written whole rather than appended to, because both are edited
// in the middle. A visit deleted from the history and a bookmark moved between
// folders are the ordinary cases, and a log would have to be replayed from its
// beginning to answer either.
//
// Everything here runs under the model lock, on whichever thread noticed the
// change. What it produces is a string, and the disk is somebody else's problem:
// see `harmony_bookmarks_file.cpp`.

namespace harmony::bookmarks {

namespace {

constexpr char kHistoryHeader[] = "harmony-history 1";
constexpr char kBookmarksHeader[] = "harmony-bookmarks 1";

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

// A carriage return is dropped only when lines are being cut, so a file written
// on this platform and one written without them read the same.
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

    // The children are copied before the walk descends: the vector they name
    // lives in the same storage every other node lives in.
    const std::vector<int> children = node->children;
    for (int child : children)
        encodeNodeLocked(child, contents);
}

} // namespace

// --- History ------------------------------------------------------------------

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
        entry.foldedURL = matchableURL(entry.url);
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
        // hold visits out of order. Every read assumes the last is the most
        // recent, so the order is restored here rather than defended against at
        // each of them.
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

// --- Bookmarks ------------------------------------------------------------------

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
        // updates rather than creates: a second node carrying a root's id would
        // be a bar that draws twice.
        if (id == HB_BOOKMARK_ROOT_BAR || id == HB_BOOKMARK_ROOT_OTHER) {
            BookmarkNode* root = bookmarkNodeLocked(id);
            if (root && !title.empty()) {
                root->title = title;
                root->foldedTitle = foldCase(title);
            }
            continue;
        }

        const BookmarkNode* target = bookmarkNodeLocked(parent);
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
            node.foldedURL = matchableURL(url);
        }

        nodes.push_back(std::move(node));

        // The parent is looked up again: pushing the child may have moved the
        // storage every node lives in, and the pointer taken before it did is no
        // longer the parent's.
        if (BookmarkNode* parentNode = bookmarkNodeLocked(parent))
            parentNode->children.push_back(id);
        if (id > highest)
            highest = id;
    }

    bookmarksNextIdLocked() = highest + 1;
}

} // namespace harmony::bookmarks
