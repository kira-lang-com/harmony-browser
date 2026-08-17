#include "harmony_dialogs_webkit.h"

#include "harmony_dialogs.h"
#include "harmony_tabs_embed.h"

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace harmony_dialogs {
namespace {

std::mutex g_mutex;
std::deque<Request> g_requests;
std::vector<WKPageRef> g_pages;
std::vector<WKPageRef> g_repaintPages;
std::map<std::string, std::string> g_trustedCertificates;
int g_nextRequestId { 1 };

std::atomic<HWND> g_hostWindow { nullptr };

// The child windows held down for as long as something is over the page, and
// the handles to put back afterwards. Read and written on the frame thread only.
std::vector<HWND> g_hiddenWindows;

// --- Delivery ----------------------------------------------------------------

// Hands one answer to WebKit and drops the references the request held. Runs on
// the WebKit thread, outside the lock.
void deliver(Request& request)
{
    // A question answered from inside its own callback has no listener: the
    // answer left with the callback's return value, and there is nothing here
    // left to tell.
    if (request.blocking)
        return;

    switch (request.kind) {
    case Kind::Alert:
        if (request.listener)
            wk().alertListenerCall(request.listener);
        break;
    case Kind::Confirm:
        if (request.listener)
            wk().confirmListenerCall(request.listener, request.accepted);
        break;
    case Kind::Prompt: {
        if (!request.listener)
            break;
        if (!request.accepted) {
            // A refused prompt is null to the page, which is exactly what
            // `window.prompt` returning null means.
            wk().promptListenerCall(request.listener, nullptr);
            break;
        }
        auto text = makeString(request.answerText);
        wk().promptListenerCall(request.listener, text);
        releaseWK(text);
        break;
    }
    case Kind::BeforeUnload:
        if (request.listener)
            wk().beforeUnloadListenerCall(request.listener, request.accepted);
        break;
    case Kind::Authenticate: {
        auto listener = wk().challengeGetDecisionListener(request.challenge);
        if (!listener)
            break;
        if (!request.accepted) {
            // Not a cancelled load: the server's own response to a refused
            // sign-in is what the page should show.
            wk().decisionUseCredential(listener, nullptr);
            break;
        }
        auto user = makeString(request.answerText);
        auto password = makeString(request.answerSecret);
        auto credential = wk().credentialCreate(
            user,
            password,
            request.remember ? kWKCredentialPersistencePermanent : kWKCredentialPersistenceForSession
        );
        wk().decisionUseCredential(listener, credential);
        releaseWK(credential);
        releaseWK(user);
        releaseWK(password);
        break;
    }
    case Kind::Certificate: {
        auto listener = wk().challengeGetDecisionListener(request.challenge);
        if (!listener)
            break;
        if (!request.accepted) {
            wk().decisionUseCredential(listener, nullptr);
            break;
        }
        if (request.remember && !request.certificateHost.empty()) {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_trustedCertificates[request.certificateHost] = request.certificatePem;
        }
        auto user = makeString(kAcceptServerTrustUser);
        auto password = makeString(std::string());
        auto credential = wk().credentialCreate(user, password, kWKCredentialPersistenceForSession);
        wk().decisionUseCredential(listener, credential);
        releaseWK(credential);
        releaseWK(user);
        releaseWK(password);
        break;
    }
    case Kind::FilePicker: {
        if (!request.listener)
            break;
        if (!request.accepted || request.answerPaths.empty()) {
            wk().openPanelCancel(request.listener);
            break;
        }
        std::vector<WKTypeRef> urls;
        urls.reserve(request.answerPaths.size());
        for (const auto& path : request.answerPaths) {
            if (auto url = wk().urlCreateWithUTF8CString(path.c_str()))
                urls.push_back(url);
        }
        if (urls.empty()) {
            wk().openPanelCancel(request.listener);
            break;
        }
        // The array adopts the reference on every URL, so nothing below
        // releases them.
        auto array = wk().arrayCreateAdoptingValues(urls.data(), urls.size());
        // The listener dereferences the allowed-types array rather than testing
        // it, so "no restriction" is an empty array and never a null one.
        WKTypeRef noValues = nullptr;
        auto allowedTypes = wk().arrayCreateAdoptingValues(&noValues, 0);
        wk().openPanelChooseFiles(request.listener, array, allowedTypes);
        releaseWK(allowedTypes);
        releaseWK(array);
        break;
    }
    }

    releaseWK(request.listener);
    releaseWK(request.challenge);
    request.listener = nullptr;
    request.challenge = nullptr;
}

// Marks a parked request answered. Runs on the frame thread.
void answer(int requestId, bool accepted, std::string text, std::string secret, bool remember)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Request* request = nullptr;
        for (auto& candidate : g_requests) {
            if (candidate.id != requestId || candidate.answered)
                continue;
            request = &candidate;
            break;
        }
        if (!request)
            return;
        request->answered = true;
        request->accepted = accepted;
        request->answerText = std::move(text);
        request->answerSecret = std::move(secret);
        request->remember = remember;
    }
    requestWebKitService();
}

