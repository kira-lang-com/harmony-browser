#include "harmony_bookmarks_internal.h"

#include <algorithm>

// The bookmark tree: folders and bookmarks, in the order the user put them in.
//
// Order is what makes this a tree rather than a set. A bookmarks bar is read
// left to right and a folder is read top to bottom, so a folder holds its
// children in a vector and a move is an erase and an insert -- not a sort key
// that two nodes can tie on and a list can then draw in either order.
//
// Nodes are addressed by id and never by pointer across a mutation: the storage
// is one vector, and a bookmark added from a menu while a panel walks the tree
// would otherwise leave that walk holding a reference into a reallocated buffer.

namespace harmony::bookmarks {

namespace {

std::vector<BookmarkNode> g_nodes;
int g_nextId = HB_BOOKMARK_ROOT_OTHER + 1;

// What the two roots are called. They are folders like any other except that
// they cannot be removed or moved: the bar is a place on screen, and a browser
// that let it be deleted would have a strip with nothing to draw and no way back.
constexpr char kBarTitle[] = "Bookmarks Bar";
constexpr char kOtherTitle[] = "Other Bookmarks";

bool isRootLocked(int id)
{
    return id == HB_BOOKMARK_ROOT_BAR || id == HB_BOOKMARK_ROOT_OTHER;
}

void detachLocked(int id)
{
    BookmarkNode* node = bookmarkNodeLocked(id);
    if (!node)
        return;

    BookmarkNode* parent = bookmarkNodeLocked(node->parent);
    if (!parent)
        return;

    parent->children.erase(
        std::remove(parent->children.begin(), parent->children.end(), id),
        parent->children.end()
    );
}

// Every node under a folder, the folder included, deepest last.
void collectSubtreeLocked(int id, std::vector<int>& out)
{
    out.push_back(id);
    const BookmarkNode* node = bookmarkNodeLocked(id);
    if (!node)
        return;

    // The children are copied before the walk descends: a recursive call may
    // create nothing here, but the vector this reads from is the same storage
    // every other node lives in.
    const std::vector<int> children = node->children;
    for (int child : children)
        collectSubtreeLocked(child, out);
}

bool isInsideLocked(int candidate, int folder)
{
    int at = candidate;
    while (at > 0) {
        if (at == folder)
            return true;
        const BookmarkNode* node = bookmarkNodeLocked(at);
        if (!node)
            return false;
        at = node->parent;
    }
    return false;
}

int childIndexLocked(int parent, int id)
{
    const BookmarkNode* folder = bookmarkNodeLocked(parent);
    if (!folder)
        return -1;

    const auto position = std::find(folder->children.begin(), folder->children.end(), id);
    if (position == folder->children.end())
        return -1;
    return static_cast<int>(position - folder->children.begin());
}

void insertChildLocked(int parent, int id, int index)
{
    BookmarkNode* folder = bookmarkNodeLocked(parent);
    if (!folder)
        return;

    const int count = static_cast<int>(folder->children.size());
    const int at = (index < 0 || index > count) ? count : index;
    folder->children.insert(folder->children.begin() + at, id);
}

} // namespace

std::vector<BookmarkNode>& bookmarkNodesLocked()
{
    return g_nodes;
}

BookmarkNode* bookmarkNodeLocked(int id)
{
    if (id <= 0)
        return nullptr;
    for (auto& node : g_nodes) {
        if (node.id == id)
            return &node;
    }
    return nullptr;
}

int& bookmarksNextIdLocked()
{
    return g_nextId;
}

void bookmarksEnsureRootsLocked()
{
    if (!bookmarkNodeLocked(HB_BOOKMARK_ROOT_BAR)) {
        BookmarkNode bar;
        bar.id = HB_BOOKMARK_ROOT_BAR;
        bar.folder = true;
        bar.title = kBarTitle;
        bar.added = unixNow();
        g_nodes.push_back(std::move(bar));
    }
    if (!bookmarkNodeLocked(HB_BOOKMARK_ROOT_OTHER)) {
        BookmarkNode other;
        other.id = HB_BOOKMARK_ROOT_OTHER;
        other.folder = true;
        other.title = kOtherTitle;
        other.added = unixNow();
        g_nodes.push_back(std::move(other));
    }
    if (g_nextId <= HB_BOOKMARK_ROOT_OTHER)
        g_nextId = HB_BOOKMARK_ROOT_OTHER + 1;
}

int bookmarksAddLocked(int parent, int index, const std::string& url, const std::string& title, bool folder)
{
    bookmarksEnsureRootsLocked();

    const BookmarkNode* target = bookmarkNodeLocked(parent);
    if (!target || !target->folder)
        return 0;
    if (!folder && url.empty())
        return 0;

    BookmarkNode node;
    node.id = g_nextId++;
    node.parent = parent;
    node.folder = folder;
    node.title = title;
    node.added = unixNow();
    if (!folder) {
        node.url = url;
        node.host = hostOfURL(url);
        node.foldedURL = matchableURL(url);
    }
    node.foldedTitle = foldCase(title);

    const int id = node.id;
    g_nodes.push_back(std::move(node));
    insertChildLocked(parent, id, index);

    markBookmarksDirtyLocked();
    bumpRevisionLocked();
    return id;
}

void bookmarksRemoveLocked(int id)
{
    if (isRootLocked(id) || !bookmarkNodeLocked(id))
        return;

    std::vector<int> doomed;
    collectSubtreeLocked(id, doomed);
    detachLocked(id);

    g_nodes.erase(
        std::remove_if(g_nodes.begin(), g_nodes.end(), [&doomed](const BookmarkNode& node) {
            return std::find(doomed.begin(), doomed.end(), node.id) != doomed.end();
        }),
        g_nodes.end()
    );

    markBookmarksDirtyLocked();
    bumpRevisionLocked();
}

void bookmarksMoveLocked(int id, int parent, int index)
{
    if (isRootLocked(id))
        return;

    const BookmarkNode* node = bookmarkNodeLocked(id);
    const BookmarkNode* target = bookmarkNodeLocked(parent);
    if (!node || !target || !target->folder)
        return;

    // A folder cannot be moved inside itself. A tree that can be made to contain
    // itself is a tree that cannot be walked, and the walk is what draws it.
    if (isInsideLocked(parent, id))
        return;

    const bool sameParent = node->parent == parent;
    const int from = sameParent ? childIndexLocked(parent, id) : -1;

    detachLocked(id);
    if (BookmarkNode* moved = bookmarkNodeLocked(id))
        moved->parent = parent;

    // Removing the node first shifts every later position down by one, so an
    // index taken from the list the user was looking at means one place too far
    // to the right once the node has left it.
    int at = index;
    if (sameParent && from >= 0 && index > from)
        at = index - 1;
    insertChildLocked(parent, id, at);

    markBookmarksDirtyLocked();
    bumpRevisionLocked();
}

int bookmarksNodeForURLLocked(const std::string& url)
{
    if (url.empty())
        return 0;
    for (const auto& node : g_nodes) {
        if (!node.folder && node.url == url)
            return node.id;
    }
    return 0;
}

int bookmarksDepthLocked(int id)
{
    int depth = 0;
    const BookmarkNode* node = bookmarkNodeLocked(id);
    while (node && node->parent > 0) {
        depth += 1;
        node = bookmarkNodeLocked(node->parent);
    }
    return depth;
}

int bookmarksLeafCountLocked(int id)
{
    const BookmarkNode* node = bookmarkNodeLocked(id);
    if (!node)
        return 0;
    if (!node->folder)
        return 1;

    int count = 0;
    const std::vector<int> children = node->children;
    for (int child : children)
        count += bookmarksLeafCountLocked(child);
    return count;
}

} // namespace harmony::bookmarks

