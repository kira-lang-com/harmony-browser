#include "harmony_menus_internal.h"

#include <cstring>

// What a menu item does.
//
// Nothing here IMPLEMENTS a browser action. Opening a tab belongs to the tabs
// registry, saving a file to the download engine, and going back to the
// navigation model, and each of those has a Kira half that the rest of this
// browser's interface already drives. So an item that names one of them is
// queued as a command and dispatched by the host, exactly as a keyboard
// accelerator is: the menu and the button that does the same thing cannot
// disagree, because they end up in the same call.
//
// What IS carried out here is what only the engine can do -- an edit command in
// the page, the inspector, the page's own source -- and what only Windows can do,
// which is the clipboard.

namespace harmony_menus {

namespace {

void executeCommand(const char* command)
{
    const WebKitApi* api = webKitApi();
    if (!api || !g_context.page)
        return;

    WKStringRef text = createString(command);
    if (!text)
        return;
    api->pageExecuteCommand(g_context.page, text);
    releaseObject(text);
}

void showInspector()
{
    const WebKitApi* api = webKitApi();
    if (!api || !g_context.page || !inspectorIsAvailable())
        return;

    WKInspectorRef inspector = api->pageGetInspector(g_context.page);
    if (!inspector) {
        setError("this page has no inspector to open");
        return;
    }
    api->inspectorShow(inspector);
}

void openInNewTab(const std::string& url, bool foreground)
{
    if (url.empty())
        return;
    queueCommand(
        foreground ? HB_MENU_COMMAND_OPEN_IN_NEW_TAB : HB_MENU_COMMAND_OPEN_IN_BACKGROUND_TAB,
        url,
        g_context.tabId
    );
}

void saveAs(const std::string& url)
{
    if (url.empty())
        return;
    queueCommand(HB_MENU_COMMAND_SAVE_AS, url, g_context.tabId);
}

void navigate(int kind)
{
    queueCommand(kind, std::string(), g_context.tabId);
}

} // namespace

void copyToClipboard(const std::string& text)
{
    if (text.empty())
        return;

    const std::wstring wide = widen(text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);

    // The clipboard takes ownership of the handle once SetClipboardData
    // succeeds, so the allocation is only freed on the paths where it does not.
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!handle) {
        setError("the clipboard could not be given the text");
        return;
    }

    auto* buffer = static_cast<wchar_t*>(GlobalLock(handle));
    if (!buffer) {
        GlobalFree(handle);
        setError("the clipboard could not be given the text");
        return;
    }
    memcpy(buffer, wide.c_str(), bytes);
    GlobalUnlock(handle);

    // The page's own window owns the clipboard for the moment it is open: this
    // runs on the engine thread, inside the menu's own message loop, and that is
    // the window the menu belongs to.
    if (!OpenClipboard(nullptr)) {
        GlobalFree(handle);
        setError("the clipboard could not be opened");
        return;
    }

    if (!EmptyClipboard() || !SetClipboardData(CF_UNICODETEXT, handle)) {
        GlobalFree(handle);
        setError("the clipboard would not take the text");
    }
    CloseClipboard();
}

void runTag(uint32_t tag)
{
    switch (tag) {
    case TagOpenLinkInNewTab:
        openInNewTab(g_context.linkURL, true);
        return;
    case TagOpenLinkInBackgroundTab:
        openInNewTab(g_context.linkURL, false);
        return;
    case TagCopyLinkAddress:
        copyToClipboard(g_context.linkURL);
        return;
    case TagSaveLinkAs:
        saveAs(g_context.linkURL);
        return;

    case TagOpenImageInNewTab:
        openInNewTab(g_context.imageURL, true);
        return;
    case TagSaveImageAs:
        saveAs(g_context.imageURL);
        return;
    case TagCopyImageAddress:
        copyToClipboard(g_context.imageURL);
        return;

    case TagOpenMediaInNewTab:
        openInNewTab(g_context.mediaURL, true);
        return;
    case TagSaveMediaAs:
        saveAs(g_context.mediaURL);
        return;
    case TagCopyMediaAddress:
        copyToClipboard(g_context.mediaURL);
        return;

    case TagBack:
        navigate(HB_MENU_COMMAND_BACK);
        return;
    case TagForward:
        navigate(HB_MENU_COMMAND_FORWARD);
        return;
    case TagReload:
        navigate(HB_MENU_COMMAND_RELOAD);
        return;
    case TagStop:
        navigate(HB_MENU_COMMAND_STOP);
        return;

    case TagSelectAll:
        executeCommand("SelectAll");
        return;

    case TagCopyPageAddress:
        copyToClipboard(g_context.pageURL);
        return;
    case TagViewPageSource:
        showPageSource(g_context.page);
        return;
    case TagInspectElement:
        showInspector();
        return;

    default:
        return;
    }
}

} // namespace harmony_menus
