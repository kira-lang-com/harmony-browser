#include "harmony_settings_internal.h"

#include <objbase.h>
#include <shobjidl.h>

// The folder a download lands in, chosen the way Windows chooses a folder.
//
// The shell dialog runs a modal message loop and calls arbitrary handler code
// inside it, so it gets a thread and a COM apartment of its own: neither the
// frame thread nor the engine thread may stop while a person is looking at a
// list of folders.

namespace harmony::settings {

namespace {

// The shell's own class and interface identifiers, spelled here rather than
// linked from uuid.lib: this is two constants, and naming them costs less than a
// library dependency that exists to hold them.
const CLSID kFileOpenDialogClass = { 0xDC1C5A9C, 0xE88A, 0x4DDE, { 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7 } };
const IID kFileOpenDialogInterface = { 0xD57C7288, 0xD4AD, 0x4768, { 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60 } };

std::atomic<HWND> g_owner { nullptr };
std::atomic<bool> g_open { false };
std::atomic<HANDLE> g_thread { nullptr };

std::wstring chosenFolder(HWND owner)
{
    IFileOpenDialog* dialog = nullptr;
    const HRESULT created = CoCreateInstance(
        kFileOpenDialogClass,
        nullptr,
        CLSCTX_INPROC_SERVER,
        kFileOpenDialogInterface,
        reinterpret_cast<void**>(&dialog)
    );
    if (FAILED(created) || !dialog) {
        setError("the Windows folder dialog could not be created");
        return { };
    }

    DWORD options = 0;
    if (FAILED(dialog->GetOptions(&options)))
        options = 0;
    // A folder that is not on the filesystem has no path to save a file into,
    // and a dialog that changed the process's working directory would move every
    // relative path this browser resolves afterwards.
    options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (FAILED(dialog->SetOptions(options))) {
        setError("the Windows folder dialog refused the options a folder choice needs");
        dialog->Release();
        return { };
    }

    (void)dialog->SetTitle(L"Choose where downloads are saved");

    const HRESULT shown = dialog->Show(owner);
    if (FAILED(shown)) {
        // A cancelled dialog is not a failure; anything else is worth naming.
        if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED))
            setError("the Windows folder dialog failed to open");
        dialog->Release();
        return { };
    }

    std::wstring path;
    IShellItem* item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
        PWSTR display = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &display)) && display) {
            path.assign(display);
            CoTaskMemFree(display);
        }
        item->Release();
    }

    dialog->Release();
    return path;
}

DWORD WINAPI pickerThreadMain(LPVOID)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(comResult);
    if (!comInitialized && comResult != RPC_E_CHANGED_MODE) {
        setError("the folder dialog thread could not enter a COM apartment");
        g_open.store(false);
        return 0;
    }

    const std::wstring path = chosenFolder(g_owner.load());
    if (!path.empty())
        hb_settings_set_download_directory(narrow(path).c_str());

    if (comInitialized)
        CoUninitialize();

    g_open.store(false);
    return 0;
}

void closeFinishedThread()
{
    HANDLE thread = g_thread.load();
    if (!thread || g_open.load())
        return;
    if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0)
        return;
    CloseHandle(thread);
    g_thread.store(nullptr);
}

} // namespace

void setPickerOwner(HWND window)
{
    if (window)
        g_owner.store(window);
    closeFinishedThread();
}

void openFolderPicker()
{
    // One dialog at a time. A second would be a second modal window over the
    // same button, and the two would answer the same setting in whichever order
    // they happened to close.
    bool expected = false;
    if (!g_open.compare_exchange_strong(expected, true))
        return;

    closeFinishedThread();
    if (g_thread.load()) {
        // The previous picker's thread has not been reaped yet, which means it is
        // still inside a dialog this browser thinks is closed.
        g_open.store(false);
        return;
    }

    HANDLE thread = CreateThread(nullptr, 0, pickerThreadMain, nullptr, 0, nullptr);
    if (!thread) {
        g_open.store(false);
        setError("the folder dialog could not be opened");
        return;
    }
    g_thread.store(thread);
}

bool folderPickerIsOpen()
{
    return g_open.load();
}

void stopFolderPicker()
{
    HANDLE thread = g_thread.exchange(nullptr);
    if (!thread)
        return;

    // A picker that is open owns the user's attention; the wait is bounded so a
    // forgotten dialog cannot hold the process open. The handle is leaked rather
    // than closed under a thread that is still inside the dialog.
    if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0)
        CloseHandle(thread);
}

} // namespace harmony::settings

using namespace harmony::settings;

extern "C" void hb_settings_choose_download_directory(void)
{
    ensureLoaded();
    openFolderPicker();
}

extern "C" int hb_settings_download_directory_pending(void)
{
    return folderPickerIsOpen() ? 1 : 0;
}
