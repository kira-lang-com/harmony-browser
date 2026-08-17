#include "harmony_menus_internal.h"

#include <mutex>

// What the page actually said.
//
// There is no `view-source:` scheme in this engine, so the source is asked for,
// written out as a text document, and opened in a tab like any other document.
// The web process answers asynchronously -- it has to serialise the main
// resource and send it back -- so the tab is asked for from inside the answer
// rather than from the menu selection that started it.
//
// The dumps go beside the other temporary files this machine keeps rather than
// into the profile: a source listing is scratch, it is named after the tab and
// the moment it was taken, and every one this run wrote is removed when the
// browser stops.

namespace harmony_menus {

namespace {

std::mutex g_dumpMutex;
std::vector<std::wstring> g_dumps;

// What the page was, carried through WebKit's answer.
struct SourceRequest {
    int tabId { 0 };
};

std::wstring temporaryDirectory()
{
    wchar_t buffer[MAX_PATH + 1] { };
    const DWORD length = GetTempPathW(MAX_PATH, buffer);
    if (!length)
        return { };
    std::wstring path(buffer, length);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

// A name no two dumps can share: the tab it came from, the process that wrote
// it, and a counter that never repeats within that process.
std::wstring dumpPath(int tabId)
{
    const std::wstring directory = temporaryDirectory();
    if (directory.empty())
        return { };

    static std::atomic<unsigned> serial { 0 };
    return directory
        + L"\\HarmonyBrowser-source-"
        + std::to_wstring(tabId)
        + L"-"
        + std::to_wstring(GetCurrentProcessId())
        + L"-"
        + std::to_wstring(serial.fetch_add(1))
        + L".txt";
}

bool writeDump(const std::wstring& path, const std::string& source)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    // A byte order mark, so the text this document is rendered as is read back as
    // the UTF-8 WebKit handed over rather than as the machine's code page.
    const char bom[] = { '\xEF', '\xBB', '\xBF' };
    DWORD written = 0;
    bool ok = WriteFile(file, bom, static_cast<DWORD>(sizeof(bom)), &written, nullptr) && written == sizeof(bom);
    if (ok && !source.empty()) {
        written = 0;
        ok = WriteFile(file, source.data(), static_cast<DWORD>(source.size()), &written, nullptr)
            && written == source.size();
    }
    CloseHandle(file);

    if (!ok)
        DeleteFileW(path.c_str());
    return ok;
}

bool isUnreservedUrlByte(unsigned char byte)
{
    if (byte >= 'A' && byte <= 'Z')
        return true;
    if (byte >= 'a' && byte <= 'z')
        return true;
    if (byte >= '0' && byte <= '9')
        return true;
    return byte == '-' || byte == '.' || byte == '_' || byte == '~';
}

// A Windows path as the file URL WebKit turns back into a path. The drive colon
// and the separators stay literal; everything else a temporary directory's name
// may hold is percent-encoded.
std::string fileURLFromPath(const std::wstring& path)
{
    const std::string narrowed = narrow(path);
    if (narrowed.empty())
        return { };

    static const char kHex[] = "0123456789ABCDEF";
    std::string url = "file:///";
    for (const char raw : narrowed) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        if (byte == '\\' || byte == '/') {
            url.push_back('/');
            continue;
        }
        if (isUnreservedUrlByte(byte) || byte == ':') {
            url.push_back(static_cast<char>(byte));
            continue;
        }
        url.push_back('%');
        url.push_back(kHex[byte >> 4]);
        url.push_back(kHex[byte & 0x0F]);
    }
    return url;
}

void sourceArrived(WKStringRef source, WKErrorRef error, void* context)
{
    SourceRequest* request = static_cast<SourceRequest*>(context);
    if (!request)
        return;
    const int tabId = request->tabId;
    delete request;

    if (error || !source) {
        setError("this page would not give up its source");
        return;
    }

    const std::wstring path = dumpPath(tabId);
    if (path.empty()) {
        setError("there is nowhere to write this page's source");
        return;
    }

    if (!writeDump(path, textOfString(source))) {
        setError("this page's source could not be written out");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        g_dumps.push_back(path);
    }

    // A source listing is a document, and a document opens in a tab. The host
    // opens it, because the tab list is the host's.
    queueCommand(HB_MENU_COMMAND_OPEN_IN_NEW_TAB, fileURLFromPath(path), tabId);
}

} // namespace

void showPageSource(WKPageRef page)
{
    const WebKitApi* api = webKitApi();
    if (!api || !page)
        return;

    WKFrameRef frame = api->pageGetMainFrame(page);
    if (!frame) {
        setError("this page has no main frame to read the source of");
        return;
    }

    auto* request = new SourceRequest;
    request->tabId = g_context.tabId;
    api->pageGetSourceForFrame(page, frame, request, sourceArrived);
}

void discardPageSources()
{
    std::vector<std::wstring> dumps;
    {
        std::lock_guard<std::mutex> lock(g_dumpMutex);
        dumps.swap(g_dumps);
    }
    for (const std::wstring& path : dumps)
        DeleteFileW(path.c_str());
}

} // namespace harmony_menus
