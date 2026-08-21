#include "harmony_menus_internal.h"

#include "harmony_tabs.h"

// What is on the menu.
//
// WebKit proposes what a RENDERING ENGINE knows about: the spelling of the word
// under the pointer, the direction text runs in, whether a video is muted. It
// does not propose anything that is about the BROWSER, because it has no tabs, no
// download list and no address bar to propose them against.
//
// So the menu this browser hands back is its own items first -- the ones a person
// right-clicks a link for -- and then whatever WebKit proposed that none of them
// already does. A proposal this module replaces is dropped rather than left
// beside its replacement: a menu offering the same action twice under two names
// is a menu that has to be read before it can be used.

namespace harmony_menus {

namespace {

// WebKit's own tags, mirrored from `WKContextMenuItemTypes.h`. Only the ones this
// module replaces are named: everything else keeps whatever tag WebKit gave it
// and is handed straight back.
constexpr uint32_t kTagOpenLinkInNewWindow = 1;
constexpr uint32_t kTagDownloadLinkToDisk = 2;
constexpr uint32_t kTagCopyLinkToClipboard = 3;
constexpr uint32_t kTagOpenImageInNewWindow = 4;
constexpr uint32_t kTagDownloadImageToDisk = 5;
constexpr uint32_t kTagGoBack = 9;
constexpr uint32_t kTagGoForward = 10;
constexpr uint32_t kTagStop = 11;
constexpr uint32_t kTagReload = 12;
constexpr uint32_t kTagInspectElement = 57;
constexpr uint32_t kTagOpenMediaInNewWindow = 75;
constexpr uint32_t kTagDownloadMediaToDisk = 76;
constexpr uint32_t kTagCopyMediaLinkToClipboard = 77;
constexpr uint32_t kTagCopyImageURLToClipboard = 88;
constexpr uint32_t kTagSelectAll = 89;

// One menu under construction. The array carries the reference WebKit adopts;
// every item put into it is released here, because the array retains what it is
// given.
class MenuBuilder {
public:
    MenuBuilder(const WebKitApi& api, WKMutableArrayRef array)
        : m_api(api)
        , m_array(array)
    {
    }

    void addAction(uint32_t tag, const char* title, bool enabled = true)
    {
        WKStringRef text = m_api.stringCreateWithUTF8CString(title);
        if (!text)
            return;

        WKContextMenuItemRef item = m_api.contextMenuItemCreateAsAction(tag, text, enabled);
        releaseObject(text);
        addItem(item);
    }

    // A rule only ever separates two runs of items, so one asked for before
    // anything has been added, or twice in a row, draws nothing.
    void addSeparator()
    {
        if (m_empty || m_separatorPending)
            return;
        m_separatorPending = true;
    }

    // An item WebKit proposed, handed back as it is. The array retains it, and
    // the reference the proposal itself holds is WebKit's.
    void addProposal(WKContextMenuItemRef item)
    {
        if (!item)
            return;
        flushSeparator();
        m_api.arrayAppendItem(m_array, item);
        m_empty = false;
    }

private:
    void addItem(WKContextMenuItemRef item)
    {
        if (!item)
            return;
        flushSeparator();
        m_api.arrayAppendItem(m_array, item);
        releaseObject(item);
        m_empty = false;
    }

    void flushSeparator()
    {
        if (!m_separatorPending)
            return;
        m_separatorPending = false;

        WKContextMenuItemRef rule = m_api.contextMenuItemSeparatorItem();
        if (!rule)
            return;
        m_api.arrayAppendItem(m_array, rule);
        releaseObject(rule);
    }

