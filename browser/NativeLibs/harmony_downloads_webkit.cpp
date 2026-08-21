#include "harmony_downloads.h"
#include "harmony_downloads_internal.h"

#include "harmony_tabs_embed.h"

#include <mutex>

// Where the download engine meets WebKit.
//
// WebKit is owned by the tabs registry -- one module loads WebKit2.dll, one
// thread runs it, one navigation client answers each page's policy listener --
// so this module does not reach for a page of its own. It registers with that
// registry instead, and the three engine-thread entry points in
// harmony_downloads.h are called from what it registers:
//
//   the download client   a link the page marked as a download, an attachment,
//                         a file the engine cannot render, or a URL the host
//                         asked to be saved, and then the WKDownloadRef itself;
//   the cycle hook        the queued cancels, issued against WebKit;
//   the teardown hook     the live downloads, released before WebKit goes.

namespace harmony_downloads {
namespace {

int shouldDownloadAction(int, const void* navigationAction, void*)
{
    return hb_downloads_should_download_action(navigationAction);
}

int shouldDownloadResponse(int, const void* navigationResponse, void*)
{
    return hb_downloads_should_download_response(navigationResponse);
}

void didBecomeDownload(int, const void* download, void*)
{
    hb_downloads_adopt(download);
}

void cycle(void*)
{
    hb_downloads_pump();
}

void teardown(void*)
{
    hb_downloads_shutdown();
}

std::mutex g_attachMutex;
bool g_attached { false };

}
}

using namespace harmony_downloads;

void hb_downloads_attach(void)
{
    // The host calls this every frame, the way it asks the registry to start
    // every frame, so that neither has to be sequenced against the other.
    std::lock_guard<std::mutex> lock(g_attachMutex);
    if (g_attached)
        return;
    g_attached = true;

    hb_tabs_add_download_client(shouldDownloadAction, shouldDownloadResponse, didBecomeDownload, nullptr);
    hb_tabs_add_cycle_hook(cycle, nullptr);
    hb_tabs_add_teardown_hook(teardown, nullptr);
}
