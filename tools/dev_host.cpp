// Development host for the runtime DLL: loads it into this process, lets the
// web UI run, and stops it cleanly. This is the iteration loop for the page --
// with --dev the Dll serves web/ straight off disk and turns DevTools back on,
// so a stylesheet edit is an F5 instead of a rebuild.
//
// It is a tool, never a product: nothing in dist/ depends on it.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

using GetStateFn = uint32_t(WINAPI*)();
using RequestStopFn = int(WINAPI*)();
using WaitStoppedFn = int(WINAPI*)(uint32_t);

std::wstring executable_directory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    std::wstring text = path;
    const size_t slash = text.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : text.substr(0, slash);
}

void report(const char* step) {
    std::printf("  [host] %s\n", step);
    std::fflush(stdout);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::wstring dll = L"dist\\x64\\shadowtrainer.dll";
    bool development = false;
    int run_seconds = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--dev") == 0) development = true;
        else if (std::wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc)
            run_seconds = _wtoi(argv[++i]);
        else dll = argv[i];
    }
    if (dll.find(L'\\') == std::wstring::npos) {
        wchar_t full[MAX_PATH]{};
        GetFullPathNameW(dll.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
        dll = full;
    }

    if (development) {
        // The page is read from the repository's web/ directory.
        const std::wstring root = executable_directory() + L"\\..\\..\\web";
        SetEnvironmentVariableW(L"SHADOWTRAINER_WEBUI_DIR", root.c_str());
        SetEnvironmentVariableW(L"SHADOWTRAINER_WEBUI_DEV", L"1");
        std::wprintf(L"dev assets: %s\n", root.c_str());
    }

    report("loading");
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) {
        std::printf("cannot load %ls (error %lu)\n", dll.c_str(), GetLastError());
        return 1;
    }
    const auto state = reinterpret_cast<GetStateFn>(GetProcAddress(module, "CE_GetState"));
    const auto request_stop = reinterpret_cast<RequestStopFn>(GetProcAddress(module, "CE_RequestStop"));
    const auto wait_stopped = reinterpret_cast<WaitStoppedFn>(GetProcAddress(module, "CE_WaitStopped"));
    if (!state || !request_stop || !wait_stopped) {
        std::printf("the DLL does not export the session entry points\n");
        return 1;
    }
    report("loaded");

    for (int i = 0; i < 400 && state() == 0 /* CE_STARTING */; ++i) Sleep(25);
    std::printf("state after startup: %u\n", state());
    if (state() != 1) {
        std::printf("the session did not reach CE_RUNNING\n");
        return 1;
    }
    report("running");

    if (run_seconds > 0) {
        std::printf("running for %d second(s)\n", run_seconds);
        Sleep(static_cast<DWORD>(run_seconds) * 1000);
    } else {
        std::printf("press Enter to stop the session\n");
        std::getchar();
    }

    report("requesting stop");
    request_stop();
    report("waiting");
    const int stopped = wait_stopped(20000);
    std::printf("CE_WaitStopped -> %d\n", stopped);
    report(stopped == 0 ? "stopped" : "did not stop in time");
    const BOOL freed = FreeLibrary(module);
    std::printf("FreeLibrary -> %d\n", freed);
    report("done");
    return stopped == 0 && freed ? 0 : 1;
}
