#include "harmony_downloads.h"
#include "harmony_downloads_internal.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <mutex>

namespace harmony_downloads {
namespace {

constexpr wchar_t kHistoryFileName[] = L"downloads.history";
constexpr char kHistoryHeader[] = "harmony-downloads 1";

// How many finished downloads are remembered between runs. A history that grows
// without bound stops being something anyone reads.
constexpr size_t kHistoryLimit = 200;

// One writer at a time, and one reader that never sees a half-written file:
// the write goes to a sibling and is moved over the real name.
std::mutex g_fileMutex;

std::string escapeField(const std::string& value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (char character : value) {
        switch (character) {
        case '\\': escaped += "\\\\"; break;
        case '\t': escaped += "\\t"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        default: escaped += character; break;
        }
    }
    return escaped;
}

std::string unescapeField(const std::string& value)
{
    std::string plain;
    plain.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '\\' || index + 1 >= value.size()) {
            plain += value[index];
            continue;
        }
        ++index;
        switch (value[index]) {
        case 't': plain += '\t'; break;
        case 'n': plain += '\n'; break;
        case 'r': plain += '\r'; break;
        default: plain += value[index]; break;
        }
    }
    return plain;
}

std::vector<std::string> splitFields(const std::string& line)
{
    std::vector<std::string> fields;
    std::string current;
    for (char character : line) {
        if (character == '\t') {
            fields.push_back(current);
            current.clear();
            continue;
        }
        current += character;
    }
    fields.push_back(current);
    return fields;
}

std::vector<std::string> splitLines(const std::string& contents)
{
    std::vector<std::string> lines;
    std::string current;
    for (char character : contents) {
        if (character == '\n') {
            lines.push_back(current);
            current.clear();
            continue;
        }
        if (character != '\r')
            current += character;
    }
    if (!current.empty())
        lines.push_back(current);
    return lines;
}

long long parseSigned(const std::string& value)
{
    long long parsed = 0;
    bool negative = false;
    size_t index = 0;
    if (index < value.size() && (value[index] == '-' || value[index] == '+')) {
        negative = value[index] == '-';
        ++index;
    }
    for (; index < value.size(); ++index) {
        if (value[index] < '0' || value[index] > '9')
            break;
        parsed = parsed * 10 + (value[index] - '0');
    }
    return negative ? -parsed : parsed;
}

unsigned long long parseUnsigned(const std::string& value)
{
    unsigned long long parsed = 0;
    for (char character : value) {
        if (character < '0' || character > '9')
            break;
        parsed = parsed * 10 + static_cast<unsigned long long>(character - '0');
    }
    return parsed;
}

std::wstring historyFilePath()
{
    const std::wstring directory = applicationDataDirectory();
    if (directory.empty())
        return { };
    return directory + L"\\" + kHistoryFileName;
}

bool writeWholeFile(const std::wstring& path, const std::string& contents)
{
    const std::wstring temporary = path + L".tmp";
    HANDLE file = CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return false;

    bool ok = true;
    size_t written = 0;
    while (written < contents.size()) {
        DWORD chunk = 0;
        const DWORD remaining = static_cast<DWORD>(
            std::min<size_t>(contents.size() - written, 1u << 20)
        );
        if (!WriteFile(file, contents.data() + written, remaining, &chunk, nullptr) || !chunk) {
            ok = false;
            break;
        }
        written += chunk;
    }
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::string readWholeFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (file == INVALID_HANDLE_VALUE)
        return { };

    std::string contents;
    char buffer[16384];
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) || !read)
            break;
        contents.append(buffer, read);
    }
    CloseHandle(file);
    return contents;
}

struct ShellRequest {
    std::wstring path;
    bool reveal { false };
};

