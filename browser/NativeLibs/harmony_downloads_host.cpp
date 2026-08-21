#include "harmony_downloads.h"
#include "harmony_downloads_internal.h"

// What the host's frame thread calls. Nothing here touches a WebKit object:
// every read answers from the record store behind its lock, and the one command
// there is -- cancel -- is queued for the WebKit thread to issue.

namespace harmony_downloads {
namespace {

// Text crosses as a pointer the CALLER's thread owns. The store is free to
// change the record the moment the lock is dropped, and a caller that read the
// record's own buffer would be reading it while it moved.
const char* publishText(std::string&& value, std::string& storage)
{
    storage = std::move(value);
    return storage.c_str();
}

}
}

using namespace harmony_downloads;

int hb_downloads_revision(void)
{
    return model::revision();
}

int hb_downloads_count(void)
{
    return model::count();
}

int hb_downloads_id_at(int index)
{
    return model::idAt(index);
}

int hb_downloads_active_count(void)
{
    return model::activeCount();
}

int hb_downloads_state(int id)
{
    return model::stateOf(id);
}

long long hb_downloads_received_bytes(int id)
{
    return model::receivedBytes(id);
}

long long hb_downloads_total_bytes(int id)
{
    return model::totalBytes(id);
}

const char* hb_downloads_text(int id, int field)
{
    static thread_local std::string storage;
    return publishText(model::textOf(id, field), storage);
}

const char* hb_downloads_directory(void)
{
    static thread_local std::string storage;
    return publishText(narrow(downloadsDirectory()), storage);
}

void hb_downloads_cancel(int id)
{
    model::queueCancel(id);
}

void hb_downloads_remove(int id)
{
    model::remove(id);
}

void hb_downloads_clear_finished(void)
{
    model::clearTerminal();
}

void hb_downloads_reveal(int id)
{
    revealInExplorer(model::nativePathOf(id, false));
}

void hb_downloads_open(int id)
{
    openWithShell(model::nativePathOf(id, true));
}

void hb_downloads_reveal_directory(void)
{
    openWithShell(downloadsDirectory());
}

void hb_downloads_force_next(const char* url)
{
    if (!url || !*url)
        return;
    model::forceURL(url);
}
