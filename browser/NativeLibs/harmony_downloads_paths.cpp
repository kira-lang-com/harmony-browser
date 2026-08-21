#include "harmony_downloads_internal.h"

#include <objbase.h>
#include <shlobj.h>

#include <cwctype>
#include <mutex>

namespace harmony_downloads {
namespace {

// The known folders this shim asks for, spelled out rather than taken from
// uuid.lib so the link line stays kernel32/user32/ole32/shell32.
const KNOWNFOLDERID kFolderDownloads = {
    0x374DE290, 0x123F, 0x4565, { 0x91, 0x64, 0x39, 0xC4, 0x92, 0x5E, 0x46, 0x7B }
};
const KNOWNFOLDERID kFolderRoamingAppData = {
    0x3EB685DB, 0x65F9, 0x4CF6, { 0xA0, 0x3A, 0xE3, 0xEF, 0x65, 0x72, 0x9F, 0x3D }
};
const KNOWNFOLDERID kFolderProfile = {
    0x5E6C858F, 0x0E22, 0x4760, { 0x9A, 0xFE, 0xEA, 0x33, 0x17, 0xB6, 0x71, 0x73 }
};

constexpr wchar_t kApplicationFolderName[] = L"HarmonyBrowser";

// Windows accepts a 255-character path component. A name at the limit loses its
// middle rather than its extension, because the extension is what decides which
// application opens the file.
constexpr size_t kMaxComponentLength = 200;

std::wstring knownFolder(const KNOWNFOLDERID& id, DWORD flags)
{
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, flags, nullptr, &raw);
    if (FAILED(result) || !raw) {
        if (raw)
            CoTaskMemFree(raw);
        return { };
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

bool isReservedDeviceName(const std::wstring& stem)
{
    static const wchar_t* const reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
        L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9",
    };

    std::wstring upper;
    upper.reserve(stem.size());
    for (wchar_t character : stem)
        upper += static_cast<wchar_t>(towupper(character));

    for (const wchar_t* name : reserved) {
        if (upper == name)
            return true;
    }
    return false;
}

int hexDigit(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character >= 'a' && character <= 'f')
        return character - 'a' + 10;
    if (character >= 'A' && character <= 'F')
        return character - 'A' + 10;
    return -1;
}

using harmony::paths::directoryExists;
using harmony::paths::executableDirectory;
using harmony::paths::fileExists;
using harmony::paths::makeDirectories;

// The name a URL implies: its last path component, without the query, with its
// percent escapes resolved back to the bytes they stand for.
std::wstring lastPathComponent(const std::string& url)
{
    std::string value = url;
    const auto query = value.find_first_of("?#");
    if (query != std::string::npos)
        value.erase(query);
    while (!value.empty() && (value.back() == '/' || value.back() == '\\'))
        value.pop_back();

    const auto slash = value.find_last_of("/\\");
    if (slash != std::string::npos)
        value = value.substr(slash + 1);

    std::string decoded;
    decoded.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '%' && index + 2 < value.size()) {
            const int high = hexDigit(value[index + 1]);
            const int low = hexDigit(value[index + 2]);
            if (high >= 0 && low >= 0) {
                decoded += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        decoded += value[index];
    }

    return widen(decoded);
}

}

std::wstring downloadsDirectory()
{
    // Which folder this is gets answered once: asking the shell is a registry
    // read, and the host asks on every frame the downloads panel is open.
    // Whether it still exists is answered every time, because it can be deleted
    // while the browser is running.
    static std::mutex cacheMutex;
    static std::wstring cached;

    std::lock_guard<std::mutex> lock(cacheMutex);
    if (!cached.empty()) {
        makeDirectories(cached);
        return cached;
    }

    std::wstring path = knownFolder(kFolderDownloads, KF_FLAG_CREATE);
    if (!path.empty() && makeDirectories(path)) {
        cached = path;
        return cached;
    }

    const std::wstring profile = knownFolder(kFolderProfile, 0);
    if (!profile.empty()) {
        path = profile + L"\\Downloads";
        if (makeDirectories(path)) {
            cached = path;
            return cached;
        }
    }

    const std::wstring beside = executableDirectory();
    if (!beside.empty()) {
        path = beside + L"\\Downloads";
        if (makeDirectories(path)) {
            cached = path;
            return cached;
        }
    }
    return { };
}

std::wstring applicationDataDirectory()
{
    const std::wstring roaming = knownFolder(kFolderRoamingAppData, KF_FLAG_CREATE);
    if (roaming.empty())
        return { };

    const std::wstring path = roaming + L"\\" + kApplicationFolderName;
    if (!makeDirectories(path))
        return { };
    return path;
}

std::wstring sanitizedFileName(const std::string& suggested, const std::string& url)
{
    std::wstring name = widen(suggested);
    if (name.empty())
        name = lastPathComponent(url);

    std::wstring clean;
    clean.reserve(name.size());
    for (wchar_t character : name) {
        if (character < 0x20 || character == 0x7F)
            continue;
        switch (character) {
        case L'<': case L'>': case L':': case L'"':
        case L'/': case L'\\': case L'|': case L'?': case L'*':
            clean += L'_';
            break;
        default:
            clean += character;
            break;
        }
    }

    while (!clean.empty() && (clean.front() == L' ' || clean.front() == L'.'))
        clean.erase(clean.begin());
    while (!clean.empty() && (clean.back() == L' ' || clean.back() == L'.'))
        clean.pop_back();

    if (clean.size() > kMaxComponentLength) {
        const auto dot = clean.find_last_of(L'.');
        std::wstring extension;
        if (dot != std::wstring::npos && clean.size() - dot <= 16)
            extension = clean.substr(dot);
        clean = clean.substr(0, kMaxComponentLength - extension.size()) + extension;
    }

    if (clean.empty())
        return L"download";

    const auto dot = clean.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? clean : clean.substr(0, dot);
    if (isReservedDeviceName(stem))
        clean = L"_" + clean;

    return clean;
}

std::wstring uniqueDestination(const std::wstring& directory, const std::wstring& name)
{
    if (directory.empty() || name.empty())
        return { };

    const auto dot = name.find_last_of(L'.');
    const bool hasExtension = dot != std::wstring::npos && dot != 0;
    const std::wstring stem = hasExtension ? name.substr(0, dot) : name;
    const std::wstring extension = hasExtension ? name.substr(dot) : std::wstring();

    std::wstring candidate = directory + L"\\" + name;
    if (!fileExists(candidate) && !directoryExists(candidate))
        return candidate;

    for (int index = 1; index < 10000; ++index) {
        candidate = directory + L"\\" + stem + L" (" + std::to_wstring(index) + L")" + extension;
        if (!fileExists(candidate) && !directoryExists(candidate))
            return candidate;
    }

    // Ten thousand files of one name means the counter is not what tells them
    // apart. The clock is.
    return directory + L"\\" + stem + L"-" + std::to_wstring(nowTicks()) + extension;
}

}
