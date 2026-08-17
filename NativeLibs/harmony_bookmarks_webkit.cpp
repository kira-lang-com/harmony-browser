#include "harmony_bookmarks_internal.h"
#include "harmony_bookmarks_host.h"

#include "harmony_data_store.h"
#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

#include <algorithm>
#include <atomic>
#include <cstddef>

// How the history hears that a page arrived.
//
// Not from a navigation client. A page carries one, the tabs registry owns it,
// and its did-commit field is already the navigation model's: a second module
// writing the same field would take the report away from the first, and the
// address bar would go quiet the moment the history started working.
//
// The registry's page-state observer is the seam built for exactly this. Every
// registered observer is called for every change and none of them can take a
// report away from another, and what it hands over -- the page, on the WebKit
// thread -- is enough to ask WebKit the only question that matters: which
// document is committed right now. A document that is committed and is not the
// one this tab last recorded is a visit, whether it arrived by a load, by a
// redirect or by a script pushing a new address onto the same document.
//
// Nothing is recorded for a private tab. Which tabs those are is the profile's
// answer, asked at the moment the page reports, because a page opened with
// window.open from a private tab is private too and a list of our own could not
// have known that.

namespace harmony::bookmarks {

namespace {

using WKTypeRef = const void*;
using WKPageRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;

// The slice of WebKit's C API this module calls, declared here rather than
// included from the engine's headers: the engine is loaded at run time and its
// checkout is not required to build this package.
struct WebKitApi {
    WKURLRef (*pageCopyCommittedURL)(WKPageRef) { nullptr };
    WKStringRef (*pageCopyTitle)(WKPageRef) { nullptr };
    WKStringRef (*urlCopyString)(WKURLRef) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CStringNonStrict)(WKStringRef, char*, size_t) { nullptr };
    void (*release)(WKTypeRef) { nullptr };
};

WebKitApi g_api;
bool g_usable { false };
std::atomic<bool> g_attached { false };

// One tab, as the WebKit thread knows it: the document it last recorded, so a
// title arriving seconds later knows which entry it belongs to, and a page
// reporting its progress a hundred times does not record a hundred visits.
struct TabRecord {
    int tabId { 0 };
    std::string committedURL;
    std::string recordedTitle;
};

std::vector<TabRecord> g_records;

template<typename Function>
bool resolve(const char* name, Function& function)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
    return function != nullptr;
}

