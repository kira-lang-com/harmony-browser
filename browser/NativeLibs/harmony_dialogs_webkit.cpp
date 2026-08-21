#include "harmony_dialogs_webkit.h"

#include "harmony_dialogs.h"
#include "harmony_tabs_embed.h"

#include <mutex>
#include <string>
#include <vector>

namespace harmony_dialogs {
namespace {

WebKitApi g_api;
bool g_resolved { false };
bool g_usable { false };

std::mutex g_errorMutex;
std::string g_error;

template <typename Fn>
bool loadFunction(const char* name, Fn& slot)
{
    slot = reinterpret_cast<Fn>(hb_tabs_webkit_symbol(name));
    if (slot)
        return true;
    setDialogsError(std::string("the WebKit runtime exports no ") + name);
    return false;
}

} // namespace

bool resolveWebKitApi()
{
    if (g_resolved)
        return g_usable;

    // The tabs registry owns the runtime, so its symbol seam answers nothing
    // until the engine is up. Resolution is retried rather than remembered as a
    // failure: the first page's question is what usually asks first, and by then
    // the engine that created the page is loaded.
    if (!hb_tabs_webkit_symbol("WKRelease")) {
        setDialogsError("the WebKit runtime is not loaded yet");
        return false;
    }
    g_resolved = true;

    g_usable =
        loadFunction("WKPageForceRepaint", g_api.pageForceRepaint)
        && loadFunction("WKPageRunJavaScriptAlertResultListenerCall", g_api.alertListenerCall)
        && loadFunction("WKPageRunJavaScriptConfirmResultListenerCall", g_api.confirmListenerCall)
        && loadFunction("WKPageRunJavaScriptPromptResultListenerCall", g_api.promptListenerCall)
        && loadFunction("WKPageRunBeforeUnloadConfirmPanelResultListenerCall", g_api.beforeUnloadListenerCall)
        && loadFunction("WKOpenPanelParametersGetAllowsMultipleFiles", g_api.openPanelAllowsMultipleFiles)
        && loadFunction("WKOpenPanelParametersGetAllowsDirectories", g_api.openPanelAllowsDirectories)
        && loadFunction("WKOpenPanelParametersCopyAcceptedMIMETypes", g_api.openPanelCopyAcceptedMIMETypes)
        && loadFunction("WKOpenPanelParametersCopyAcceptedFileExtensions", g_api.openPanelCopyAcceptedFileExtensions)
        && loadFunction("WKOpenPanelResultListenerChooseFiles", g_api.openPanelChooseFiles)
        && loadFunction("WKOpenPanelResultListenerCancel", g_api.openPanelCancel)
        && loadFunction("WKAuthenticationChallengeGetDecisionListener", g_api.challengeGetDecisionListener)
        && loadFunction("WKAuthenticationChallengeGetProtectionSpace", g_api.challengeGetProtectionSpace)
        && loadFunction("WKAuthenticationChallengeGetPreviousFailureCount", g_api.challengeGetPreviousFailureCount)
        && loadFunction("WKAuthenticationDecisionListenerUseCredential", g_api.decisionUseCredential)
        && loadFunction("WKProtectionSpaceCopyHost", g_api.protectionSpaceCopyHost)
        && loadFunction("WKProtectionSpaceGetPort", g_api.protectionSpaceGetPort)
        && loadFunction("WKProtectionSpaceCopyRealm", g_api.protectionSpaceCopyRealm)
        && loadFunction("WKProtectionSpaceGetIsProxy", g_api.protectionSpaceGetIsProxy)
        && loadFunction("WKProtectionSpaceGetAuthenticationScheme", g_api.protectionSpaceGetAuthenticationScheme)
        && loadFunction("WKProtectionSpaceCopyCertificateChain", g_api.protectionSpaceCopyCertificateChain)
        && loadFunction("WKProtectionSpaceGetCertificateVerificationError", g_api.protectionSpaceGetCertificateVerificationError)
        && loadFunction("WKProtectionSpaceCopyCertificateVerificationErrorDescription", g_api.protectionSpaceCopyCertificateVerificationErrorDescription)
        && loadFunction("WKCredentialCreate", g_api.credentialCreate)
        && loadFunction("WKSecurityOriginCopyProtocol", g_api.securityOriginCopyProtocol)
        && loadFunction("WKSecurityOriginCopyHost", g_api.securityOriginCopyHost)
        && loadFunction("WKSecurityOriginGetPort", g_api.securityOriginGetPort)
        && loadFunction("WKFrameIsMainFrame", g_api.frameIsMainFrame)
        && loadFunction("WKStringCreateWithUTF8CString", g_api.stringCreateWithUTF8CString)
        && loadFunction("WKStringGetMaximumUTF8CStringSize", g_api.stringGetMaximumUTF8CStringSize)
        && loadFunction("WKStringGetUTF8CString", g_api.stringGetUTF8CString)
        && loadFunction("WKURLCreateWithUTF8CString", g_api.urlCreateWithUTF8CString)
        && loadFunction("WKArrayCreateAdoptingValues", g_api.arrayCreateAdoptingValues)
        && loadFunction("WKArrayGetItemAtIndex", g_api.arrayGetItemAtIndex)
        && loadFunction("WKArrayGetSize", g_api.arrayGetSize)
        && loadFunction("WKDataGetBytes", g_api.dataGetBytes)
        && loadFunction("WKDataGetSize", g_api.dataGetSize)
        && loadFunction("WKRetain", g_api.retain)
        && loadFunction("WKRelease", g_api.release);

    if (g_usable)
        setDialogsError(std::string());
    return g_usable;
}

const WebKitApi& wk()
{
    return g_api;
}

void setDialogsError(const std::string& text)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error = text;
}

