#include "harmony_downloads.h"
#include "harmony_downloads_internal.h"

#include <cstdint>

namespace harmony_downloads {
namespace {

// The WebKit entry points this shim needs, resolved out of the module the
// browser bridge already loaded. A download cannot exist before WebKit2.dll is
// in the process, so this looks the module up by name rather than repeating the
// bridge's discovery order.
struct WebKitApi {
    HMODULE module { nullptr };

    WKTypeRef (*retain)(WKTypeRef) { nullptr };
    void (*release)(WKTypeRef) { nullptr };

    void (*downloadSetClient)(WKDownloadRef, void*) { nullptr };
    void (*downloadCancel)(WKDownloadRef, const void*, void (*)(WKDataRef, const void*)) { nullptr };
    WKURLRequestRef (*downloadCopyRequest)(WKDownloadRef) { nullptr };

    WKURLRef (*urlRequestCopyURL)(WKURLRequestRef) { nullptr };
    WKStringRef (*urlCopyString)(WKURLRef) { nullptr };
    WKStringRef (*stringCreateWithUTF8CString)(const char*) { nullptr };
    size_t (*stringGetMaximumUTF8CStringSize)(WKStringRef) { nullptr };
    size_t (*stringGetUTF8CString)(WKStringRef, char*, size_t) { nullptr };

    bool (*navigationActionShouldPerformDownload)(WKNavigationActionRef) { nullptr };
    WKURLRequestRef (*navigationActionCopyRequest)(WKNavigationActionRef) { nullptr };

    bool (*navigationResponseCanShowMIMEType)(WKNavigationResponseRef) { nullptr };
    WKURLResponseRef (*navigationResponseCopyResponse)(WKNavigationResponseRef) { nullptr };
    WKFrameInfoRef (*navigationResponseCopyFrameInfo)(WKNavigationResponseRef) { nullptr };
    bool (*frameInfoGetIsMainFrame)(WKFrameInfoRef) { nullptr };

    WKURLRef (*urlResponseCopyURL)(WKURLResponseRef) { nullptr };
    WKStringRef (*urlResponseCopyMIMEType)(WKURLResponseRef) { nullptr };
    bool (*urlResponseIsAttachment)(WKURLResponseRef) { nullptr };

