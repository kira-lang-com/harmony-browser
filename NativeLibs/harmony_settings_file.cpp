#include "harmony_settings_internal.h"

#include "harmony_data_store_internal.h"

#include <cstdlib>

// Where the settings live on disk, and the two directions they cross it in.
//
// The directory is the data store's, asked for through its own layout: this
// browser keeps ONE directory under %LOCALAPPDATA%, and a second module that
// derived the same path again would be a second module that could be moved
// without the first following.
//
// The format is one `key<TAB>value` line per setting under a version line.
// Anything the reader does not recognise is skipped rather than refused, so a
// file written by a newer build still yields every setting this one knows.

namespace harmony::settings {

namespace {

constexpr wchar_t kSettingsFileName[] = L"\\Settings.txt";
constexpr char kSettingsHeader[] = "harmony-settings 1";

// How long a run of edits settles before the file is written. Dragging a zoom
// level or typing a home address should not write a file per keystroke.
constexpr ULONGLONG kSaveDelayMs = 800;

std::wstring g_path;
std::string g_pathUtf8;
bool g_pathResolved = false;

std::atomic<bool> g_dirty { false };
std::atomic<ULONGLONG> g_dirtySince { 0 };

void resolvePath()
{
    if (g_pathResolved)
        return;
    g_pathResolved = true;

    // The data store's own layout, prepared here when the engine has not
    // reached it yet. This runs on the frame thread before the engine thread
    // exists -- see the ORDER note in harmony_settings.h -- so the two callers
    // of prepareLayout never overlap.
    if (!harmony::datastore::prepareLayout()) {
        setError("the profile directory could not be created, so settings cannot be saved");
        return;
    }

    const std::wstring& root = harmony::datastore::layout().root;
    if (root.empty()) {
        setError("the profile directory could not be named, so settings cannot be saved");
        return;
    }

    g_path = root + kSettingsFileName;
    g_pathUtf8 = narrow(g_path);
}

std::string readWholeFile(const std::wstring& path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return { };

    std::string contents;
    char buffer[8192];
    DWORD read = 0;
    while (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) && read > 0)
        contents.append(buffer, read);
    CloseHandle(file);
    return contents;
}

std::vector<std::string> splitLines(const std::string& contents)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= contents.size()) {
        size_t end = contents.find('\n', start);
        if (end == std::string::npos)
            end = contents.size();

        std::string line = contents.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

bool isTruth(const std::string& value)
{
    return value == "on";
}

const char* spellTruth(bool value)
{
    return value ? "on" : "off";
}

// --- Reading ------------------------------------------------------------------

// One line's key and value. A line with no tab carries a key and nothing else,
// which is how a value cleared to empty comes back.
void splitLine(const std::string& line, std::string& key, std::string& value)
{
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
        key = line;
        value.clear();
        return;
    }
    key = line.substr(0, tab);
    value = line.substr(tab + 1);
}

void readEngine(Values& values, const std::string& value)
{
    const size_t tab = value.find('\t');
    if (tab == std::string::npos)
        return;

    SearchEngine engine;
    engine.name = value.substr(0, tab);
    engine.query = value.substr(tab + 1);
    if (engine.name.empty() || !isUsableQueryTemplate(engine.query))
        return;
    values.engines.push_back(std::move(engine));
}

void chooseDefaultEngine(Values& values, const std::string& name)
{
    for (size_t index = 0; index < values.engines.size(); ++index) {
        if (values.engines[index].name == name) {
            values.defaultEngine = static_cast<int>(index);
            return;
        }
    }
}

} // namespace

const std::wstring& settingsFilePath()
{
    resolvePath();
    return g_path;
}

const std::string& settingsFilePathUtf8()
{
    resolvePath();
    return g_pathUtf8;
}

