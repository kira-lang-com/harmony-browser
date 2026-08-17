#include "harmony_webkit.h"

#include "harmony_data_store.h"
#include "harmony_dialogs.h"
#include "harmony_downloads.h"
#include "harmony_input.h"
#include "harmony_navigation_host.h"
#include "harmony_navigation_state.h"
#include "harmony_permissions.h"
#include "harmony_tabs.h"

#include <atomic>
#include <string>

namespace {

std::atomic<bool> g_attached { false };

// Joins every system to the tab registry, exactly once.
//
// Each of these registers hooks with the registry and writes its own fields into
// the one UI client, the one navigation client and the one state client a page
// carries. None of them installs a client; every one of them would be silently
// replaced by the next if it did.
//
// Order among them does not matter -- the registry consults each list whole when
// it builds a page, and no two systems write the same field. What matters is that
// ALL of them are registered before the registry creates its first page: a page
// made earlier carries clients those hooks never wrote into, and nothing
// afterwards can go back and fill them in.
//
// That is why this runs from the same call that starts the registry, ahead of the
// start, rather than from each system's own first use. There is no frame in which
// one of them is attached and another is not.
//
// The profile is the one system not named here: it is the registry's own
// dependency -- a page configuration cannot be built without a data store to put
// on it -- so the registry attaches it from the call that creates the context,
// which is earlier than any page and earlier than this.
void attachSystems(void* host_window)
{
    bool expected = false;
    if (!g_attached.compare_exchange_strong(expected, true))
        return;

    // The questions a page asks its UI client, and the two authentication fields
    // of its navigation client.
    hb_permissions_attach();
    hb_dialogs_attach();

    // What turns a navigation into a file rather than a page.
    hb_downloads_attach();

    // What the address bar, the tab strip and the history menus read.
    hb_nav_attach();

    // The keyboard, which owns the UI client's three focus fields. It takes the
    // host's window because its hook goes on the message queue of the thread
    // this call is on, which is the thread that owns that window.
    hb_input_attach(host_window);
}

const char* publish(const char* text)
{
    static thread_local std::string copy;
    copy = text ? text : "";
    return copy.c_str();
}

} // namespace

extern "C" int hb_browser_frame(
    void* host_window,
    int x,
    int y,
    int width,
    int height,
    double backing_scale,
    const char* home_url
) {
    if (!host_window)
        return 0;

    attachSystems(host_window);

    // A host that has not measured itself yet reports nothing for its scale, and
    // a page laid out at a scale of zero is a page of no size at all.
    if (backing_scale > 0.0)
        hb_tabs_set_backing_scale(backing_scale);

    // A window too narrow or too short to hold a page holds no page. The registry
    // keeps the last rectangle it was given rather than taking an empty one, so
    // the view stays where it was until there is somewhere to put it.
    if (width > 0 && height > 0)
        hb_tabs_set_bounds(x, y, width, height);

    // Last, so the first tab is created at the size and scale it will be shown
    // at rather than at nothing.
    return hb_tabs_start(host_window, home_url);
}

extern "C" int hb_browser_ready(void)
{
    return hb_tabs_ready();
}

extern "C" const char* hb_browser_error(void)
{
    // The engine first: a browser whose engine never started has nothing else
    // worth reporting, and every system below it would only be saying so again.
    const char* const sources[] = {
        hb_tabs_error(),
        hb_nav_diagnostic(),
        hb_data_store_error(),
        hb_dialogs_error(),
        hb_permissions_error(),
        hb_input_error(),
    };

    for (const char* text : sources) {
        if (text && *text)
            return publish(text);
    }
    return publish("");
}
