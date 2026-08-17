#include "harmony_navigation_webkit.h"

#include "harmony_navigation_model.h"
#include "harmony_tabs_embed.h"

namespace harmony_navigation {
namespace {

WebKitApi g_api;
bool g_usable { false };

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

bool resolveApi()
{
    // The tabs registry owns the runtime, so its symbol seam is what answers
    // here: this model has no engine of its own to load, and looking the module
    // up by name would be a second discovery order to keep in step with the
    // registry's.
    if (!hb_tabs_webkit_symbol("WKRelease")) {
        setDiagnostic("the WebKit engine is not loaded, so navigation state has nothing to track");
        return false;
    }

    WebKitApi api;
    std::string missing;
    bool ok = true;
    ok &= resolve("WKPageCopyTitle", api.pageCopyTitle, missing);
    ok &= resolve("WKPageCopyActiveURL", api.pageCopyActiveURL, missing);
    ok &= resolve("WKPageCopyCommittedURL", api.pageCopyCommittedURL, missing);
    ok &= resolve("WKPageCopyProvisionalURL", api.pageCopyProvisionalURL, missing);
    ok &= resolve("WKPageGetEstimatedProgress", api.pageGetEstimatedProgress, missing);
    ok &= resolve("WKPageCanGoBack", api.pageCanGoBack, missing);
    ok &= resolve("WKPageCanGoForward", api.pageCanGoForward, missing);
    ok &= resolve("WKPageLoadURL", api.pageLoadURL, missing);
    ok &= resolve("WKPageReload", api.pageReload, missing);
    ok &= resolve("WKPageReloadFromOrigin", api.pageReloadFromOrigin, missing);
    ok &= resolve("WKPageStopLoading", api.pageStopLoading, missing);
    ok &= resolve("WKPageGoBack", api.pageGoBack, missing);
    ok &= resolve("WKPageGoForward", api.pageGoForward, missing);
    ok &= resolve("WKPageGoToBackForwardListItem", api.pageGoToBackForwardListItem, missing);
    ok &= resolve("WKPageGetBackForwardList", api.pageGetBackForwardList, missing);
    ok &= resolve("WKBackForwardListGetBackListCount", api.backForwardListGetBackListCount, missing);
    ok &= resolve("WKBackForwardListGetForwardListCount", api.backForwardListGetForwardListCount, missing);
    ok &= resolve("WKBackForwardListGetItemAtIndex", api.backForwardListGetItemAtIndex, missing);
    ok &= resolve("WKBackForwardListItemCopyURL", api.backForwardListItemCopyURL, missing);
    ok &= resolve("WKBackForwardListItemCopyTitle", api.backForwardListItemCopyTitle, missing);
    ok &= resolve("WKURLCreateWithUTF8CString", api.urlCreateWithUTF8CString, missing);
    ok &= resolve("WKURLCopyString", api.urlCopyString, missing);
    ok &= resolve("WKStringGetMaximumUTF8CStringSize", api.stringGetMaximumUTF8CStringSize, missing);
    ok &= resolve("WKStringGetUTF8CStringNonStrict", api.stringGetUTF8CStringNonStrict, missing);
    ok &= resolve("WKErrorGetErrorCode", api.errorGetErrorCode, missing);
    ok &= resolve("WKErrorCopyLocalizedDescription", api.errorCopyLocalizedDescription, missing);
    ok &= resolve("WKRelease", api.release, missing);

    if (!ok) {
        setDiagnostic("this WebKit build does not export " + missing);
        return false;
    }

    g_api = api;
    setDiagnostic(std::string());
    return true;
}

} // namespace

const WebKitApi* webKitApi()
{
    // Only success is remembered. The first ask can arrive before the registry
    // has finished loading the engine, and latching that would leave the model
    // tracking nothing for the rest of the run because it was asked one cycle
    // too early.
    if (!g_usable)
        g_usable = resolveApi();
    return g_usable ? &g_api : nullptr;
}

void releaseWebKitObject(WKTypeRef object)
{
    if (object && g_usable && g_api.release)
        g_api.release(object);
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
    releaseWebKitObject(text);
    return result;
}

} // namespace harmony_navigation