    WKStringRef (*errorCopyLocalizedDescription)(WKErrorRef) { nullptr };
    int (*errorGetErrorCode)(WKErrorRef) { nullptr };
};

// WebKit reports a cancelled load with this code, and a cancelled download
// arrives as a failure carrying it.
constexpr int kErrorCodeCancelled = 302;

WebKitApi g_api;

template<typename Function>
bool loadFunction(HMODULE module, const char* name, Function& function)
{
    function = reinterpret_cast<Function>(GetProcAddress(module, name));
    return function != nullptr;
}

// Only SUCCESS is remembered. This is asked from the WebKit thread, and the
// first ask can arrive before the engine has finished loading; latching that
// failure would leave the browser unable to download anything for the rest of
// the run because it was asked one cycle too early.
const WebKitApi* api()
{
    if (g_api.module)
        return &g_api;

    HMODULE module = GetModuleHandleW(L"WebKit2.dll");
    if (!module)
        return nullptr;

    WebKitApi resolved;
    resolved.module = module;

    bool ok = true;
    ok &= loadFunction(module, "WKRetain", resolved.retain);
    ok &= loadFunction(module, "WKRelease", resolved.release);
    ok &= loadFunction(module, "WKDownloadSetClient", resolved.downloadSetClient);
    ok &= loadFunction(module, "WKDownloadCancel", resolved.downloadCancel);
    ok &= loadFunction(module, "WKDownloadCopyRequest", resolved.downloadCopyRequest);
    ok &= loadFunction(module, "WKURLRequestCopyURL", resolved.urlRequestCopyURL);
    ok &= loadFunction(module, "WKURLCopyString", resolved.urlCopyString);
    ok &= loadFunction(module, "WKStringCreateWithUTF8CString", resolved.stringCreateWithUTF8CString);
    ok &= loadFunction(module, "WKStringGetMaximumUTF8CStringSize", resolved.stringGetMaximumUTF8CStringSize);
    ok &= loadFunction(module, "WKStringGetUTF8CString", resolved.stringGetUTF8CString);
    ok &= loadFunction(module, "WKNavigationActionShouldPerformDownload", resolved.navigationActionShouldPerformDownload);
    ok &= loadFunction(module, "WKNavigationActionCopyRequest", resolved.navigationActionCopyRequest);
    ok &= loadFunction(module, "WKNavigationResponseCanShowMIMEType", resolved.navigationResponseCanShowMIMEType);
    ok &= loadFunction(module, "WKNavigationResponseCopyResponse", resolved.navigationResponseCopyResponse);
    ok &= loadFunction(module, "WKNavigationResponseCopyFrameInfo", resolved.navigationResponseCopyFrameInfo);
    ok &= loadFunction(module, "WKFrameInfoGetIsMainFrame", resolved.frameInfoGetIsMainFrame);
    ok &= loadFunction(module, "WKURLResponseCopyURL", resolved.urlResponseCopyURL);
    ok &= loadFunction(module, "WKURLResponseCopyMIMEType", resolved.urlResponseCopyMIMEType);
    ok &= loadFunction(module, "WKURLResponseIsAttachment", resolved.urlResponseIsAttachment);
    ok &= loadFunction(module, "WKErrorCopyLocalizedDescription", resolved.errorCopyLocalizedDescription);
    ok &= loadFunction(module, "WKErrorGetErrorCode", resolved.errorGetErrorCode);

    if (!ok)
        return nullptr;

    g_api = resolved;
    return &g_api;
}

void releaseObject(WKTypeRef object)
{
    if (object && g_api.release)
        g_api.release(object);
}

// The text of a WKString. The ref is borrowed: whoever created it still owns it.
std::string textOf(WKStringRef string)
{
    if (!string || !g_api.stringGetMaximumUTF8CStringSize || !g_api.stringGetUTF8CString)
        return { };

    const size_t capacity = g_api.stringGetMaximumUTF8CStringSize(string);
    if (capacity < 2)
        return { };

    std::string text(capacity, '\0');
    const size_t length = g_api.stringGetUTF8CString(string, &text[0], capacity);
    if (length < 2)
        return { };

    // The written length counts the terminator the std::string does not keep.
    text.resize(length - 1);
    return text;
}

std::string textOfURL(WKURLRef url)
{
    if (!url || !g_api.urlCopyString)
        return { };

    WKStringRef string = g_api.urlCopyString(url);
    std::string text = textOf(string);
    releaseObject(string);
    return text;
}

std::string requestURL(WKURLRequestRef request)
{
    if (!request || !g_api.urlRequestCopyURL)
        return { };

    WKURLRef url = g_api.urlRequestCopyURL(request);
    std::string text = textOfURL(url);
    releaseObject(url);
    return text;
}

int idFromContext(const void* context)
{
    return static_cast<int>(reinterpret_cast<uintptr_t>(context));
}

const void* contextFromId(int id)
{
    return reinterpret_cast<const void*>(static_cast<uintptr_t>(id));
}

void finishDownload(int id, State state, std::string error)
{
    releaseObject(model::finish(id, state, std::move(error)));
}

// --- Download client ---------------------------------------------------------

bool willPerformHTTPRedirection(
    WKDownloadRef,
    WKURLResponseRef,
    WKURLRequestRef newRequest,
    const void* clientInfo
) {
    // A redirected download is still the download the user asked for; what
    // changes is the address it should be remembered under.
    model::setURL(idFromContext(clientInfo), requestURL(newRequest));
    return true;
}

WKStringRef decideDestinationWithResponse(
    WKDownloadRef,
    WKURLResponseRef response,
    WKStringRef suggestedFilename,
    const void* clientInfo
) {
    const int id = idFromContext(clientInfo);

    const std::string suggested = textOf(suggestedFilename);
    std::string mimeType;
    if (response && g_api.urlResponseCopyMIMEType) {
        WKStringRef mime = g_api.urlResponseCopyMIMEType(response);
        mimeType = textOf(mime);
        releaseObject(mime);
    }

    const std::string url = model::textOf(id, HB_DOWNLOAD_TEXT_URL);

    const std::wstring directory = downloadsDirectory();
    if (directory.empty()) {
        finishDownload(id, State::Failed, "There is no writable Downloads folder");
        return nullptr;
    }

    const std::wstring name = sanitizedFileName(suggested, url);
    const std::wstring destination = uniqueDestination(directory, name);
    if (destination.empty()) {
        finishDownload(id, State::Failed, "This file could not be given a name");
        return nullptr;
    }

    const std::string utf8Destination = narrow(destination);
    const auto slash = destination.find_last_of(L'\\');
    const std::wstring chosenName = slash == std::wstring::npos
        ? destination
        : destination.substr(slash + 1);

    model::setDestination(id, narrow(chosenName), utf8Destination, destination, mimeType);

    if (!g_api.stringCreateWithUTF8CString) {
        finishDownload(id, State::Failed, "The engine could not be told where to save this file");
        return nullptr;
    }

    // WebKit adopts this string: it is handed over with the reference the create
    // call made, and must not be released here.
    return g_api.stringCreateWithUTF8CString(utf8Destination.c_str());
}

void didWriteData(
    WKDownloadRef,
    long long,
    long long totalBytesWritten,
    long long totalBytesExpectedToWrite,
    const void* clientInfo
) {
    // WKURLResponseGetExpectedContentLength is 32 bits wide and would report a
    // five gigabyte file as a small one, so the size comes from here.
    const long long total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : -1;
    model::setProgress(idFromContext(clientInfo), totalBytesWritten, total);
}

void didFinish(WKDownloadRef, const void* clientInfo)
{
    finishDownload(idFromContext(clientInfo), State::Finished, { });
}

void didFailWithError(WKDownloadRef, WKErrorRef error, WKDataRef, const void* clientInfo)
{
    std::string description;
    int code = 0;
    if (error) {
        if (g_api.errorCopyLocalizedDescription) {
            WKStringRef message = g_api.errorCopyLocalizedDescription(error);
            description = textOf(message);
            releaseObject(message);
        }
        if (g_api.errorGetErrorCode)
            code = g_api.errorGetErrorCode(error);
    }

    State state = State::Failed;
    if (code == kErrorCodeCancelled) {
        state = State::Cancelled;
        description.clear();
    } else if (description.empty()) {
        description = "The download failed";
    }

    finishDownload(idFromContext(clientInfo), state, std::move(description));
}

void didCancel(WKDataRef, const void* context)
{
    finishDownload(idFromContext(context), State::Cancelled, { });
}

// WKDownloadClientV0, laid out exactly as WebKit declares it. WebKit copies the
// struct by version: the version field and the members after it must agree, or
// it reads past the end of what is passed.
struct DownloadClientBase {
    int version;
    const void* clientInfo;
};

struct DownloadClientV0 {
    DownloadClientBase base;