void loadSettings()
{
    const std::wstring& path = settingsFilePath();
    if (path.empty())
        return;

    const std::string contents = readWholeFile(path);
    if (contents.empty())
        return;

    const std::vector<std::string> lines = splitLines(contents);
    if (lines.empty() || lines.front() != kSettingsHeader) {
        // A file this build cannot read is left exactly as it is: overwriting it
        // with defaults would throw away settings a newer build still wants.
        setError("the settings file was written by a version this build does not read");
        return;
    }

    Values values;
    // The engines named by the file replace the built-in list wholesale, so an
    // engine a person removed does not come back on the next run.
    values.engines.clear();
    std::string chosenEngine;

    for (size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty())
            continue;

        std::string key;
        std::string value;
        splitLine(lines[index], key, value);

        if (key == "home")
            values.homeURL = value.empty() ? kDefaultHomeURL : value;
        else if (key == "startup")
            values.startup = value == "new-tab" ? HB_SETTINGS_STARTUP_NEW_TAB : HB_SETTINGS_STARTUP_RESTORE_SESSION;
        else if (key == "zoom")
            values.zoomPercent = std::atoi(value.c_str());
        else if (key == "javascript")
            values.javaScript = isTruth(value);
        else if (key == "images")
            values.images = isTruth(value);
        else if (key == "popups")
            values.popups = isTruth(value);
        else if (key == "do-not-track")
            values.doNotTrack = isTruth(value);
        else if (key == "downloads")
            values.downloadDirectory = value;
        else if (key == "engine")
            readEngine(values, value);
        else if (key == "default-engine")
            chosenEngine = value;
    }

    if (values.zoomPercent < kMinimumZoomPercent || values.zoomPercent > kMaximumZoomPercent)
        values.zoomPercent = kDefaultZoomPercent;
    // A file that named no engine this build could parse still has to answer a
    // typed phrase, so the built-in list stands in for one that came back empty.
    if (values.engines.empty())
        values.engines = defaultEngines();
    chooseDefaultEngine(values, chosenEngine);
    if (values.defaultEngine < 0 || values.defaultEngine >= static_cast<int>(values.engines.size()))
        values.defaultEngine = 0;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_values = std::move(values);
}

void markDirty()
{
    g_dirty.store(true);
    g_dirtySince.store(GetTickCount64());
}

void serviceSave()
{
    if (!g_dirty.load())
        return;
    if (GetTickCount64() - g_dirtySince.load() < kSaveDelayMs)
        return;
    flushSave();
}

void flushSave()
{
    if (!g_dirty.exchange(false))
        return;

    const std::wstring& path = settingsFilePath();
    if (path.empty())
        return;

    std::string contents = kSettingsHeader;
    contents += '\n';
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        contents += "home\t" + g_values.homeURL + "\n";
        contents += std::string("startup\t")
            + (g_values.startup == HB_SETTINGS_STARTUP_NEW_TAB ? "new-tab" : "restore-session")
            + "\n";
        contents += "zoom\t" + std::to_string(g_values.zoomPercent) + "\n";
        contents += std::string("javascript\t") + spellTruth(g_values.javaScript) + "\n";
        contents += std::string("images\t") + spellTruth(g_values.images) + "\n";
        contents += std::string("popups\t") + spellTruth(g_values.popups) + "\n";
        contents += std::string("do-not-track\t") + spellTruth(g_values.doNotTrack) + "\n";
        contents += "downloads\t" + g_values.downloadDirectory + "\n";

        for (const SearchEngine& engine : g_values.engines)
            contents += "engine\t" + engine.name + "\t" + engine.query + "\n";

        // The chosen engine is named rather than numbered: an index would point
        // at whichever engine happened to take that place after an edit.
        if (g_values.defaultEngine >= 0 && g_values.defaultEngine < static_cast<int>(g_values.engines.size()))
            contents += "default-engine\t" + g_values.engines[static_cast<size_t>(g_values.defaultEngine)].name + "\n";
    }

    // Written beside the real file and moved over it, so a run that ends
    // mid-write leaves the last complete set of settings rather than half of a
    // new one.
    const std::wstring temporary = path + L".new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError("the settings could not be written");
        return;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
    CloseHandle(file);

    if (!ok || written != contents.size()) {
        DeleteFileW(temporary.c_str());
        setError("the settings could not be written");
        return;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        setError("the settings could not be replaced");
    }
}

} // namespace harmony::settings
