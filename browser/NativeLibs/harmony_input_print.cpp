#include "harmony_input_internal.h"

#include "harmony_text.h"

#include <commdlg.h>

#include <algorithm>

// Printing the page.
//
// The page is spooled as it is composed on screen. This port's web process
// carries no paginated print path -- the message that hands printed pages back
// to the UI process is built for Cocoa and for GTK and for neither of the two
// this browser runs on -- so what a printer can be given is the view's own
// content, captured the way a tab's stand-in frame is and stretched onto the
// sheet.
//
// The dialog and the spooling run on a thread of this module's own. A print
// dialog is modal, and a modal dialog on the frame thread would stop the
// browser drawing, while one on the engine thread would stop every page in it.

namespace harmony_input {

namespace {

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

struct PrintJob {
    HWND window { nullptr };
    std::wstring title;
};

std::mutex g_jobMutex;
std::deque<PrintJob> g_jobs;
HANDLE g_jobEvent { nullptr };
HANDLE g_printThread { nullptr };
std::atomic<bool> g_stopping { false };

void describeBitmap(BITMAPINFO& info, int width, int height)
{
    info = BITMAPINFO { };
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    // Negative: the rows run top down, which is the order the capture wrote
    // them and the order the printer is handed them.
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
}

// The window's composed content. `PW_RENDERFULLCONTENT` is what asks for what
// is on screen rather than for the result of a paint message, which is the only
// thing that answers for a view whose pixels belong to another process.
bool capturePixels(HWND window, std::vector<unsigned char>& pixels, int& width, int& height)
{
    if (!window || !IsWindow(window))
        return false;

    RECT client { };
    if (!GetClientRect(window, &client))
        return false;

    width = client.right - client.left;
    height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return false;

    HDC windowDC = GetDC(window);
    if (!windowDC)
        return false;

    bool captured = false;
    HDC memoryDC = CreateCompatibleDC(windowDC);
    if (memoryDC) {
        BITMAPINFO info { };
        describeBitmap(info, width, height);

        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(windowDC, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap && bits) {
            HGDIOBJ previous = SelectObject(memoryDC, bitmap);
            RECT fill { 0, 0, width, height };
            FillRect(memoryDC, &fill, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            if (PrintWindow(window, memoryDC, PW_RENDERFULLCONTENT)) {
                GdiFlush();
                const auto* start = static_cast<const unsigned char*>(bits);
                pixels.assign(start, start + static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
                captured = true;
            }
            SelectObject(memoryDC, previous);
        }
        if (bitmap)
            DeleteObject(bitmap);
        DeleteDC(memoryDC);
    }

    ReleaseDC(window, windowDC);
    return captured;
}

void spoolPage(HDC printer, const std::vector<unsigned char>& pixels, int width, int height)
{
    const int sheetWidth = GetDeviceCaps(printer, HORZRES);
    const int sheetHeight = GetDeviceCaps(printer, VERTRES);
    if (sheetWidth <= 0 || sheetHeight <= 0)
        return;

    // Fitted rather than filled: a page stretched to the sheet's proportions is
    // a page nobody would recognise.
    const double scale = std::min(
        static_cast<double>(sheetWidth) / static_cast<double>(width),
        static_cast<double>(sheetHeight) / static_cast<double>(height)
    );
    const int drawWidth = std::max(1, static_cast<int>(static_cast<double>(width) * scale));
    const int drawHeight = std::max(1, static_cast<int>(static_cast<double>(height) * scale));
    const int left = (sheetWidth - drawWidth) / 2;

    BITMAPINFO info { };
    describeBitmap(info, width, height);

    SetStretchBltMode(printer, HALFTONE);
    SetBrushOrgEx(printer, 0, 0, nullptr);
    StretchDIBits(
        printer,
        left,
        0,
        drawWidth,
        drawHeight,
        0,
        0,
        width,
        height,
        pixels.data(),
        &info,
        DIB_RGB_COLORS,
        SRCCOPY
    );
}

void runJob(const PrintJob& job)
{
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    if (!capturePixels(job.window, pixels, width, height)) {
        setError("the page could not be captured for printing");
        return;
    }

    PRINTDLGW dialog { };
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = g_hostWindow.load();
    dialog.Flags = PD_RETURNDC | PD_NOPAGENUMS | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
    dialog.nCopies = 1;

    if (!PrintDlgW(&dialog)) {
        // A cancelled dialog is an answer, not a failure. Only a dialog that
        // could not be shown is worth reporting.
        if (CommDlgExtendedError() != 0)
            setError("the print dialog could not be opened");
        if (dialog.hDevMode)
            GlobalFree(dialog.hDevMode);
        if (dialog.hDevNames)
            GlobalFree(dialog.hDevNames);
        return;
    }

    HDC printer = dialog.hDC;
    if (printer) {
        DOCINFOW document { };
        document.cbSize = sizeof(document);
        document.lpszDocName = job.title.empty() ? L"Harmony Browser" : job.title.c_str();

        if (StartDocW(printer, &document) > 0) {
            if (StartPage(printer) > 0) {
                spoolPage(printer, pixels, width, height);
                EndPage(printer);
            }
            EndDoc(printer);
        } else
            setError("the printer refused the document");

        DeleteDC(printer);
    }

    if (dialog.hDevMode)
        GlobalFree(dialog.hDevMode);
    if (dialog.hDevNames)
        GlobalFree(dialog.hDevNames);
}

DWORD WINAPI printThreadMain(LPVOID)
{
    for (;;) {
        WaitForSingleObject(g_jobEvent, INFINITE);
        if (g_stopping.load())
            return 0;

        for (;;) {
            PrintJob job;
            {
                std::lock_guard<std::mutex> lock(g_jobMutex);
                if (g_jobs.empty())
                    break;
                job = std::move(g_jobs.front());
                g_jobs.pop_front();
            }
            runJob(job);
            if (g_stopping.load())
                return 0;
        }
    }
}

bool startPrintThread()
{
    if (g_printThread)
        return true;

    if (!g_jobEvent) {
        g_jobEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!g_jobEvent) {
            setError("the print queue could not be created");
            return false;
        }
    }

    g_stopping.store(false);
    g_printThread = CreateThread(nullptr, 0, printThreadMain, nullptr, 0, nullptr);
    if (!g_printThread) {
        setError("the print thread could not be started");
        return false;
    }
    return true;
}

} // namespace

void printActiveTab()
{
    const int tab = g_activeTabId.load();
    HWND window = tabWindow(tab);
    if (!window || !IsWindow(window)) {
        setError("there is no page to print");
        return;
    }

    if (!startPrintThread())
        return;

    PrintJob job;
    job.window = window;
    job.title = harmony::text::widen(hb_tabs_title(tab));

    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        g_jobs.push_back(std::move(job));
    }
    SetEvent(g_jobEvent);
}

void stopPrinting()
{
    if (!g_printThread) {
        if (g_jobEvent) {
            CloseHandle(g_jobEvent);
            g_jobEvent = nullptr;
        }
        return;
    }

    g_stopping.store(true);
    SetEvent(g_jobEvent);
    // A dialog the user has left open is theirs to answer: the wait is bounded
    // so a browser closing behind one still closes.
    const DWORD finished = WaitForSingleObject(g_printThread, 2000);
    CloseHandle(g_printThread);
    g_printThread = nullptr;

    // The event stays alive while the thread that waits on it does. Closing it
    // out from under a dialog the user is still answering would hand the
    // handle back to the process for something else to be opened onto.
    if (finished == WAIT_OBJECT_0) {
        CloseHandle(g_jobEvent);
        g_jobEvent = nullptr;
    }

    std::lock_guard<std::mutex> lock(g_jobMutex);
    g_jobs.clear();
}

} // namespace harmony_input

extern "C" void hb_input_print(void)
{
    harmony_input::printActiveTab();
}