    bool (*willPerformHTTPRedirection)(WKDownloadRef, WKURLResponseRef, WKURLRequestRef, const void*);
    void (*didReceiveAuthenticationChallenge)(WKDownloadRef, WKAuthenticationChallengeRef, const void*);
    WKStringRef (*decideDestinationWithResponse)(WKDownloadRef, WKURLResponseRef, WKStringRef, const void*);
    void (*didWriteData)(WKDownloadRef, long long, long long, long long, const void*);
    void (*didFinish)(WKDownloadRef, const void*);
    void (*didFailWithError)(WKDownloadRef, WKErrorRef, WKDataRef, const void*);
};

static_assert(sizeof(DownloadClientV0) == sizeof(DownloadClientBase) + 6 * sizeof(void*),
    "WKDownloadClientV0 carries 6 pointer-sized members after its base");

}
}

using namespace harmony_downloads;

void hb_downloads_pump(void)
{
    int id = 0;
    while (model::nextCancel(id)) {
        const WebKitApi* resolved = api();
        const WKDownloadRef download = model::liveDownload(id);
        if (resolved && download) {
            resolved->downloadCancel(download, contextFromId(id), didCancel);
            continue;
        }

        // Nothing to ask WebKit: the download never reached it, so the record is
        // the only thing that has to move.
        finishDownload(id, State::Cancelled, { });
    }
}