void releaseWK(WKTypeRef value)
{
    if (value && g_api.release)
        g_api.release(value);
}

std::string stringFromWK(WKStringRef value)
{
    if (!value || !g_api.stringGetMaximumUTF8CStringSize)
        return std::string();
    const size_t capacity = g_api.stringGetMaximumUTF8CStringSize(value);
    if (!capacity)
        return std::string();
    std::string buffer(capacity, '\0');
    const size_t written = g_api.stringGetUTF8CString(value, &buffer[0], capacity);
    if (!written)
        return std::string();
    // The written length counts the terminator this string does not keep.
    buffer.resize(written - 1);
    return buffer;
}

WKStringRef makeString(const std::string& text)
{
    if (!g_api.stringCreateWithUTF8CString)
        return nullptr;
    return g_api.stringCreateWithUTF8CString(text.c_str());
}

std::vector<std::string> stringsFromArray(WKArrayRef array)
{
    std::vector<std::string> values;
    if (!array)
        return values;
    const size_t count = g_api.arrayGetSize(array);
    values.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        std::string text = stringFromWK(g_api.arrayGetItemAtIndex(array, index));
        if (!text.empty())
            values.push_back(std::move(text));
    }
    return values;
}

std::string originText(WKSecurityOriginRef origin)
{
    if (!origin)
        return std::string();

    auto protocolRef = g_api.securityOriginCopyProtocol(origin);
    auto hostRef = g_api.securityOriginCopyHost(origin);
    const std::string protocol = stringFromWK(protocolRef);
    const std::string host = stringFromWK(hostRef);
    releaseWK(protocolRef);
    releaseWK(hostRef);

    if (host.empty())
        return std::string();

    std::string text;
    if (!protocol.empty())
        text = protocol + "://";
    text += host;
    if (const unsigned short port = g_api.securityOriginGetPort(origin))
        text += ":" + std::to_string(port);
    return text;
}

int frameFlags(WKFrameRef frame)
{
    if (frame && !g_api.frameIsMainFrame(frame))
        return HB_DIALOG_FLAG_SUBFRAME;
    return 0;
}

extern "C" const char* hb_dialogs_error(void)
{
    static thread_local std::string copy;
    {
        std::lock_guard<std::mutex> lock(g_errorMutex);
        copy = g_error;
    }
    return copy.c_str();
}

} // namespace harmony_dialogs