const WebKitApi* webKitApi()
{
    // Only success is remembered. The first ask can arrive before the registry
    // has finished loading the engine, and latching that would leave the history
    // recording nothing for the rest of the run because it was asked one cycle
    // too early.
    if (g_usable)
        return &g_api;

    if (!hb_tabs_webkit_symbol("WKRelease"))
        return nullptr;

    WebKitApi api;
    bool ok = true;
    ok &= resolve("WKPageCopyCommittedURL", api.pageCopyCommittedURL);
    ok &= resolve("WKPageCopyTitle", api.pageCopyTitle);
    ok &= resolve("WKURLCopyString", api.urlCopyString);
    ok &= resolve("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize);
    ok &= resolve("WKStringGetUTF8CStringNonStrict", api.stringGetUTF8CStringNonStrict);
    ok &= resolve("WKRelease", api.release);
    if (!ok) {
        setError("this WebKit build does not export what a browsing history is read from");
        return nullptr;
    }

    g_api = api;
    g_usable = true;
    return &g_api;
}

void releaseObject(WKTypeRef object)
{
    if (object && g_usable && g_api.release)
        g_api.release(object);
}

std::string textOfString(WKStringRef value)
{
    if (!g_usable || !value)
        return { };

    const size_t capacity = g_api.stringGetMaximumUTF8CStringSize(value);
    if (capacity < 2)
        return { };

    std::string text(capacity, '\0');
    const size_t written = g_api.stringGetUTF8CStringNonStrict(value, &text[0], capacity);
    if (!written)
        return { };

    // The count includes the terminator WebKit wrote, which a `std::string`
    // carries for itself.
    text.resize(written - 1);
    return text;
}

std::string committedURLOf(WKPageRef page)
{
    const WebKitApi* api = webKitApi();
    if (!api || !page)
        return { };

    WKURLRef url = api->pageCopyCommittedURL(page);
    if (!url)
        return { };

    WKStringRef text = api->urlCopyString(url);
    std::string result = textOfString(text);
    releaseObject(text);
    releaseObject(url);
    return result;
}

std::string titleOf(WKPageRef page)
{
    const WebKitApi* api = webKitApi();
    if (!api || !page)
        return { };

    WKStringRef title = api->pageCopyTitle(page);
    std::string result = textOfString(title);
    releaseObject(title);
    return result;
}

TabRecord& recordFor(int tabId)
{
    for (auto& record : g_records) {
        if (record.tabId == tabId)
            return record;
    }
    g_records.push_back(TabRecord { tabId, std::string(), std::string() });
    return g_records.back();
}

void forgetRecord(int tabId)
{
    g_records.erase(
        std::remove_if(g_records.begin(), g_records.end(), [tabId](const TabRecord& record) {
            return record.tabId == tabId;
        }),
        g_records.end()
    );
}

// --- The registry's seams ---------------------------------------------------

// Runs on the WebKit thread whenever WebKit reports that a page's load state,
// title, address or back/forward availability moved.
void pageStateChanged(int tabId, void* page, int field, void*)
{
    if (tabId <= 0 || !page)
        return;

    // Progress moves many times a second and says nothing about which document
    // is committed. Answering it would ask WebKit for two strings per frame of
    // every load, for a history that cannot have changed.
    if (field == HB_TABS_PAGE_STATE_PROGRESS)
        return;

    // The store the tab browses in is the profile's answer and can only be given
    // once the page exists, which is why it is asked here rather than kept.
    if (hb_data_store_tab_is_private(tabId))
        return;

    const std::string committed = committedURLOf(page);
    if (committed.empty() || !isRecordableURL(committed))
        return;

    const std::string title = titleOf(page);
    TabRecord& record = recordFor(tabId);

    if (record.committedURL != committed) {
        record.committedURL = committed;
        record.recordedTitle = title;

        std::lock_guard<std::mutex> lock(modelMutex());
        historyRecordLocked(committed, title, unixNow());
        return;
    }

    // The same document, named at last. WebKit reports a commit before the
    // document says what it is called, so this is where most rows get their
    // name.
    if (title.empty() || title == record.recordedTitle)
        return;

    record.recordedTitle = title;
    std::lock_guard<std::mutex> lock(modelMutex());
    historyRetitleLocked(committed, title);
}

void pageCreated(int tabId, void*, void*)
{
    if (tabId > 0)
        recordFor(tabId);
}

void pageDestroying(int tabId, void*, void*)
{
    forgetRecord(tabId);
}

// Runs on the WebKit thread, once per run-loop cycle, before WebKit's own. The
// files are read here on the first cycle the profile has a directory to be read
// from, and written from here once a change to either model has settled.
void engineCycle(void*)
{
    filesPump();
}

void engineTeardown(void*)
{
    g_records.clear();
    filesFlush();
}

} // namespace
} // namespace harmony::bookmarks

using namespace harmony::bookmarks;

// --- The engine seam ----------------------------------------------------------

extern "C" void hb_bookmarks_attach(void)
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
    hb_tabs_add_page_state_observer(pageStateChanged, nullptr);
    hb_tabs_add_cycle_hook(engineCycle, nullptr);
    hb_tabs_add_teardown_hook(engineTeardown, nullptr);
}

extern "C" void hb_bookmarks_shutdown(void)
{
    // The last write happens on the caller's thread rather than the engine's:
    // the host calls this as the process ends, and a write queued onto a run
    // loop that is about to stop is a write that never happens.
    filesFlush();
    filesStop();
}
