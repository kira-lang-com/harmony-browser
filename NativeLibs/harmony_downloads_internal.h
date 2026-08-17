#ifndef HARMONY_DOWNLOADS_INTERNAL_H
#define HARMONY_DOWNLOADS_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

#include "harmony_paths.h"
#include "harmony_text.h"

namespace harmony_downloads {

// WebKit's C API is all opaque const pointers, and this shim resolves it out of
// the already-loaded WebKit2.dll rather than linking against it, so one alias
// per ref keeps the call sites readable without pulling in WebKit's headers.
using WKTypeRef = const void*;
using WKDownloadRef = const void*;
using WKStringRef = const void*;
using WKURLRef = const void*;
using WKURLRequestRef = const void*;
using WKURLResponseRef = const void*;
using WKNavigationActionRef = const void*;
using WKNavigationResponseRef = const void*;
using WKFrameInfoRef = const void*;
using WKErrorRef = const void*;
using WKDataRef = const void*;
using WKAuthenticationChallengeRef = const void*;

enum class State : int {
    Starting = 0,
    Running = 1,
    Finished = 2,
    Failed = 3,
    Cancelled = 4,
};

bool isTerminal(State state);

// One download, live or remembered. A record loaded from the history has no
// WebKit object behind it and is otherwise indistinguishable from a download
// that finished this session.
struct Record {
    int id { 0 };
    State state { State::Starting };

    // Retained while the download is live, released and cleared the moment it
    // reaches a terminal state. Only the WebKit thread may release it.
    WKDownloadRef download { nullptr };

    std::string url;
    std::string fileName;
    std::string path;
    std::wstring nativePath;
    std::string mimeType;
    std::string error;

    long long received { 0 };
    long long total { -1 };

    // FILETIME ticks, 0 until the download reaches a terminal state.
    unsigned long long completedAt { 0 };
};

// --- Text --------------------------------------------------------------------

using harmony::text::narrow;
using harmony::text::widen;

// FILETIME ticks as a date and time in the user's locale, empty for 0.
std::string formatTimestamp(unsigned long long ticks);

// --- Filesystem --------------------------------------------------------------

using harmony::paths::parentDirectory;

// Where the shell says downloads go, falling back to the profile's Downloads
// folder and then to a folder beside this executable. Created if missing.
std::wstring downloadsDirectory();

// Where this application keeps what it remembers between runs. Created if
// missing; empty when it could not be.
std::wstring applicationDataDirectory();

// A file name the shell will accept, taken from what the server suggested and
// falling back to the URL's last path component.
std::wstring sanitizedFileName(const std::string& suggested, const std::string& url);

// `directory\name`, with " (2)", " (3)" and so on inserted before the extension
// until nothing is overwritten.
std::wstring uniqueDestination(const std::wstring& directory, const std::wstring& name);

unsigned long long nowTicks();

// --- History -----------------------------------------------------------------

// The terminal records, newest first, written atomically. Reading a file that
// is missing, truncated or from an unknown version yields an empty list rather
// than a failure: history is what the browser remembers, not what it needs.
void writeHistory(const std::vector<Record>& records);
std::vector<Record> readHistory();

// --- Shell -------------------------------------------------------------------

// Both spawn a thread that initializes OLE, acts, and exits: a shell verb runs
// arbitrary handler code, and the frame thread cannot wait for it.
void revealInExplorer(const std::wstring& path);
void openWithShell(const std::wstring& path);

// --- Model -------------------------------------------------------------------

// The record store, shared by the WebKit thread and the host's frame thread.
// Every function locks for as long as it touches the records and no longer: no
// WebKit call and no disk write happens with the lock held.
namespace model {

// Adds a live download and returns its id. The WebKit object must already be
// retained by the caller.
int add(std::string url, WKDownloadRef download, std::string fileName);

int revision();
int count();
int idAt(int index);
int activeCount();
int stateOf(int id);
long long receivedBytes(int id);
long long totalBytes(int id);

// The text of one field, and where the file is, for the shell.
std::string textOf(int id, int field);
std::wstring nativePathOf(int id, bool finishedOnly);

void setURL(int id, std::string url);
void setDestination(
    int id,
    std::string fileName,
    std::string path,
    std::wstring nativePath,
    std::string mimeType
);
void setProgress(int id, long long received, long long total);

// Moves a record to a terminal state and writes the history. Returns the WebKit
// object that was behind it for the caller to release, or null when the record
// was already terminal or unknown.
WKDownloadRef finish(int id, State state, std::string error);

// Clears and returns every live download's WebKit object.
std::vector<WKDownloadRef> takeLiveDownloads();

// Forgets one terminal record, or all of them, and rewrites the history. A
// running download is never forgotten: it is cancelled.
void remove(int id);
void clearTerminal();

// Cancel is the only thing the host asks the WebKit thread to do. It is drained
// by hb_downloads_pump, which the tabs registry runs once per WebKit cycle.
void queueCancel(int id);
bool nextCancel(int& id);

// The download that is queued against an id, or null when it has none.
WKDownloadRef liveDownload(int id);

void forceURL(std::string url);
bool consumeForcedURL(const std::string& url);

}

}

#endif
