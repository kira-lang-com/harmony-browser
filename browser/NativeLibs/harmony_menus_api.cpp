#include "harmony_menus_internal.h"

#include <deque>
#include <mutex>

// The queue between the menu and the browser.
//
// A menu selection is answered on the WebKit thread, inside the nested message
// loop the popup menu runs. Everything a system above the engine owns is put here
// and taken off by the host's frame thread, which is the same shape the input
// system's accelerators cross the same boundary in -- and the same reason: the
// tab list, the download engine and the navigation model are driven from the
// frame thread, and reaching them from inside a menu's run loop would drive them
// from two.

namespace harmony_menus {

namespace {

struct Command {
    int kind { HB_MENU_COMMAND_NONE };
    std::string text;
    int tabId { 0 };
};

std::mutex g_queueMutex;
std::deque<Command> g_queue;

std::atomic<int> g_revision { 0 };

std::mutex g_errorMutex;
std::string g_error;

// The command the calling thread last popped, so its text outlives the lock.
thread_local Command t_current;

} // namespace

void queueCommand(int kind, std::string text, int tabId)
{
    if (kind == HB_MENU_COMMAND_NONE)
        return;

    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        g_queue.push_back(Command { kind, std::move(text), tabId });
    }
    g_revision.fetch_add(1);
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

} // namespace harmony_menus

using namespace harmony_menus;

extern "C" int hb_menus_next_command(void)
{
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty()) {
        t_current = Command { };
        return HB_MENU_COMMAND_NONE;
    }

    t_current = std::move(g_queue.front());
    g_queue.pop_front();
    return t_current.kind;
}

extern "C" const char* hb_menus_command_text(void)
{
    return t_current.text.c_str();
}

extern "C" int hb_menus_command_tab(void)
{
    return t_current.tabId;
}

extern "C" int hb_menus_revision(void)
{
    return g_revision.load();
}

extern "C" const char* hb_menus_error(void)
{
    static thread_local std::string storage;
    storage = currentError();
    return storage.c_str();
}
