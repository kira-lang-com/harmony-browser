#include "harmony_dialogs_webkit.h"

#include "harmony_text.h"

#include <objbase.h>
#include <shobjidl.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace harmony_dialogs {
namespace {

// The shell's own class and interface identifiers, spelled here rather than
// linked from uuid.lib: the whole picker is three interfaces, and naming them
// costs less than a library dependency that exists to hold four constants.
const CLSID kFileOpenDialogClass = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
const IID kFileOpenDialogInterface = { 0xD57C7288, 0xD4AD, 0x4768, { 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60 } };

std::mutex g_mutex;
std::deque<FilePickerRequest> g_queue;
HANDLE g_event { nullptr };
HANDLE g_thread { nullptr };
std::atomic<bool> g_stopping { false };

// --- Text --------------------------------------------------------------------

using harmony::text::narrow;
using harmony::text::widen;

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    });
    return text;
}

bool isUnreservedUrlByte(unsigned char ch)
{
    return (ch >= 'A' && ch <= 'Z')
        || (ch >= 'a' && ch <= 'z')
        || (ch >= '0' && ch <= '9')
        || ch == '-' || ch == '.' || ch == '_' || ch == '~';
}

// A Windows path as the file URL WebKit turns back into a path. The drive
// colon and the separators stay literal; everything else a file name may hold —
// spaces, '#', '?', anything non-ASCII — is percent-encoded.
std::string fileUrlFromPath(const std::string& path)
{
    if (path.empty())
        return std::string();

    std::string rest = path;
    std::string url = "file:///";
    if (rest.size() > 1 && (rest[0] == '\\' || rest[0] == '/') && (rest[1] == '\\' || rest[1] == '/')) {
        // A UNC path is an authority, not a rooted local path.
        url = "file://";
        rest = rest.substr(2);
    }

    for (const char raw : rest) {
        const unsigned char ch = static_cast<unsigned char>(raw);
        if (ch == '\\' || ch == '/') {
            url.push_back('/');
            continue;
        }
        if (isUnreservedUrlByte(ch) || ch == ':') {
            url.push_back(static_cast<char>(ch));
            continue;
        }
        static const char kHex[] = "0123456789ABCDEF";
        url.push_back('%');
        url.push_back(kHex[ch >> 4]);
        url.push_back(kHex[ch & 0x0F]);
    }
    return url;
}

// --- accept -> file types ----------------------------------------------------

struct MimeExtensions {
    const char* mime;
    const char* extensions;
};

// The mapping an `accept` attribute needs to become a shell filter. A browser
// that answers `accept="image/*"` with "All Files" makes the attribute a lie,
// so the wildcard families are spelled out rather than dropped.
constexpr MimeExtensions kMimeExtensions[] = {
    { "image/*", "jpg;jpeg;png;gif;bmp;webp;svg;ico;tif;tiff;avif;heic" },
    { "video/*", "mp4;webm;mov;avi;mkv;m4v;ogv;mpg;mpeg" },
    { "audio/*", "mp3;wav;ogg;m4a;flac;aac;opus;wma" },
    { "text/*", "txt;csv;html;htm;css;js;json;xml;md" },
    { "image/png", "png" },
    { "image/jpeg", "jpg;jpeg" },
    { "image/gif", "gif" },
    { "image/webp", "webp" },
    { "image/bmp", "bmp" },
    { "image/svg+xml", "svg" },
    { "image/x-icon", "ico" },
    { "image/tiff", "tif;tiff" },
    { "image/avif", "avif" },
    { "image/heic", "heic" },
    { "video/mp4", "mp4;m4v" },
    { "video/webm", "webm" },
    { "video/quicktime", "mov" },
    { "audio/mpeg", "mp3" },
    { "audio/wav", "wav" },
    { "audio/ogg", "ogg;opus" },
    { "audio/flac", "flac" },
    { "text/plain", "txt" },
    { "text/html", "html;htm" },
    { "text/csv", "csv" },
    { "text/css", "css" },
    { "text/javascript", "js;mjs" },
    { "text/markdown", "md" },
    { "application/json", "json" },
    { "application/xml", "xml" },
    { "text/xml", "xml" },
    { "application/pdf", "pdf" },
    { "application/zip", "zip" },
    { "application/gzip", "gz" },
    { "application/msword", "doc" },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", "docx" },
    { "application/vnd.ms-excel", "xls" },
    { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", "xlsx" },
    { "application/vnd.ms-powerpoint", "ppt" },
    { "application/vnd.openxmlformats-officedocument.presentationml.presentation", "pptx" },
    { "font/ttf", "ttf" },
    { "font/otf", "otf" },
    { "font/woff", "woff" },
    { "font/woff2", "woff2" },
};

void appendExtension(std::vector<std::string>& into, std::string extension)
{
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    if (extension.empty() || extension == "*")
        return;
    extension = lowered(std::move(extension));
    if (std::find(into.begin(), into.end(), extension) == into.end())
        into.push_back(std::move(extension));
}

void appendSemicolonList(std::vector<std::string>& into, const char* list)
{
    std::string current;
    for (const char* cursor = list;; ++cursor) {
        if (*cursor && *cursor != ';') {
            current.push_back(*cursor);
            continue;
        }
        appendExtension(into, std::move(current));
        current.clear();
        if (!*cursor)
            return;
    }
}

std::vector<std::string> acceptedExtensions(const FilePickerRequest& request)
{
    std::vector<std::string> extensions;
    for (const auto& extension : request.extensions)
        appendExtension(extensions, extension);
    for (const auto& mime : request.mimeTypes) {
        const std::string wanted = lowered(mime);
        for (const auto& entry : kMimeExtensions) {
            if (wanted == entry.mime) {
                appendSemicolonList(extensions, entry.extensions);
                break;
            }
        }
    }
    return extensions;
}

// --- The dialog --------------------------------------------------------------

std::vector<std::string> pathsFromShellItem(IShellItem* item)
{
    std::vector<std::string> paths;
    if (!item)
        return paths;
    PWSTR display = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &display)) && display) {
        std::string path = narrow(display);
        if (!path.empty())
            paths.push_back(std::move(path));
        CoTaskMemFree(display);
    }
    return paths;
}

