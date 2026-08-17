#ifndef HARMONY_PATHS_H
#define HARMONY_PATHS_H

#include <string>

// What every module asks the filesystem.
//
// Four questions -- what is above this path, is it a file, is it a directory,
// and make it exist -- plus where this executable lives. They were once answered
// by a copy in each module, which is how two modules come to disagree about
// whether a trailing separator names a directory. One answer each, here.
namespace harmony::paths {

// Everything before the last separator, empty when the path holds none.
std::wstring parentDirectory(const std::wstring& path);

// Whether the path names something at all, a file, or a directory. A path that
// cannot be read answers false rather than throwing the caller a distinction it
// has no use for.
bool pathExists(const std::wstring& path);
bool fileExists(const std::wstring& path);
bool directoryExists(const std::wstring& path);

// Creates the path and every directory above it. True when the directory exists
// afterwards, whoever created it.
bool makeDirectories(const std::wstring& path);

// The directory this executable was loaded from, empty when it cannot be named.
std::wstring executableDirectory();

} // namespace harmony::paths

#endif
