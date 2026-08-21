#include "harmony_input_internal.h"

#include <cstdlib>
#include <iterator>

// Page zoom, remembered per site.
//
// A browser's zoom is a property of the site, not of the tab and not of the
// document: a level set on one page of a site is the level its every page opens
// at, in this tab and in any other, today and after a restart. That is why the
// levels are keyed by origin and written to the profile, and why a tab that
// navigates is checked against them rather than left at whatever the last page
// was showing at.

namespace harmony_input {

namespace {

// The ladder a step moves along, the same one every browser offers.
constexpr int kZoomSteps[] = { 25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400, 500 };
constexpr int kZoomDefault = 100;

constexpr wchar_t kZoomFileName[] = L"\\ZoomLevels.txt";
constexpr char kZoomHeader[] = "harmony-zoom 1";

// How long a run of steps settles before the levels are written out. Holding
// Ctrl+plus should not write a file per keystroke.
constexpr ULONGLONG kZoomSaveDelayMs = 1200;

std::mutex g_zoomMutex;
std::vector<std::pair<std::string, int>> g_levels;

std::atomic<bool> g_zoomDirty { false };
std::atomic<ULONGLONG> g_zoomDirtySince { 0 };

// Frame thread. What each tab was last told, so a tab is only spoken to when
// its site or its level has actually moved.
struct AppliedZoom {
    int tabId { 0 };
    std::string url;
    int percent { kZoomDefault };
};
std::vector<AppliedZoom> g_applied;
int g_appliedRevision { -1 };
int g_appliedGeneration { -1 };

// Bumped whenever a level changes. A step taken with Ctrl and the wheel is
// matched on the engine thread's pump, so the change can arrive from either
// thread; this is what tells the frame thread's scan that its picture of who
// is showing at what is out of date.
std::atomic<int> g_zoomGeneration { 0 };

struct ZoomRequest {
    int tabId { 0 };
    double factor { 1.0 };
};

int storedLevel(const std::string& origin)
{
    if (origin.empty())
        return kZoomDefault;

    std::lock_guard<std::mutex> lock(g_zoomMutex);
    for (const auto& entry : g_levels) {
        if (entry.first == origin)
            return entry.second;
    }
    return kZoomDefault;
}

void storeLevel(const std::string& origin, int percent)
{
    if (origin.empty())
        return;

    {
        std::lock_guard<std::mutex> lock(g_zoomMutex);
        for (auto& entry : g_levels) {
            if (entry.first == origin) {
                entry.second = percent;
                g_zoomDirty.store(true);
                g_zoomDirtySince.store(GetTickCount64());
                return;
            }
        }
        // A site left at the default is a site with nothing to remember, so it
        // is not written down at all.
        if (percent != kZoomDefault)
            g_levels.emplace_back(origin, percent);
    }
    g_zoomDirty.store(true);
    g_zoomDirtySince.store(GetTickCount64());
}

int clampPercent(int percent)
{
    const int lowest = kZoomSteps[0];
    const int highest = kZoomSteps[static_cast<int>(std::size(kZoomSteps)) - 1];
    if (percent < lowest)
        return lowest;
    if (percent > highest)
        return highest;
    return percent;
}

int steppedPercent(int percent, int direction)
{
    const int count = static_cast<int>(std::size(kZoomSteps));
    if (direction > 0) {
        for (int index = 0; index < count; ++index) {
            if (kZoomSteps[index] > percent)
                return kZoomSteps[index];
        }
        return kZoomSteps[count - 1];
    }
    for (int index = count - 1; index >= 0; --index) {
        if (kZoomSteps[index] < percent)
            return kZoomSteps[index];
    }
    return kZoomSteps[0];
}

// Engine thread.
void applyZoomToTab(void* context)
{
    auto* request = static_cast<ZoomRequest*>(context);
    if (!request)
        return;

    if (const WebKitApi* api = webKitApi()) {
        if (WKPageRef page = hb_tabs_page(request->tabId))
            api->pageSetPageZoomFactor(page, request->factor);
    }
    delete request;
}

void postZoom(int tabId, int percent)
{
    if (tabId <= 0)
        return;

    auto* request = new ZoomRequest;
    request->tabId = tabId;
    request->factor = static_cast<double>(percent) / 100.0;
    hb_tabs_invoke_on_webkit_thread(applyZoomToTab, request);
}

std::string activeOrigin()
{
    const int tab = g_activeTabId.load();
    if (tab <= 0)
        return { };
    return originOfURL(hb_tabs_url(tab));
}

// Every tab showing this site is moved together: the level belongs to the site,
// so a second tab on it must not be left behind.
void applyToEverySiteTab(const std::string& origin, int percent)
{
    const int count = hb_tabs_count();
    for (int index = 0; index < count; ++index) {
        const int tab = hb_tabs_id_at(index);
        if (tab <= 0)
            continue;
        if (originOfURL(hb_tabs_url(tab)) != origin)
            continue;
        postZoom(tab, percent);
    }
}

void changeZoom(int percent)
{
    const std::string origin = activeOrigin();
    const int level = clampPercent(percent);

    if (origin.empty()) {
        // A page with no site -- about:blank, a data URL -- can still be zoomed;
        // there is just nothing to remember it under.
        postZoom(g_activeTabId.load(), level);
    } else {
        storeLevel(origin, level);
        applyToEverySiteTab(origin, level);
    }

    g_zoomGeneration.fetch_add(1);
    bumpRevision();
}

std::wstring zoomFilePath()
{
    const std::wstring directory = profileDirectory();
    if (directory.empty())
        return { };
    return directory + kZoomFileName;
}

} // namespace

void zoomStep(int direction)
{
    changeZoom(steppedPercent(zoomPercent(), direction));
}

void zoomReset()
{
    changeZoom(kZoomDefault);
}

void zoomSet(int percent)
{
    changeZoom(percent);
}

int zoomPercent()
{
    return storedLevel(activeOrigin());
}

std::string zoomOrigin()
{
    return activeOrigin();
}

void serviceZoom()
{
    const int revision = hb_tabs_revision();
    const int generation = g_zoomGeneration.load();
    if (revision != g_appliedRevision || generation != g_appliedGeneration) {
        g_appliedRevision = revision;
        g_appliedGeneration = generation;

        const int count = hb_tabs_count();
        std::vector<AppliedZoom> live;
        live.reserve(static_cast<size_t>(count));

        for (int index = 0; index < count; ++index) {
            const int tab = hb_tabs_id_at(index);
            if (tab <= 0)
                continue;

            const std::string url = hb_tabs_url(tab);
            const int wanted = storedLevel(originOfURL(url));

            AppliedZoom entry;
            entry.tabId = tab;
            entry.url = url;
            entry.percent = wanted;

            const AppliedZoom* previous = nullptr;
            for (const auto& applied : g_applied) {
                if (applied.tabId == tab)
                    previous = &applied;
            }

            // A page that has just been loaded starts at the engine's own
            // scale, so the level is applied again whenever the address moves
            // and not only when the site does.
            if (!previous || previous->url != url || previous->percent != wanted)
                postZoom(tab, wanted);

            live.push_back(std::move(entry));
        }
        g_applied = std::move(live);
    }

    if (g_zoomDirty.load() && GetTickCount64() - g_zoomDirtySince.load() >= kZoomSaveDelayMs)
        saveZoomLevels();
}

void loadZoomLevels()
{
    const std::wstring path = zoomFilePath();
    if (path.empty())
        return;

    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;

    std::string contents;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, static_cast<DWORD>(sizeof(buffer)), &read, nullptr) && read > 0)
        contents.append(buffer, read);
    CloseHandle(file);

    std::vector<std::pair<std::string, int>> levels;
    size_t start = 0;
    bool firstLine = true;
    while (start <= contents.size()) {
        size_t end = contents.find('\n', start);
        if (end == std::string::npos)
            end = contents.size();

        std::string line = contents.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (firstLine) {
            firstLine = false;
            if (line != kZoomHeader)
                return;
            continue;
        }
        if (line.empty())
            continue;

        const size_t tab = line.find('\t');
        if (tab == std::string::npos)
            continue;

        const std::string origin = line.substr(0, tab);
        const int percent = std::atoi(line.c_str() + tab + 1);
        if (origin.empty() || percent <= 0)
            continue;
        levels.emplace_back(origin, clampPercent(percent));
    }

    std::lock_guard<std::mutex> lock(g_zoomMutex);
    g_levels = std::move(levels);
}

