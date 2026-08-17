#include "harmony_menus_internal.h"

#include "harmony_tabs.h"
#include "harmony_tabs_embed.h"

// The engine seam: the entry points this module calls, and the one client it
// installs.
//
// A page carries ONE context-menu client, and no other system of this browser
// wants one -- the registry owns the UI, navigation and state clients, and the
// input system owns the find client. So this module installs its own, from the
// registry's page observer, exactly as the find client is installed: the page is
// alive, no load has started on it, and nothing else can be replaced by it.

namespace harmony_menus {

namespace {

WebKitApi g_api;
bool g_usable { false };

std::atomic<bool> g_attached { false };

template<typename Function>
bool resolve(const char* name, Function& function, std::string& missing)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
    if (function)
        return true;

    if (!missing.empty())
        missing += ", ";
    missing += name;
    return false;
}

template<typename Function>
void resolveOptional(const char* name, Function& function)
{
    function = reinterpret_cast<Function>(hb_tabs_webkit_symbol(name));
}

bool resolveApi()
{
    // The tabs registry owns the runtime, so its symbol seam is what answers
    // here: this module has no engine of its own to load, and looking the module
    // up by name would be a second discovery order to keep in step.
    if (!hb_tabs_webkit_symbol("WKRelease"))
        return false;

    WebKitApi api;
    std::string missing;
    bool ok = true;
    ok &= resolve("WKPageSetPageContextMenuClient", api.pageSetPageContextMenuClient, missing);
    ok &= resolve("WKHitTestResultCopyAbsoluteLinkURL", api.hitTestResultCopyAbsoluteLinkURL, missing);
    ok &= resolve("WKHitTestResultCopyAbsoluteImageURL", api.hitTestResultCopyAbsoluteImageURL, missing);
    ok &= resolve("WKHitTestResultCopyAbsoluteMediaURL", api.hitTestResultCopyAbsoluteMediaURL, missing);
    ok &= resolve("WKHitTestResultIsContentEditable", api.hitTestResultIsContentEditable, missing);
    ok &= resolve("WKContextMenuItemCreateAsAction", api.contextMenuItemCreateAsAction, missing);
    ok &= resolve("WKContextMenuItemSeparatorItem", api.contextMenuItemSeparatorItem, missing);
    ok &= resolve("WKContextMenuItemGetTag", api.contextMenuItemGetTag, missing);
    ok &= resolve("WKMutableArrayCreate", api.mutableArrayCreate, missing);
    ok &= resolve("WKArrayAppendItem", api.arrayAppendItem, missing);
    ok &= resolve("WKArrayGetSize", api.arrayGetSize, missing);
    ok &= resolve("WKArrayGetItemAtIndex", api.arrayGetItemAtIndex, missing);
    ok &= resolve("WKPageExecuteCommand", api.pageExecuteCommand, missing);
    ok &= resolve("WKPageGetMainFrame", api.pageGetMainFrame, missing);
    ok &= resolve("WKPageGetSourceForFrame", api.pageGetSourceForFrame, missing);
    ok &= resolve("WKPageCanGoBack", api.pageCanGoBack, missing);
    ok &= resolve("WKPageCanGoForward", api.pageCanGoForward, missing);
    ok &= resolve("WKStringCreateWithUTF8CString", api.stringCreateWithUTF8CString, missing);
    ok &= resolve("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize, missing);
    ok &= resolve("WKStringGetUTF8CStringNonStrict", api.stringGetUTF8CStringNonStrict, missing);
    ok &= resolve("WKURLCopyString", api.urlCopyString, missing);
    ok &= resolve("WKRetain", api.retain, missing);
    ok &= resolve("WKRelease", api.release, missing);

    resolveOptional("WKPageGetInspector", api.pageGetInspector);
    resolveOptional("WKInspectorShow", api.inspectorShow);
    resolveOptional("WKPageCopyPageConfiguration", api.pageCopyPageConfiguration);
    resolveOptional("WKPageConfigurationGetPreferences", api.pageConfigurationGetPreferences);
    resolveOptional("WKPreferencesSetDeveloperExtrasEnabled", api.preferencesSetDeveloperExtrasEnabled);

    if (!ok) {
        setError("this WebKit build does not export " + missing);
        return false;
    }

    g_api = api;
    return true;
}

// --- The client ---------------------------------------------------------------
//
// `WKPageContextMenuClientV2`, field for field. Version 2 is the first that hands
// the hit test to the menu builder, and the last this browser needs: version 3's
// two fields exist for a client that draws the menu itself, and the Windows port
// draws a real popup menu from the items handed back instead.
//
// Every member after the base is pointer-sized, so the mirror is exact as long as
// the field count is; the size is asserted below.

struct ContextMenuClientBase {
    int version;
    const void* clientInfo;
};

struct ContextMenuClientV2 {
    ContextMenuClientBase base;

    void* getContextMenuFromProposedMenu_deprecatedForUseWithV0;
    void* customContextMenuItemSelected;
    void* contextMenuDismissed;
    void* getContextMenuFromProposedMenu;
};

static_assert(
    sizeof(ContextMenuClientV2) == sizeof(ContextMenuClientBase) + 4 * sizeof(void*),
    "the context menu client mirror has gained or lost a field"
);

using GetContextMenuFromProposedMenuCallback = void (*)(
    WKPageRef,
    WKArrayRef proposedMenu,
    WKArrayRef* newMenu,
    WKHitTestResultRef,
    WKTypeRef userData,
    const void* clientInfo
);
using CustomContextMenuItemSelectedCallback = void (*)(WKPageRef, WKContextMenuItemRef, const void* clientInfo);
using ContextMenuDismissedCallback = void (*)(WKPageRef, const void* clientInfo);

int tabOfClient(const void* clientInfo)
{
    return static_cast<int>(reinterpret_cast<intptr_t>(clientInfo));
}

std::string urlOfHitTest(WKURLRef (*copy)(WKHitTestResultRef), WKHitTestResultRef hitTest)
{
    WKURLRef url = copy(hitTest);
    std::string text = textOfURL(url);
    releaseObject(url);
    return text;
}

void readContext(int tabId, WKPageRef page, WKHitTestResultRef hitTest)
{
    g_context = MenuContext { };
    g_context.tabId = tabId;
    g_context.page = page;
    g_context.pageURL = hb_tabs_url(tabId);
    g_context.canGoBack = g_api.pageCanGoBack(page);
    g_context.canGoForward = g_api.pageCanGoForward(page);

    if (!hitTest)
        return;

    g_context.linkURL = urlOfHitTest(g_api.hitTestResultCopyAbsoluteLinkURL, hitTest);
    g_context.imageURL = urlOfHitTest(g_api.hitTestResultCopyAbsoluteImageURL, hitTest);
    g_context.mediaURL = urlOfHitTest(g_api.hitTestResultCopyAbsoluteMediaURL, hitTest);
    g_context.editable = g_api.hitTestResultIsContentEditable(hitTest);
}

void getContextMenuFromProposedMenu(
    WKPageRef page,
    WKArrayRef proposedMenu,
    WKArrayRef* newMenu,
    WKHitTestResultRef hitTest,
    WKTypeRef,
    const void* clientInfo
) {
    if (!newMenu)
        return;
    *newMenu = nullptr;

    const int tabId = tabOfClient(clientInfo);
    if (tabId <= 0 || !webKitApi())
        return;

    readContext(tabId, page, hitTest);

    // WebKit adopts the array it is handed, so the one reference this creates is
    // the one it releases.
    *newMenu = buildMenu(proposedMenu);
}

void customContextMenuItemSelected(WKPageRef page, WKContextMenuItemRef item, const void* clientInfo)
{
    if (!webKitApi() || !item)
        return;
    // The menu that was up is the one this answers for: WebKit shows one at a
    // time, and the context was read when that one was built.
    if (g_context.tabId != tabOfClient(clientInfo) || g_context.page != page)
        return;

    runTag(g_api.contextMenuItemGetTag(item));
}

void contextMenuDismissed(WKPageRef, const void*)
{
}

// Developer extras are what put an inspector behind a page. They are this
// module's to set because the inspector item is this module's to offer: the
// settings module writes the preferences a person chooses, and this is not one of
// them.
void enableDeveloperExtras(WKPageRef page)
{
    if (!g_api.pageCopyPageConfiguration || !g_api.pageConfigurationGetPreferences || !g_api.preferencesSetDeveloperExtrasEnabled)
        return;

    WKPageConfigurationRef configuration = g_api.pageCopyPageConfiguration(page);
    if (!configuration)
        return;

    if (WKPreferencesRef preferences = g_api.pageConfigurationGetPreferences(configuration)) {
        (void)g_api.retain(preferences);
        g_api.preferencesSetDeveloperExtrasEnabled(preferences, true);
        releaseObject(preferences);
    }
    releaseObject(configuration);
}

void installClient(int tabId, WKPageRef page)
{
    if (!webKitApi() || !page)
        return;

    ContextMenuClientV2 client { };
    client.base.version = 2;
    client.base.clientInfo = reinterpret_cast<const void*>(static_cast<intptr_t>(tabId));
    client.getContextMenuFromProposedMenu = reinterpret_cast<void*>(
        static_cast<GetContextMenuFromProposedMenuCallback>(getContextMenuFromProposedMenu)
    );
    client.customContextMenuItemSelected = reinterpret_cast<void*>(
        static_cast<CustomContextMenuItemSelectedCallback>(customContextMenuItemSelected)
    );
    client.contextMenuDismissed = reinterpret_cast<void*>(
        static_cast<ContextMenuDismissedCallback>(contextMenuDismissed)
    );

    g_api.pageSetPageContextMenuClient(page, &client);

    if (inspectorIsAvailable())
        enableDeveloperExtras(page);
}

void detachClient(WKPageRef page)
{
    if (!webKitApi() || !page)
        return;
    g_api.pageSetPageContextMenuClient(page, nullptr);
}

void pageCreated(int tabId, void* page, void*)
{
    installClient(tabId, page);
}

void pageDestroying(int tabId, void* page, void*)
{
    if (g_context.tabId == tabId)
        g_context = MenuContext { };
    detachClient(page);
}

// Installs the client on every tab that already exists. A module that only ever
// wrote into new pages would leave a browser attached late with a page that
// answers a right-click with WebKit's own menu and none of this browser's.
void installOnExistingTabs(void*)
{
    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int tabId = hb_tabs_id_at(index);
        if (tabId <= 0)
            continue;
        installClient(tabId, hb_tabs_page(tabId));
    }
}

void engineTeardown(void*)
{
    g_context = MenuContext { };
}

} // namespace

