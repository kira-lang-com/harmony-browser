#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "harmony_paths.h"

#include <iterator>

namespace harmony::paths {

std::wstring parentDirectory(const std::wstring& path)
{
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return { };
    return path.substr(0, slash);
}

bool pathExists(const std::wstring& path)
{
    return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool fileExists(const std::wstring& path)
{
    if (path.empty())
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool directoryExists(const std::wstring& path)
{
    if (path.empty())
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool makeDirectories(const std::wstring& path)
{
    if (path.empty())
        return false;
    if (directoryExists(path))
        return true;

    // A drive root and a UNC share are created by whoever mounted them; only
    // what is below them is this browser's to make.
    const std::wstring parent = parentDirectory(path);
    if (!parent.empty() && parent != path && parent.size() > 2 && !directoryExists(parent))
        makeDirectories(parent);

    if (CreateDirectoryW(path.c_str(), nullptr))
        return true;
    // Another thread or another process may have created it in between, which is
    // the same answer as having created it here.
    return GetLastError() == ERROR_ALREADY_EXISTS && directoryExists(path);
}

std::wstring executableDirectory()
{
    // The extended path limit, because a module may be loaded from below one.
    wchar_t buffer[32768] { };
    const DWORD length = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (!length || length >= std::size(buffer))
        return { };
    return parentDirectory(std::wstring(buffer, length));
}

} // namespace harmony::paths