DWORD WINAPI shellThreadMain(LPVOID parameter)
{
    std::unique_ptr<ShellRequest> request(static_cast<ShellRequest*>(parameter));
    const HRESULT initialized = OleInitialize(nullptr);

    if (request->reveal) {
        PIDLIST_ABSOLUTE item = ILCreateFromPathW(request->path.c_str());
        if (item) {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            ILFree(item);
        } else {
            const std::wstring folder = parentDirectory(request->path);
            if (!folder.empty())
                ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else {
        ShellExecuteW(nullptr, nullptr, request->path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    if (SUCCEEDED(initialized))
        OleUninitialize();
    return 0;
}

void runShellRequest(std::wstring path, bool reveal)
{
    if (path.empty())
        return;

    auto request = std::make_unique<ShellRequest>();
    request->path = std::move(path);
    request->reveal = reveal;

    // Handed over before the thread starts, because from the moment it does the
    // request is the thread's to delete. A thread that failed to start deletes
    // it here instead: ownership moves exactly once, either way.
    ShellRequest* owned = request.release();
    HANDLE thread = CreateThread(nullptr, 0, shellThreadMain, owned, 0, nullptr);
    if (!thread) {
        delete owned;
        return;
    }

    // Nothing waits for it: a shell verb runs a handler this process does not
    // own, and a frame must not be behind one.
    CloseHandle(thread);
}

}

void writeHistory(const std::vector<Record>& records)
{
    const std::wstring path = historyFilePath();
    if (path.empty())
        return;

    std::string contents = kHistoryHeader;
    contents += '\n';

    size_t written = 0;
    for (const Record& record : records) {
        if (!isTerminal(record.state))
            continue;
        if (written >= kHistoryLimit)
            break;

        contents += std::to_string(static_cast<int>(record.state));
        contents += '\t';
        contents += std::to_string(record.received);
        contents += '\t';
        contents += std::to_string(record.total);
        contents += '\t';
        contents += std::to_string(record.completedAt);
        contents += '\t';
        contents += escapeField(record.url);
        contents += '\t';
        contents += escapeField(record.fileName);
        contents += '\t';
        contents += escapeField(record.path);
        contents += '\t';
        contents += escapeField(record.mimeType);
        contents += '\t';
        contents += escapeField(record.error);
        contents += '\n';
        ++written;
    }

    std::lock_guard<std::mutex> lock(g_fileMutex);
    if (writeWholeFile(path, contents))
        return;

    // The previous history is still on disk: writeWholeFile replaces the real
    // file only once the whole of the new one has been written, and removes what
    // it half-wrote otherwise. What is lost is this change, and a browser that
    // silently stopped remembering its downloads would look like one that never
    // did.
    std::fprintf(stderr, "harmony: downloads: the download history at %s could not be written\n",
        narrow(path).c_str());
}

std::vector<Record> readHistory()
{
    const std::wstring path = historyFilePath();
    if (path.empty())
        return { };

    std::string contents;
    {
        std::lock_guard<std::mutex> lock(g_fileMutex);
        contents = readWholeFile(path);
    }
    if (contents.empty())
        return { };

    const std::vector<std::string> lines = splitLines(contents);
    if (lines.empty() || lines.front() != kHistoryHeader)
        return { };

    std::vector<Record> records;
    for (size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty())
            continue;

        const std::vector<std::string> fields = splitFields(lines[index]);
        if (fields.size() < 9)
            continue;

        const long long state = parseSigned(fields[0]);
        if (state < HB_DOWNLOAD_STATE_FINISHED || state > HB_DOWNLOAD_STATE_CANCELLED)
            continue;

        Record record;
        record.state = static_cast<State>(static_cast<int>(state));
        record.received = parseSigned(fields[1]);
        record.total = parseSigned(fields[2]);
        record.completedAt = parseUnsigned(fields[3]);
        record.url = unescapeField(fields[4]);
        record.fileName = unescapeField(fields[5]);
        record.path = unescapeField(fields[6]);
        record.mimeType = unescapeField(fields[7]);
        record.error = unescapeField(fields[8]);
        record.nativePath = widen(record.path);
        records.push_back(std::move(record));

        if (records.size() >= kHistoryLimit)
            break;
    }
    return records;
}

void revealInExplorer(const std::wstring& path)
{
    runShellRequest(path, true);
}

void openWithShell(const std::wstring& path)
{
    runShellRequest(path, false);
}

}