int hb_downloads_should_download_action(const void* navigation_action)
{
    const WebKitApi* resolved = api();
    if (!resolved || !navigation_action)
        return 0;

    if (resolved->navigationActionShouldPerformDownload(navigation_action))
        return 1;

    WKURLRequestRef request = resolved->navigationActionCopyRequest(navigation_action);
    const std::string url = requestURL(request);
    releaseObject(request);
    return model::consumeForcedURL(url) ? 1 : 0;
}

int hb_downloads_should_download_response(const void* navigation_response)
{
    const WebKitApi* resolved = api();
    if (!resolved || !navigation_response)
        return 0;

    WKURLResponseRef response = resolved->navigationResponseCopyResponse(navigation_response);
    if (!response)
        return 0;

    bool download = resolved->urlResponseIsAttachment(response);

    if (!download) {
        WKURLRef url = resolved->urlResponseCopyURL(response);
        const std::string text = textOfURL(url);
        releaseObject(url);
        download = model::consumeForcedURL(text);
    }

    // A main frame the engine cannot render is a file, not a page. A subframe
    // that cannot be rendered is the page's business.
    if (!download && !resolved->navigationResponseCanShowMIMEType(navigation_response)) {
        WKFrameInfoRef frame = resolved->navigationResponseCopyFrameInfo(navigation_response);
        download = frame && resolved->frameInfoGetIsMainFrame(frame);
        releaseObject(frame);
    }

    releaseObject(response);
    return download ? 1 : 0;
}

void hb_downloads_adopt(const void* download)
{
    const WebKitApi* resolved = api();
    if (!resolved || !download)
        return;

    WKURLRequestRef request = resolved->downloadCopyRequest(download);
    std::string url = requestURL(request);
    releaseObject(request);

    // A name to show before the destination is decided; the decision replaces it
    // with the name the file is actually saved under.
    const std::string provisionalName = narrow(sanitizedFileName({ }, url));

    // The registry hands the download over borrowed, and it is kept here until
    // it reaches a terminal state. Whoever reaches that state -- didFinish,
    // didFailWithError, the cancel callback, or shutdown -- releases it.
    if (!resolved->retain(download))
        return;
    const int id = model::add(std::move(url), download, provisionalName);

    DownloadClientV0 client { };
    client.base.version = 0;
    client.base.clientInfo = contextFromId(id);
    client.willPerformHTTPRedirection = willPerformHTTPRedirection;
    client.didReceiveAuthenticationChallenge = nullptr;
    client.decideDestinationWithResponse = decideDestinationWithResponse;
    client.didWriteData = didWriteData;
    client.didFinish = didFinish;
    client.didFailWithError = didFailWithError;

    // WebKit copies the client struct out of this call, so the stack copy is all
    // it ever needs.
    resolved->downloadSetClient(download, &client);
}

void hb_downloads_shutdown(void)
{
    for (WKDownloadRef download : model::takeLiveDownloads())
        releaseObject(download);
}
