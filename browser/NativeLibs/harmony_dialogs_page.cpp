#include "harmony_dialogs_webkit.h"

#include "harmony_dialogs.h"
#include "harmony_tabs_embed.h"

#include <atomic>
#include <string>

// What a page asks, written into the client every page shares.
//
// The tabs registry owns that client and hands each module the struct before it
// is installed, so nothing here calls WKPageSetPageUIClient: a second client
// would replace the first and take somebody else's callbacks with it. What is
// written depends on the version the registry declares, because WebKit copies
// only that many bytes and reads no further.

namespace harmony_dialogs {
namespace {

// The versions of WKPageUIClient the fields below arrived in.
constexpr int kVersionAsyncJavaScriptDialogs = 6;
constexpr int kVersionAsyncBeforeUnload = 7;

// --- JavaScript dialogs, answered later ---------------------------------------

void runJavaScriptAlert(
    WKPageRef page,
    WKStringRef alertText,
    WKFrameRef frame,
    WKSecurityOriginRef origin,
    WKPageRunJavaScriptAlertResultListenerRef listener,
    const void*
)
{
    Request request;
    request.kind = Kind::Alert;
    request.page = page;
    request.listener = listener;
    request.message = stringFromWK(alertText);
    request.origin = originText(origin);
    request.flags = frameFlags(frame);
    enqueueRequest(std::move(request));
}

void runJavaScriptConfirm(
    WKPageRef page,
    WKStringRef message,
    WKFrameRef frame,
    WKSecurityOriginRef origin,
    WKPageRunJavaScriptConfirmResultListenerRef listener,
    const void*
)
{
    Request request;
    request.kind = Kind::Confirm;
    request.page = page;
    request.listener = listener;
    request.message = stringFromWK(message);
    request.origin = originText(origin);
    request.flags = frameFlags(frame);
    enqueueRequest(std::move(request));
}

void runJavaScriptPrompt(
    WKPageRef page,
    WKStringRef message,
    WKStringRef defaultValue,
    WKFrameRef frame,
    WKSecurityOriginRef origin,
    WKPageRunJavaScriptPromptResultListenerRef listener,
    const void*
)
{
    Request request;
    request.kind = Kind::Prompt;
    request.page = page;
    request.listener = listener;
    request.message = stringFromWK(message);
    request.defaultValue = stringFromWK(defaultValue);
    request.origin = originText(origin);
    request.flags = frameFlags(frame);
    enqueueRequest(std::move(request));
}

void runBeforeUnloadConfirmPanel(
    WKPageRef page,
    WKStringRef message,
    WKFrameRef frame,
    WKPageRunBeforeUnloadConfirmPanelResultListenerRef listener,
    const void*
)
{
    Request request;
    request.kind = Kind::BeforeUnload;
    request.page = page;
    request.listener = listener;
    request.message = stringFromWK(message);
    request.flags = frameFlags(frame);
    enqueueRequest(std::move(request));
}

// --- JavaScript dialogs, answered from the callback ----------------------------
//
// A client older than the version that introduced a listener takes the answer as
// the callback's return value, so the answer has to be in hand before it
// returns. Parking the question and waiting is what makes that possible without
// answering for the person: the WebKit thread stays inside this call, pumping
// its own messages so every other page keeps running, until a person presses
// something.

bool askAndWait(Kind kind, WKPageRef page, WKFrameRef frame, std::string message, std::string defaultValue, std::string& answerText)
{
    Request request;
    request.kind = kind;
    request.page = page;
    request.blocking = true;
    request.message = std::move(message);
    request.defaultValue = std::move(defaultValue);
    request.flags = frameFlags(frame);

    const int id = enqueueRequest(std::move(request));
    if (!id)
        return false;
    return waitForBlockingAnswer(id, answerText);
}

void runJavaScriptAlertV0(WKPageRef page, WKStringRef alertText, WKFrameRef frame, const void*)
{
    std::string answer;
    askAndWait(Kind::Alert, page, frame, stringFromWK(alertText), std::string(), answer);
}

bool runJavaScriptConfirmV0(WKPageRef page, WKStringRef message, WKFrameRef frame, const void*)
{
    std::string answer;
    return askAndWait(Kind::Confirm, page, frame, stringFromWK(message), std::string(), answer);
}

WKStringRef runJavaScriptPromptV0(WKPageRef page, WKStringRef message, WKStringRef defaultValue, WKFrameRef frame, const void*)
{
    std::string answer;
    if (!askAndWait(Kind::Prompt, page, frame, stringFromWK(message), stringFromWK(defaultValue), answer))
        return nullptr;
    return makeString(answer);
}

bool runBeforeUnloadConfirmPanelV6(WKPageRef page, WKStringRef message, WKFrameRef frame, const void*)
{
    std::string answer;
    return askAndWait(Kind::BeforeUnload, page, frame, stringFromWK(message), std::string(), answer);
}

// --- The file picker ---------------------------------------------------------

void runOpenPanel(
    WKPageRef page,
    WKFrameRef,
    WKOpenPanelParametersRef parameters,
    WKOpenPanelResultListenerRef listener,
    const void*
)
{
    auto mimeTypesArray = wk().openPanelCopyAcceptedMIMETypes(parameters);
    auto extensionsArray = wk().openPanelCopyAcceptedFileExtensions(parameters);

    FilePickerRequest picker;
    picker.owner = hostWindow();
    picker.allowsMultiple = wk().openPanelAllowsMultipleFiles(parameters);
    picker.allowsDirectories = wk().openPanelAllowsDirectories(parameters);
    picker.mimeTypes = stringsFromArray(mimeTypesArray);
    picker.extensions = stringsFromArray(extensionsArray);

    releaseWK(mimeTypesArray);
    releaseWK(extensionsArray);

    Request request;
    request.kind = Kind::FilePicker;
    request.page = page;
    request.listener = listener;
    picker.id = enqueueRequest(std::move(request));

    filePickerSubmit(std::move(picker));
}

// --- Authentication ----------------------------------------------------------

bool isPasswordScheme(WKProtectionSpaceAuthenticationScheme scheme)
{
    return scheme == kWKProtectionSpaceAuthenticationSchemeDefault
        || scheme == kWKProtectionSpaceAuthenticationSchemeHTTPBasic
        || scheme == kWKProtectionSpaceAuthenticationSchemeHTTPDigest
        || scheme == kWKProtectionSpaceAuthenticationSchemeNTLM
        || scheme == kWKProtectionSpaceAuthenticationSchemeNegotiate;
}

// The certificate chain a protection space presents, as the PEM text WinCairo
// stores it in.
std::string certificateChainPem(WKProtectionSpaceRef protectionSpace)
{
    auto chain = wk().protectionSpaceCopyCertificateChain(protectionSpace);
    if (!chain)
        return std::string();

    std::string pem;
    const size_t count = wk().arrayGetSize(chain);
    for (size_t index = 0; index < count; ++index) {
        auto item = wk().arrayGetItemAtIndex(chain, index);
        if (!item)
            continue;
        const unsigned char* bytes = wk().dataGetBytes(item);
        const size_t size = wk().dataGetSize(item);
        if (bytes && size)
            pem.append(reinterpret_cast<const char*>(bytes), size);
    }
    releaseWK(chain);
    return pem;
}

void acceptServerTrust(WKAuthenticationChallengeRef challenge)
{
    auto listener = wk().challengeGetDecisionListener(challenge);
    if (!listener)
        return;
    auto user = makeString(kAcceptServerTrustUser);
    auto password = makeString(std::string());
    auto credential = wk().credentialCreate(user, password, kWKCredentialPersistenceForSession);
    wk().decisionUseCredential(listener, credential);
    releaseWK(credential);
    releaseWK(user);
    releaseWK(password);
}

// A refused sign-in is not a cancelled load: continuing without a credential is
// what lets the server's own response reach the page.
void continueWithoutCredential(WKAuthenticationChallengeRef challenge)
{
    if (auto listener = wk().challengeGetDecisionListener(challenge))
        wk().decisionUseCredential(listener, nullptr);
}

void queueCertificateRequest(WKPageRef page, WKAuthenticationChallengeRef challenge, WKProtectionSpaceRef space)
{
    auto hostRef = wk().protectionSpaceCopyHost(space);
    auto descriptionRef = wk().protectionSpaceCopyCertificateVerificationErrorDescription(space);
    const std::string host = stringFromWK(hostRef);
    const std::string description = stringFromWK(descriptionRef);
    releaseWK(hostRef);
    releaseWK(descriptionRef);

    const std::string pem = certificateChainPem(space);
    if (certificateIsTrusted(host, pem)) {
        // Answered once already for this exact certificate. Asking again for
        // every subresource is what makes an interstitial worthless.
        acceptServerTrust(challenge);
        return;
    }

    Request request;
    request.kind = Kind::Certificate;
    request.page = page;
    request.challenge = challenge;
    request.origin = host;
    request.message = description;
    request.code = wk().protectionSpaceGetCertificateVerificationError(space);
    request.certificateHost = host;
    request.certificatePem = pem;
    enqueueRequest(std::move(request));
}

void queueCredentialRequest(WKPageRef page, WKAuthenticationChallengeRef challenge, WKProtectionSpaceRef space)
{
    auto hostRef = wk().protectionSpaceCopyHost(space);
    auto realmRef = wk().protectionSpaceCopyRealm(space);
    const std::string host = stringFromWK(hostRef);
    const std::string realm = stringFromWK(realmRef);
    releaseWK(hostRef);
    releaseWK(realmRef);

    const int port = wk().protectionSpaceGetPort(space);
    const bool proxy = wk().protectionSpaceGetIsProxy(space);
    const int failures = wk().challengeGetPreviousFailureCount(challenge);

    Request request;
    request.kind = Kind::Authenticate;
    request.page = page;
    request.challenge = challenge;
    request.origin = port ? host + ":" + std::to_string(port) : host;
    request.detail = realm;
    request.code = failures;
    request.flags = (proxy ? HB_DIALOG_FLAG_PROXY : 0) | (failures > 0 ? HB_DIALOG_FLAG_RETRY : 0);
    enqueueRequest(std::move(request));
}

// --- The tabs registry's seams -------------------------------------------------

void writeClientFields(int, hb_wk_page_ui_client_v19* shared, void*)
{
    if (!shared || !resolveWebKitApi())
        return;

    // The registry's struct is this layout under a name of its own, untyped, so
    // the fields written below land where WebKit reads them. The version is
    // still asked of the client rather than assumed: a field past the version
    // the registry installed is a field WebKit never copies.
    const int version = shared->base.version;
    auto* client = reinterpret_cast<WKPageUIClientV19*>(shared);

    client->runOpenPanel = runOpenPanel;

    if (version >= kVersionAsyncJavaScriptDialogs) {
        client->runJavaScriptAlert = runJavaScriptAlert;
        client->runJavaScriptConfirm = runJavaScriptConfirm;
        client->runJavaScriptPrompt = runJavaScriptPrompt;
    } else {
        client->runJavaScriptAlert_deprecatedForUseWithV0 = runJavaScriptAlertV0;
        client->runJavaScriptConfirm_deprecatedForUseWithV0 = runJavaScriptConfirmV0;
        client->runJavaScriptPrompt_deprecatedForUseWithV0 = runJavaScriptPromptV0;
    }

    if (version >= kVersionAsyncBeforeUnload)
        client->runBeforeUnloadConfirmPanel = runBeforeUnloadConfirmPanel;
    else
        client->runBeforeUnloadConfirmPanel_deprecatedForUseWithV6 = runBeforeUnloadConfirmPanelV6;
}

// --- Authentication, on the page's navigation client --------------------------
//
// A sign-in, a proxy's sign-in and a certificate the system refused all arrive
// as an authentication challenge on the page's NAVIGATION client rather than its
// UI client. That client is the registry's too, so these two fields are written
// into it through the registry's hook. Nothing else in it is touched: the rest
// is a report or a decision that belongs to whoever owns it.

// Whether this browser can answer at all: the password schemes, and server
// trust, which is the certificate interstitial.
bool canAuthenticateAgainstProtectionSpace(WKPageRef, WKProtectionSpaceRef space, const void*)
{
    if (!space || !resolveWebKitApi())
        return false;

    const auto scheme = wk().protectionSpaceGetAuthenticationScheme(space);
    if (scheme == kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested)
        return true;
    return isPasswordScheme(scheme);
}

void didReceiveAuthenticationChallenge(WKPageRef page, WKAuthenticationChallengeRef challenge, const void*)
{
    if (!challenge || !resolveWebKitApi())
        return;

    auto space = wk().challengeGetProtectionSpace(challenge);
    if (!space) {
        continueWithoutCredential(challenge);
        return;
    }

    const auto scheme = wk().protectionSpaceGetAuthenticationScheme(space);
    if (scheme == kWKProtectionSpaceAuthenticationSchemeServerTrustEvaluationRequested) {
        queueCertificateRequest(page, challenge, space);
        return;
    }
    if (isPasswordScheme(scheme)) {
        queueCredentialRequest(page, challenge, space);
        return;
    }

    // A client certificate, an OAuth redirect, a scheme this build does not
    // know: nothing here can supply one, so the load continues without a
    // credential and the server's own answer is what the page shows.
    continueWithoutCredential(challenge);
}

using CanAuthenticateCallback = bool (*)(WKPageRef, WKProtectionSpaceRef, const void*);
using DidReceiveChallengeCallback = void (*)(WKPageRef, WKAuthenticationChallengeRef, const void*);

void writeNavigationClientFields(int, hb_wk_page_navigation_client_v3* client, void*)
{
    if (!client || !resolveWebKitApi())
        return;

    client->canAuthenticateAgainstProtectionSpace = reinterpret_cast<void*>(
        static_cast<CanAuthenticateCallback>(canAuthenticateAgainstProtectionSpace)
    );
    client->didReceiveAuthenticationChallenge = reinterpret_cast<void*>(
        static_cast<DidReceiveChallengeCallback>(didReceiveAuthenticationChallenge)
    );
}

void pageCreated(int, void* page, void*)
{
    if (!page || !resolveWebKitApi())
        return;
    attachPage(page);
}

void pageDestroying(int, void* page, void*)
{
    if (!page)
        return;
    detachPage(page);
}

std::atomic<bool> g_attached { false };

} // namespace

void attachToTabRegistry()
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    hb_tabs_add_ui_client_hook(writeClientFields, nullptr);
    hb_tabs_add_navigation_client_hook(writeNavigationClientFields, nullptr);
    hb_tabs_add_page_observer(pageCreated, pageDestroying, nullptr);
}

} // namespace harmony_dialogs