MenuContext g_context;

const WebKitApi* webKitApi()
{
    // Only success is remembered. The first ask can arrive before the registry
    // has finished loading the engine, and latching that would leave every page
    // of this run without a browser menu because the question was asked one
    // cycle too early.
    if (!g_usable)
        g_usable = resolveApi();
    return g_usable ? &g_api : nullptr;
}

void releaseObject(WKTypeRef object)
{
    if (object && g_usable && g_api.release)
        g_api.release(object);
}

WKStringRef createString(const std::string& text)
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return nullptr;
    return api->stringCreateWithUTF8CString(text.c_str());
}

std::string textOfString(WKStringRef value)
{
    const WebKitApi* api = webKitApi();
    if (!api || !value)
        return { };

    const size_t capacity = api->stringGetMaximumUTF8CStringSize(value);
    if (capacity < 2)
        return { };

    std::string text(capacity, '\0');
    const size_t written = api->stringGetUTF8CStringNonStrict(value, &text[0], capacity);
    if (!written)
        return { };

    // The count includes the terminator WebKit wrote, which a `std::string`
    // carries for itself.
    text.resize(written - 1);
    return text;
}

std::string textOfURL(WKURLRef url)
{
    const WebKitApi* api = webKitApi();
    if (!api || !url)
        return { };

    WKStringRef text = api->urlCopyString(url);
    std::string result = textOfString(text);
    releaseObject(text);
    return result;
}

bool inspectorIsAvailable()
{
    const WebKitApi* api = webKitApi();
    if (!api)
        return false;
    return api->pageGetInspector && api->inspectorShow && api->preferencesSetDeveloperExtrasEnabled;
}

} // namespace harmony_menus

using namespace harmony_menus;

extern "C" void hb_menus_attach(void)
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
    hb_tabs_add_teardown_hook(engineTeardown, nullptr);
    hb_tabs_invoke_on_webkit_thread(installOnExistingTabs, nullptr);
}

extern "C" void hb_menus_shutdown(void)
{
    hb_tabs_invoke_on_webkit_thread(engineTeardown, nullptr);
    discardPageSources();
}