// --- The host's frame thread reads and edits the tree from here ---------------

using namespace harmony::bookmarks;

extern "C" int hb_bookmarks_child_count(int folder_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    bookmarksEnsureRootsLocked();
    const BookmarkNode* node = bookmarkNodeLocked(folder_id);
    return node ? static_cast<int>(node->children.size()) : 0;
}

extern "C" int hb_bookmarks_child_at(int folder_id, int index)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    bookmarksEnsureRootsLocked();
    const BookmarkNode* node = bookmarkNodeLocked(folder_id);
    if (!node || index < 0 || index >= static_cast<int>(node->children.size()))
        return 0;
    return node->children[static_cast<size_t>(index)];
}

extern "C" int hb_bookmarks_parent_of(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node ? node->parent : 0;
}

extern "C" int hb_bookmarks_index_of(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    if (!node)
        return -1;
    return childIndexLocked(node->parent, node_id);
}

extern "C" int hb_bookmarks_is_folder(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node && node->folder ? 1 : 0;
}

extern "C" int hb_bookmarks_depth(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return bookmarksDepthLocked(node_id);
}

extern "C" int hb_bookmarks_leaf_count(int folder_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return bookmarksLeafCountLocked(folder_id);
}

extern "C" const char* hb_bookmarks_title(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node ? answer(node->title) : "";
}