void saveZoomLevels()
{
    if (!g_zoomDirty.exchange(false))
        return;

    const std::wstring path = zoomFilePath();
    if (path.empty())
        return;

    std::string contents = kZoomHeader;
    contents += '\n';
    {
        std::lock_guard<std::mutex> lock(g_zoomMutex);
        for (const auto& entry : g_levels) {
            if (entry.second == kZoomDefault)
                continue;
            contents += entry.first;
            contents += '\t';
            contents += std::to_string(entry.second);
            contents += '\n';
        }
    }

    // Written beside the real file and moved over it, so a run that ends
    // mid-write leaves the last complete set of levels rather than half of a
    // new one.
    const std::wstring temporary = path + L".new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        setError("the zoom levels could not be written");
        return;
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
    CloseHandle(file);

    if (!ok || written != contents.size()) {
        DeleteFileW(temporary.c_str());
        setError("the zoom levels could not be written");
        return;
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        setError("the zoom levels could not be replaced");
    }
}

} // namespace harmony_input

// --- The host's surface -----------------------------------------------------

using namespace harmony_input;

extern "C" void hb_input_zoom_in(void)
{
    zoomStep(1);
}

extern "C" void hb_input_zoom_out(void)
{
    zoomStep(-1);
}

extern "C" void hb_input_zoom_reset(void)
{
    zoomReset();
}

extern "C" void hb_input_zoom_set(int percent)
{
    zoomSet(percent);
}

extern "C" int hb_input_zoom_percent(void)
{
    return zoomPercent();
}

extern "C" const char* hb_input_zoom_origin(void)
{
    static thread_local std::string storage;
    storage = zoomOrigin();
    return storage.c_str();
}
