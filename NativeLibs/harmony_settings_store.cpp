#include "harmony_settings_internal.h"

#include <mutex>

// The record every preference lives in, and the one moment it is read off disk.
//
// Nothing here reaches WebKit or the filesystem while the lock is held. A read
// copies the field it wants out and lets go; a write takes the lock, changes the
// field, and lets the frame hook decide when the change reaches disk and the
// engine.

namespace harmony::settings {

std::mutex g_mutex;
Values g_values;

std::atomic<int> g_revision { 0 };
std::atomic<int> g_engineGeneration { 0 };

namespace {

std::once_flag g_loadOnce;

std::mutex g_errorMutex;
std::string g_error;

int clampZoom(int percent)
{
    if (percent < kMinimumZoomPercent)
        return kMinimumZoomPercent;
    if (percent > kMaximumZoomPercent)
        return kMaximumZoomPercent;
    return percent;
}

// A value that is one line of a file and cannot hold a line break. Everything
// stored here is a URL, a folder or a name a person typed, and none of those is
// two lines; a pasted newline is dropped rather than left to split the record in
// half the next time it is read.
std::string oneLine(const char* text)
{
    std::string value = text ? text : "";
    std::string clean;
    clean.reserve(value.size());
    for (const char character : value) {
        if (character == '\r' || character == '\n' || character == '\t')
            continue;
        clean.push_back(character);
    }

    size_t first = clean.find_first_not_of(' ');
    if (first == std::string::npos)
        return { };
    size_t last = clean.find_last_not_of(' ');
    return clean.substr(first, last - first + 1);
}

// This thread's copy of a string the lock protects, so a caller reads it without
// racing another thread's edit.
const char* publish(std::string value)
{
    static thread_local std::string storage;
    storage = std::move(value);
    return storage.c_str();
}

} // namespace

void ensureLoaded()
{
    std::call_once(g_loadOnce, [] {
        g_values.engines = defaultEngines();
        loadSettings();
    });
}

void setError(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    g_error = message;
}

std::string currentError()
{
    std::lock_guard<std::mutex> lock(g_errorMutex);
    return g_error;
}

} // namespace harmony::settings

// --- The host's surface -------------------------------------------------------

using namespace harmony::settings;

extern "C" int hb_settings_revision(void)
{
    ensureLoaded();
    return g_revision.load();
}

extern "C" const char* hb_settings_error(void)
{
    return publish(currentError());
}

extern "C" const char* hb_settings_path(void)
{
    ensureLoaded();
    return publish(settingsFilePathUtf8());
}

// --- Home page ----------------------------------------------------------------

extern "C" const char* hb_settings_home_url(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return publish(g_values.homeURL);
}

extern "C" void hb_settings_set_home_url(const char* url)
{
    const std::string wanted = oneLine(url);
    withValues([&wanted](Values& values) {
        // A home cleared to nothing is the browser's own default rather than a
        // command to open nowhere: every tab this browser opens takes an
        // address, and there is no such thing as opening none.
        values.homeURL = wanted.empty() ? kDefaultHomeURL : wanted;
    });
}

// --- Startup ------------------------------------------------------------------

extern "C" int hb_settings_startup(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.startup;
}

extern "C" void hb_settings_set_startup(int behaviour)
{
    const int wanted = behaviour == HB_SETTINGS_STARTUP_NEW_TAB
        ? HB_SETTINGS_STARTUP_NEW_TAB
        : HB_SETTINGS_STARTUP_RESTORE_SESSION;
    withValues([wanted](Values& values) {
        values.startup = wanted;
    });
}

// --- Zoom ---------------------------------------------------------------------

extern "C" int hb_settings_default_zoom_percent(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.zoomPercent;
}

extern "C" void hb_settings_set_default_zoom_percent(int percent)
{
    const int wanted = clampZoom(percent);
    withValues([wanted](Values& values) {
        values.zoomPercent = wanted;
    });
}

// --- What a page may do -------------------------------------------------------

extern "C" int hb_settings_javascript_enabled(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.javaScript ? 1 : 0;
}

extern "C" void hb_settings_set_javascript_enabled(int enabled)
{
    const bool wanted = enabled != 0;
    withValues([wanted](Values& values) {
        values.javaScript = wanted;
    });
}

extern "C" int hb_settings_images_enabled(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.images ? 1 : 0;
}

extern "C" void hb_settings_set_images_enabled(int enabled)
{
    const bool wanted = enabled != 0;
    withValues([wanted](Values& values) {
        values.images = wanted;
    });
}

extern "C" int hb_settings_popups_enabled(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.popups ? 1 : 0;
}

extern "C" void hb_settings_set_popups_enabled(int enabled)
{
    const bool wanted = enabled != 0;
    withValues([wanted](Values& values) {
        values.popups = wanted;
    });
}

extern "C" int hb_settings_do_not_track(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_values.doNotTrack ? 1 : 0;
}

extern "C" void hb_settings_set_do_not_track(int enabled)
{
    const bool wanted = enabled != 0;
    withValues([wanted](Values& values) {
        values.doNotTrack = wanted;
    });
}

// --- Downloads ----------------------------------------------------------------

extern "C" const char* hb_settings_download_directory(void)
{
    ensureLoaded();
    std::lock_guard<std::mutex> lock(g_mutex);
    return publish(g_values.downloadDirectory);
}

extern "C" void hb_settings_set_download_directory(const char* path)
{
    std::string wanted = oneLine(path);
    // A trailing separator names the same directory and would double the one
    // this browser joins a file name on with.
    while (wanted.size() > 3 && (wanted.back() == '\\' || wanted.back() == '/'))
        wanted.pop_back();

    withValues([&wanted](Values& values) {
        values.downloadDirectory = wanted;
    });
}
