#include "harmony_settings_internal.h"

// The settings module's own lifetime: the one call per frame that reads the file
// once, keeps the engine in step with what it says, and settles the file back to
// disk.
//
// Everything here runs on the browser's frame thread. Nothing here blocks: the
// engine work is queued onto the WebKit thread and the disk write happens only
// once a run of edits has settled.

namespace harmony::settings {
namespace {

std::atomic<bool> g_started { false };

} // namespace
} // namespace harmony::settings

using namespace harmony::settings;

extern "C" void hb_settings_frame(void* host_window)
{
    // First, and before anything else in this browser's frame reaches the
    // engine: this is where the settings file is read and where the profile
    // layout this module shares with the data store is prepared. Both happen on
    // this thread, once, while the engine thread that would otherwise prepare
    // the same layout does not exist yet.
    ensureLoaded();

    setPickerOwner(static_cast<HWND>(host_window));

    bool expected = false;
    if (g_started.compare_exchange_strong(expected, true)) {
        // Registered before the registry creates its first page, so that page
        // carries the preferences this profile was configured with rather than
        // the engine's own and a correction a frame later.
        attachToEngine();
    }

    serviceStartup();
    serviceEngine();
    serviceSave();
}

extern "C" void hb_settings_shutdown(void)
{
    // The file first: the engine teardown below runs on a thread this call is
    // about to stop waiting for, and a setting changed in the last frame is
    // worth more than the tidiness of the order.
    flushSave();
    stopFolderPicker();
    detachFromEngine();
}