Request* findRequest(int requestId)
{
    for (auto& request : g_requests) {
        if (request.id == requestId)
            return &request;
    }
    return nullptr;
}

// Takes every answered request out of the queue. Runs on the WebKit thread; a
// blocking request is left where it is, because the callback still sitting in
// `waitForBlockingAnswer` is the one that owns it.
std::vector<Request> takeAnswered()
{
    std::vector<Request> ready;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto position = g_requests.begin(); position != g_requests.end();) {
        if (!position->answered || position->blocking) {
            ++position;
            continue;
        }
        ready.push_back(std::move(*position));
        position = g_requests.erase(position);
    }
    return ready;
}

void repaintCompleted(WKTypeRef, void*)
{
}

// Hands WebKit everything the frame thread has answered. WebKit thread only.
void serviceOnWebKitThread(void*)
{
    if (!resolveWebKitApi())
        return;

    std::vector<Request> ready = takeAnswered();
    std::vector<WKPageRef> repaint;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        repaint.swap(g_repaintPages);
    }

    for (auto& request : ready)
        deliver(request);

    for (auto page : repaint)
        wk().pageForceRepaint(page, nullptr, repaintCompleted);
}

// Refuses everything parked and hands the refusals back. WebKit thread only.
void refuseEverythingOnWebKitThread(void*)
{
    if (!resolveWebKitApi())
        return;

    std::vector<Request> pending;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto position = g_requests.begin(); position != g_requests.end();) {
            if (position->blocking) {
                // Its own callback is still on this thread waiting for it, and
                // that callback is what answers WebKit.
                position->answered = true;
                position->accepted = false;
                ++position;
                continue;
            }
            pending.push_back(std::move(*position));
            position = g_requests.erase(position);
        }
        g_pages.clear();
        g_repaintPages.clear();
    }

    for (auto& request : pending) {
        request.accepted = false;
        deliver(request);
    }
}

// --- View suppression --------------------------------------------------------

// A dialog the host draws lives in the host's own window, and a page's view is a
// child window on top of it. Every child window of the host belongs to the page
// host — the host itself paints into the window, not into children of it — so
// taking the children down for exactly as long as something is over the page is
// what puts that thing in front of the page and keeps the pointer off it.
BOOL CALLBACK collectVisibleChild(HWND child, LPARAM parameter)
{
    if (IsWindowVisible(child))
        reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(child);
    return TRUE;
}

void applyViewVisibility(bool hide)
{
    const UINT common = SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS;
    HWND host = g_hostWindow.load();

    if (hide) {
        if (!host)
            return;
        // Re-asserted every frame rather than on the transition: the page host
        // puts the active view back up whenever its rectangle changes, and a
        // resize while a dialog is open would otherwise leave the page in front
        // of it.
        std::vector<HWND> visible;
        EnumChildWindows(host, collectVisibleChild, reinterpret_cast<LPARAM>(&visible));
        for (HWND child : visible) {
            SetWindowPos(child, nullptr, 0, 0, 0, 0, common | SWP_HIDEWINDOW);
            bool known = false;
            for (HWND remembered : g_hiddenWindows) {
                if (remembered == child) {
                    known = true;
                    break;
                }
            }
            if (!known)
                g_hiddenWindows.push_back(child);
        }
        return;
    }

    if (g_hiddenWindows.empty())
        return;

    for (HWND child : g_hiddenWindows) {
        if (IsWindow(child))
            SetWindowPos(child, nullptr, 0, 0, 0, 0, common | SWP_SHOWWINDOW);
    }
    g_hiddenWindows.clear();

    // A view that has been out of the window holds no pixels of its own under
    // accelerated compositing, so every page is asked for a frame rather than
    // left blank until it next paints itself. The page host's stand-in frame is
    // not put back with them: the view it stood in for is about to paint, and a
    // stale frame over it would be the one thing worse than a blank moment.
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (WKPageRef page : g_pages)
            g_repaintPages.push_back(page);
    }
    requestWebKitService();
}

} // namespace

int enqueueRequest(Request request)
{
    if (!wk().retain)
        return 0;
    if (request.listener)
        wk().retain(request.listener);
    if (request.challenge)
        wk().retain(request.challenge);

    std::lock_guard<std::mutex> lock(g_mutex);
    request.id = g_nextRequestId++;
    if (g_nextRequestId <= 0)
        g_nextRequestId = 1;
    const int id = request.id;
    g_requests.push_back(std::move(request));
    return id;
}

bool waitForBlockingAnswer(int requestId, std::string& answerText)
{
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            Request* request = findRequest(requestId);
            if (!request)
                return false;
            if (request->answered) {
                const bool accepted = request->accepted;
                answerText = std::move(request->answerText);
                for (auto position = g_requests.begin(); position != g_requests.end(); ++position) {
                    if (position->id != requestId)
                        continue;
                    g_requests.erase(position);
                    break;
                }
                return accepted;
            }
        }

        // Nothing here waits on the answer directly: this thread is WebKit's,
        // and stopping it dead would stop every other page in the browser as
        // well. It runs WebKit's own messages instead, which is what keeps the
        // rest of the engine alive while one page is asking.
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