std::vector<std::string> showPicker(const FilePickerRequest& request)
{
    std::vector<std::string> chosen;

    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(
        kFileOpenDialogClass,
        nullptr,
        CLSCTX_INPROC_SERVER,
        kFileOpenDialogInterface,
        reinterpret_cast<void**>(&dialog)
    );
    if (FAILED(result) || !dialog) {
        setDialogsError("the Windows file dialog could not be created");
        return chosen;
    }

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        options = 0;
    options |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (request.allowsMultiple)
        options |= FOS_ALLOWMULTISELECT;
    if (request.allowsDirectories)
        options |= FOS_PICKFOLDERS;
    if (FAILED(dialog->SetOptions(options))) {
        // Without these the dialog would return shell items that are not files,
        // and a page would be handed a path to something that has none.
        setDialogsError("the Windows file dialog refused the options a page asked for");
        dialog->Release();
        return chosen;
    }

    // The filter strings have to outlive Show, so they are built here and the
    // spec array only points into them.
    std::wstring supportedSpec;
    std::vector<COMDLG_FILTERSPEC> filters;
    if (!request.allowsDirectories) {
        const auto extensions = acceptedExtensions(request);
        for (const auto& extension : extensions) {
            if (!supportedSpec.empty())
                supportedSpec.push_back(L';');
            supportedSpec += L"*." + widen(extension);
        }
        if (!supportedSpec.empty())
            filters.push_back(COMDLG_FILTERSPEC { L"Supported Files", supportedSpec.c_str() });
        filters.push_back(COMDLG_FILTERSPEC { L"All Files", L"*.*" });
        // A refused filter is not worth abandoning the dialog over: the picker
        // still picks, it just offers every file rather than the accepted ones.
        if (SUCCEEDED(dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data())))
            (void)dialog->SetFileTypeIndex(1);
    }

    result = dialog->Show(request.owner);
    if (FAILED(result)) {
        // A cancelled dialog is not a failure; anything else is worth naming.
        if (result != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            setDialogsError("the Windows file dialog failed to open");
        dialog->Release();
        return chosen;
    }

    if (request.allowsMultiple) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dialog->GetResults(&items)) && items) {
            DWORD count = 0;
            if (FAILED(items->GetCount(&count)))
                count = 0;
            for (DWORD index = 0; index < count; ++index) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(index, &item)) || !item)
                    continue;
                for (auto& path : pathsFromShellItem(item))
                    chosen.push_back(std::move(path));
                item->Release();
            }
            items->Release();
        }
    } else {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            chosen = pathsFromShellItem(item);
            item->Release();
        }
    }

    dialog->Release();

    std::vector<std::string> urls;
    urls.reserve(chosen.size());
    for (const auto& path : chosen)
        urls.push_back(fileUrlFromPath(path));
    return urls;
}

DWORD WINAPI pickerThreadMain(LPVOID)
{
    // The shell dialog is an apartment-threaded object, and this apartment is
    // this thread's alone: WebKit's thread must stay free to run its loop while
    // a picker is open, and the host's must stay free to draw.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized && comResult != RPC_E_CHANGED_MODE) {
        setDialogsError("the file dialog thread could not enter a COM apartment");
        return 0;
    }

    for (;;) {
        FilePickerRequest request;
        bool queued = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_queue.empty()) {
                request = std::move(g_queue.front());
                g_queue.pop_front();
                queued = true;
            } else if (g_stopping.load())
                break;
        }

        if (!queued) {
            WaitForSingleObject(g_event, INFINITE);
            continue;
        }
        if (g_stopping.load()) {
            filePickerCompleted(request.id, std::vector<std::string>());
            continue;
        }
        filePickerCompleted(request.id, showPicker(request));
    }

    if (comInitialized)
        CoUninitialize();
    return 0;
}

} // namespace

void filePickerSubmit(FilePickerRequest request)
{
    const int id = request.id;
    HANDLE wake = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_stopping.load()) {
            if (!g_event)
                g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!g_thread && g_event)
                g_thread = CreateThread(nullptr, 0, pickerThreadMain, nullptr, 0, nullptr);
            if (g_thread) {
                g_queue.push_back(std::move(request));
                wake = g_event;
            }
        }
    }

    if (wake) {
        SetEvent(wake);
        return;
    }
    // Nothing will ever run it, so the page is answered here rather than left
    // waiting on a thread that is gone or was never started.
    filePickerCompleted(id, std::vector<std::string>());
}

void filePickerShutdown()
{
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stopping.store(true);
        thread = g_thread;
        g_thread = nullptr;
        wake = g_event;
    }
    if (wake)
        SetEvent(wake);

    bool joined = true;
    if (thread) {
        // A picker that is open owns the user's attention; the wait is bounded
        // so a forgotten dialog cannot hold the process open.
        joined = WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0;
        CloseHandle(thread);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& abandoned : g_queue)
        filePickerCompleted(abandoned.id, std::vector<std::string>());
    g_queue.clear();
    // A thread still inside a dialog would fault on a closed handle, so the
    // event outlives a wait that timed out.
    if (g_event && joined) {
        CloseHandle(g_event);
        g_event = nullptr;
    }
}

} // namespace harmony_dialogs