    const WebKitApi& m_api;
    WKMutableArrayRef m_array { nullptr };
    bool m_empty { true };
    bool m_separatorPending { false };
};

void addLinkItems(MenuBuilder& menu)
{
    if (g_context.linkURL.empty())
        return;

    menu.addAction(TagOpenLinkInNewTab, "Open Link in New Tab");
    menu.addAction(TagOpenLinkInBackgroundTab, "Open Link in Background Tab");
    menu.addAction(TagSaveLinkAs, "Save Link As\xE2\x80\xA6");
    menu.addAction(TagCopyLinkAddress, "Copy Link Address");
}

void addImageItems(MenuBuilder& menu)
{
    if (g_context.imageURL.empty())
        return;

    menu.addSeparator();
    menu.addAction(TagOpenImageInNewTab, "Open Image in New Tab");
    menu.addAction(TagSaveImageAs, "Save Image As\xE2\x80\xA6");
    menu.addAction(TagCopyImageAddress, "Copy Image Address");
}

void addMediaItems(MenuBuilder& menu)
{
    if (g_context.mediaURL.empty())
        return;
    // A poster frame is reported as both, and the image group already offered
    // every one of these for it.
    if (g_context.mediaURL == g_context.imageURL)
        return;

    menu.addSeparator();
    menu.addAction(TagOpenMediaInNewTab, "Open Media in New Tab");
    menu.addAction(TagSaveMediaAs, "Save Media As\xE2\x80\xA6");
    menu.addAction(TagCopyMediaAddress, "Copy Media Address");
}

// Cut, Copy and Paste are WebKit's own proposals and stay WebKit's: only the web
// process knows whether there is a selection to cut and whether the field will
// take a paste, and an item this module enabled by guessing would be an item that
// looks available and does nothing.
//
// Select All is not proposed on this port at all, so it is built here.
void addEditingItems(MenuBuilder& menu)
{
    if (!g_context.editable)
        return;

    menu.addSeparator();
    menu.addAction(TagSelectAll, "Select All");
}

// Where the page has been. A browser offers these on the page itself rather than
// on a link or an image, which is exactly the case WebKit proposes them in -- and
// it proposes them only while they are available, so the row moves under the
// pointer as a load finishes. These stay in place and recede instead.
void addNavigationItems(MenuBuilder& menu)
{
    if (!g_context.linkURL.empty() || !g_context.imageURL.empty() || !g_context.mediaURL.empty())
        return;
    if (g_context.editable)
        return;

    const bool loading = hb_tabs_is_loading(g_context.tabId) != 0;

    menu.addSeparator();
    menu.addAction(TagBack, "Back", g_context.canGoBack);
    menu.addAction(TagForward, "Forward", g_context.canGoForward);
    menu.addAction(TagReload, "Reload");
    menu.addAction(TagStop, "Stop", loading);
}

// What the menu says about the document itself, wherever it was raised.
void addPageItems(MenuBuilder& menu)
{
    menu.addSeparator();
    menu.addAction(TagCopyPageAddress, "Copy Page Address", !g_context.pageURL.empty());
    menu.addAction(TagViewPageSource, "View Page Source");
    if (inspectorIsAvailable())
        menu.addAction(TagInspectElement, "Inspect Element");
}

void addRemainingProposals(const WebKitApi& api, MenuBuilder& menu, WKArrayRef proposedMenu)
{
    if (!proposedMenu)
        return;

    const size_t count = api.arrayGetSize(proposedMenu);
    bool separated = false;

    for (size_t index = 0; index < count; ++index) {
        WKContextMenuItemRef item = api.arrayGetItemAtIndex(proposedMenu, index);
        if (!item)
            continue;

        const uint32_t tag = api.contextMenuItemGetTag(item);
        if (isSupersededProposal(tag))
            continue;
        // A rule WebKit drew between two runs it proposed is a rule between two
        // runs that may no longer both be there.
        if (tag == 0)
            continue;

        if (!separated) {
            separated = true;
            menu.addSeparator();
        }
        menu.addProposal(item);
    }
}

} // namespace

bool isSupersededProposal(uint32_t tag)
{
    switch (tag) {
    case kTagOpenLinkInNewWindow:
    case kTagDownloadLinkToDisk:
    case kTagCopyLinkToClipboard:
    case kTagOpenImageInNewWindow:
    case kTagDownloadImageToDisk:
    case kTagCopyImageURLToClipboard:
    case kTagOpenMediaInNewWindow:
    case kTagDownloadMediaToDisk:
    case kTagCopyMediaLinkToClipboard:
    case kTagGoBack:
    case kTagGoForward:
    case kTagStop:
    case kTagReload:
    case kTagSelectAll:
    case kTagInspectElement:
        return true;
    default:
        return false;
    }
}

WKMutableArrayRef buildMenu(WKArrayRef proposedMenu)
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return nullptr;

    WKMutableArrayRef array = api->mutableArrayCreate();
    if (!array)
        return nullptr;

    MenuBuilder menu(*api, array);
    addLinkItems(menu);
    addImageItems(menu);
    addMediaItems(menu);
    addEditingItems(menu);
    addNavigationItems(menu);
    addRemainingProposals(*api, menu, proposedMenu);
    addPageItems(menu);
    return array;
}

} // namespace harmony_menus