void requestWebKitService()
{
    hb_tabs_invoke_on_webkit_thread(serviceOnWebKitThread, nullptr);
}

bool certificateIsTrusted(const std::string& host, const std::string& pem)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto known = g_trustedCertificates.find(host);
    return known != g_trustedCertificates.end() && known->second == pem;
}

HWND hostWindow()
{
    return g_hostWindow.load();
}

void filePickerCompleted(int requestId, std::vector<std::string> paths)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Request* request = findRequest(requestId);
        if (!request || request->answered)
            return;
        request->answered = true;
        request->accepted = !paths.empty();
        request->answerPaths = std::move(paths);
    }
    requestWebKitService();
}

// --- The WebKit thread -------------------------------------------------------

void attachPage(WKPageRef page)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (WKPageRef known : g_pages) {
        if (known == page)
            return;
    }
    g_pages.push_back(page);
}

void detachPage(WKPageRef page)
{
    std::vector<Request> orphaned;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto position = g_requests.begin(); position != g_requests.end();) {
            if (position->page != page) {
                ++position;
                continue;
            }
            if (position->blocking) {
                // The callback waiting on it is further down this same thread's
                // stack; marking it refused is what lets that callback return.
                position->answered = true;
                position->accepted = false;
                ++position;
                continue;
            }
            orphaned.push_back(std::move(*position));
            position = g_requests.erase(position);
        }
        for (auto position = g_pages.begin(); position != g_pages.end();) {
            if (*position == page)
                position = g_pages.erase(position);
            else
                ++position;
        }
    }

    // A page that is going away still owes every listener an answer, and the
    // one it owes is the answer a person dismissing the dialog would have given.
    if (!resolveWebKitApi())
        return;
    for (auto& request : orphaned) {
        request.accepted = false;
        deliver(request);
    }
}

// --- The C surface -------------------------------------------------------------

extern "C" void hb_dialogs_attach(void)
{
    attachToTabRegistry();
}

extern "C" void hb_dialogs_shutdown(void)
{
    filePickerShutdown();
    hb_tabs_invoke_on_webkit_thread(refuseEverythingOnWebKitThread, nullptr);
}

// --- The frame thread --------------------------------------------------------

extern "C" int hb_dialogs_frame(void* host_window, int host_overlay)
{
    if (host_window)
        g_hostWindow.store(static_cast<HWND>(host_window));

    int presenting = 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // The newest question, not the oldest: a page that asks a second thing
        // from inside the answer to the first is asking about the answer being
        // given, and that is the one a person is looking at.
        for (auto position = g_requests.rbegin(); position != g_requests.rend(); ++position) {
            if (position->kind == Kind::FilePicker || position->answered)
                continue;
            presenting = position->id;
            break;
        }
    }

    applyViewVisibility(presenting != 0 || host_overlay != 0);
    return presenting;
}

extern "C" int hb_dialogs_request_kind(int request_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    if (!request || request->kind == Kind::FilePicker)
        return HB_DIALOG_NONE;
    return static_cast<int>(request->kind);
}

extern "C" int hb_dialogs_request_flags(int request_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    return request ? request->flags : 0;
}

extern "C" int hb_dialogs_request_code(int request_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    return request ? request->code : 0;
}

extern "C" const char* hb_dialogs_request_message(int request_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    storage = request ? request->message : std::string();
    return storage.c_str();
}

extern "C" const char* hb_dialogs_request_origin(int request_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    storage = request ? request->origin : std::string();
    return storage.c_str();
}

extern "C" const char* hb_dialogs_request_default_value(int request_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    storage = request ? request->defaultValue : std::string();
    return storage.c_str();
}

extern "C" const char* hb_dialogs_request_detail(int request_id)
{
    static thread_local std::string storage;
    std::lock_guard<std::mutex> lock(g_mutex);
    const Request* request = findRequest(request_id);
    storage = request ? request->detail : std::string();
    return storage.c_str();
}

extern "C" void hb_dialogs_respond_accept(int request_id)
{
    answer(request_id, true, std::string(), std::string(), false);
}

extern "C" void hb_dialogs_respond_dismiss(int request_id)
{
    answer(request_id, false, std::string(), std::string(), false);
}

extern "C" void hb_dialogs_respond_text(int request_id, const char* text)
{
    answer(request_id, true, text ? std::string(text) : std::string(), std::string(), false);
}

extern "C" void hb_dialogs_respond_credential(int request_id, const char* user, const char* password, int remember)
{
    answer(
        request_id,
        true,
        user ? std::string(user) : std::string(),
        password ? std::string(password) : std::string(),
        remember != 0
    );
}

extern "C" void hb_dialogs_respond_trust(int request_id, int remember)
{
    answer(request_id, true, std::string(), std::string(), remember != 0);
}

} // namespace harmony_dialogs