extern "C" const char* hb_bookmarks_url(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node ? answer(node->url) : "";
}

extern "C" const char* hb_bookmarks_host(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node ? answer(node->host) : "";
}

extern "C" double hb_bookmarks_added(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    const BookmarkNode* node = bookmarkNodeLocked(node_id);
    return node ? node->added : 0.0;
}

extern "C" int hb_bookmarks_node_for_url(const char* url)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return bookmarksNodeForURLLocked(url ? url : "");
}

extern "C" int hb_bookmarks_add(int parent_id, int index, const char* url, const char* title)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return bookmarksAddLocked(parent_id, index, url ? url : "", title ? title : "", false);
}

extern "C" int hb_bookmarks_add_folder(int parent_id, int index, const char* title)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    return bookmarksAddLocked(parent_id, index, std::string(), title ? title : "", true);
}

extern "C" void hb_bookmarks_remove(int node_id)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    bookmarksRemoveLocked(node_id);
}

extern "C" void hb_bookmarks_rename(int node_id, const char* title)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    BookmarkNode* node = bookmarkNodeLocked(node_id);
    if (!node)
        return;

    const std::string next = title ? title : "";
    if (node->title == next)
        return;

    node->title = next;
    node->foldedTitle = foldCase(next);
    markBookmarksDirtyLocked();
    bumpRevisionLocked();
}

extern "C" void hb_bookmarks_set_url(int node_id, const char* url)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    BookmarkNode* node = bookmarkNodeLocked(node_id);
    if (!node || node->folder)
        return;

    const std::string next = url ? url : "";
    if (next.empty() || node->url == next)
        return;

    node->url = next;
    node->host = hostOfURL(next);
    node->foldedURL = matchableURL(next);
    markBookmarksDirtyLocked();
    bumpRevisionLocked();
}

extern "C" void hb_bookmarks_move(int node_id, int parent_id, int index)
{
    std::lock_guard<std::mutex> lock(modelMutex());
    bookmarksMoveLocked(node_id, parent_id, index);
}

extern "C" int hb_bookmarks_toggle(const char* url, const char* title)
{
    const std::string address = url ? url : "";
    if (address.empty())
        return 0;

    std::lock_guard<std::mutex> lock(modelMutex());
    bookmarksEnsureRootsLocked();

    // Every node for the address goes, not only the first: the same page can be
    // filed in two folders, and a star that unfiled one of them would go dark
    // over a page that is still bookmarked.
    bool removed = false;
    for (;;) {
        const int existing = bookmarksNodeForURLLocked(address);
        if (!existing)
            break;
        bookmarksRemoveLocked(existing);
        removed = true;
    }
    if (removed)
        return 0;

    // A page with no title of its own is filed under the site it is on, which is
    // what the row would have been named anyway.
    std::string label = title ? title : "";
    if (label.empty())
        label = hostOfURL(address);
    if (label.empty())
        label = address;

    return bookmarksAddLocked(HB_BOOKMARK_ROOT_BAR, -1, address, label, false) ? 1 : 0;
}
