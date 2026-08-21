#include "harmony_input_internal.h"

// Find in page.
//
// WebKit does the searching, the scrolling and the highlighting: a find bar is
// a text field, a direction and a count. What is here is the seam -- the query
// posted to the engine thread, and the answers WebKit reports back to the find
// client, published for the frame thread to draw.
//
// Which match is selected is counted here rather than read from WebKit, which
// reports how many matches a page holds and not which of them it just scrolled
// to. Counting it is exact as long as the count is: each search moves one match
// in the direction it was asked for, and a search that fails moves nothing.

namespace harmony_input {

namespace {

std::mutex g_findMutex;
std::string g_query;

std::atomic<int> g_matchCount { 0 };
std::atomic<int> g_matchIndex { 0 };
std::atomic<bool> g_searched { false };

// What the search in flight was asked to do, read when its answer arrives.
std::atomic<bool> g_pendingRepeat { false };
std::atomic<int> g_pendingDirection { 1 };

struct FindRequest {
    int tabId { 0 };
    std::string text;
    unsigned options { kFindOptionsForward };
    bool hide { false };
};

int tabOfClient(const void* clientInfo)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(clientInfo));
}

void advanceMatchIndex(int matchCount)
{
    if (matchCount <= 0) {
        g_matchIndex.store(0);
        return;
    }

    if (!g_pendingRepeat.load()) {
        g_matchIndex.store(1);
        return;
    }

    const int direction = g_pendingDirection.load();
    int index = g_matchIndex.load() + (direction < 0 ? -1 : 1);
    if (index > matchCount)
        index = 1;
    if (index < 1)
        index = matchCount;
    g_matchIndex.store(index);
}

// --- The find client, on the engine thread ----------------------------------

void didFindString(WKPageRef, WKStringRef, unsigned matchCount, const void* clientInfo)
{
    if (tabOfClient(clientInfo) != g_activeTabId.load())
        return;

    const int count = static_cast<int>(matchCount);
    g_matchCount.store(count);
    advanceMatchIndex(count);
    g_searched.store(true);
    bumpRevision();
}

void didFailToFindString(WKPageRef, WKStringRef, const void* clientInfo)
{
    if (tabOfClient(clientInfo) != g_activeTabId.load())
        return;

    g_matchCount.store(0);
    g_matchIndex.store(0);
    g_searched.store(true);
    bumpRevision();
}

void didCountStringMatches(WKPageRef, WKStringRef, unsigned matchCount, const void* clientInfo)
{
    if (tabOfClient(clientInfo) != g_activeTabId.load())
        return;

    g_matchCount.store(static_cast<int>(matchCount));
    bumpRevision();
}

void runFind(void* context)
{
    auto* request = static_cast<FindRequest*>(context);
    if (!request)
        return;

    if (const WebKitApi* api = webKitApi()) {
        if (WKPageRef page = hb_tabs_page(request->tabId)) {
            if (request->hide)
                api->pageHideFindUI(page);
            else if (WKStringRef text = createString(request->text)) {
                api->pageFindString(page, text, request->options, kFindMaxMatchCount);
                releaseObject(text);
            }
        }
    }
    delete request;
}

void postFind(const std::string& text, bool backwards, bool hide)
{
    const int tab = g_activeTabId.load();
    if (tab <= 0)
        return;

    auto* request = new FindRequest;
    request->tabId = tab;
    request->text = text;
    request->options = backwards ? kFindOptionsBackward : kFindOptionsForward;
    request->hide = hide;
    hb_tabs_invoke_on_webkit_thread(runFind, request);
}

} // namespace

void findOpen()
{
    if (!g_findOpen.exchange(true))
        bumpRevision();
}

void findClose()
{
    if (!g_findOpen.exchange(false))
        return;

    {
        std::lock_guard<std::mutex> lock(g_findMutex);
        g_query.clear();
    }
    g_matchCount.store(0);
    g_matchIndex.store(0);
    g_searched.store(false);
    postFind(std::string(), false, true);
    bumpRevision();
}

void findSearch(const std::string& text, bool backwards)
{
    std::string previous;
    {
        std::lock_guard<std::mutex> lock(g_findMutex);
        previous = g_query;
        g_query = text;
    }

    if (text.empty()) {
        g_matchCount.store(0);
        g_matchIndex.store(0);
        g_searched.store(false);
        postFind(std::string(), false, true);
        bumpRevision();
        return;
    }

    // Typing into the field restarts the search at the first match; asking for
    // the same text again is what walks the matches.
    g_pendingRepeat.store(text == previous);
    g_pendingDirection.store(backwards ? -1 : 1);
    postFind(text, backwards, false);
}

int findMatchCount()
{
    return g_matchCount.load();
}

int findMatchIndex()
{
    return g_matchIndex.load();
}

bool findSearched()
{
    return g_searched.load();
}

void attachFindClient(int tabId, WKPageRef page)
{
    const WebKitApi* api = webKitApi();
    if (!api || !page)
        return;

    FindClientV0 client { };
    client.version = 0;
    client.clientInfo = reinterpret_cast<const void*>(static_cast<intptr_t>(tabId));
    client.didFindString = didFindString;
    client.didFailToFindString = didFailToFindString;
    client.didCountStringMatches = didCountStringMatches;
    api->pageSetPageFindClient(page, &client);
}

void detachFindClient(int, WKPageRef page)
{
    const WebKitApi* api = webKitApi();
    if (!api || !page)
        return;
    api->pageSetPageFindClient(page, nullptr);
}

} // namespace harmony_input

// --- The host's surface -----------------------------------------------------

using namespace harmony_input;

extern "C" void hb_input_find_open(void)
{
    findOpen();
}

extern "C" void hb_input_find_close(void)
{
    findClose();
}

extern "C" int hb_input_find_is_open(void)
{
    return g_findOpen.load() ? 1 : 0;
}

extern "C" void hb_input_find_search(const char* text, int backwards)
{
    findSearch(text ? std::string(text) : std::string(), backwards != 0);
}

extern "C" int hb_input_find_match_count(void)
{
    return findMatchCount();
}

extern "C" int hb_input_find_match_index(void)
{
    return findMatchIndex();
}

extern "C" int hb_input_find_searched(void)
{
    return findSearched() ? 1 : 0;
}
