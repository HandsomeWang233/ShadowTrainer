#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace ce {
// Runs only on the DLL's private bootstrap thread, and owns that thread's COM
// apartment for the whole call. WM_CLOSE hides this window; WM_APP+1 shows only,
// WM_APP+2 stops, WM_APP+3 toggles. A separate 16 ms foreground-host key sampler
// stays alive while hidden.
//
// The window is published and CE_RUNNING is set as soon as the window is on
// screen; the WebView2 environment is created asynchronously afterwards, so a
// slow or missing runtime never delays the session becoming usable.
int run_ui(HINSTANCE module, std::atomic<bool>& stop_requested,
           std::atomic<HWND>& window, std::atomic<uint32_t>& state);
}
