#include <windows.h>
#include "ce/api.h"
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>

void require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
    std::printf("PASS %s\n", text);
}
template<class T> T symbol(HMODULE module, const char* name) {
    auto address = GetProcAddress(module, name);
    if (!address) throw std::runtime_error(std::string("missing export ") + name);
    return reinterpret_cast<T>(address);
}

struct Api {
    HMODULE module = nullptr;
    decltype(&CE_GetState) state;
    decltype(&CE_GetHostPid) pid;
    decltype(&CE_GetApiVersion) version;
    decltype(&CE_RequestStop) stop;
    decltype(&CE_WaitStopped) wait;
    decltype(&CE_ShowWindow) show;
    decltype(&CE_FirstScan) first;
    decltype(&CE_NextScan) next;
    decltype(&CE_ResultCount) count;
    decltype(&CE_GetResult) result;
    decltype(&CE_ReadInt32) read;
    decltype(&CE_WriteInt32) write;
    decltype(&CE_SetFreeze) freeze;
    decltype(&CE_SaveTable) save;
    decltype(&CE_LoadTable) load;
    explicit Api(const wchar_t* path) {
        module = LoadLibraryW(path);
        if (!module) throw std::runtime_error("LoadLibrary failed");
#define LOAD(field, name) field = symbol<decltype(field)>(module, #name)
        LOAD(state, CE_GetState); LOAD(pid, CE_GetHostPid); LOAD(version, CE_GetApiVersion);
        LOAD(stop, CE_RequestStop); LOAD(wait, CE_WaitStopped); LOAD(show, CE_ShowWindow);
        LOAD(first, CE_FirstScan); LOAD(next, CE_NextScan); LOAD(count, CE_ResultCount);
        LOAD(result, CE_GetResult); LOAD(read, CE_ReadInt32); LOAD(write, CE_WriteInt32);
        LOAD(freeze, CE_SetFreeze); LOAD(save, CE_SaveTable); LOAD(load, CE_LoadTable);
#undef LOAD
    }
    ~Api() {
        if (module && stop() == CE_OK && wait(10000) == CE_OK) FreeLibrary(module);
    }
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid != GetCurrentProcessId() || GetWindow(window, GW_OWNER)) return TRUE;
    wchar_t title[256]{};
    GetWindowTextW(window, title, 256);
    if (GetPropW(window, L"SHADOWTRAINER_WINDOW") || wcscmp(title, L"ShadowTrainer") == 0) {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

int wmain(int argc, wchar_t** argv) {
    try {
        volatile LONG sample = 123456789;
        if (argc == 2 && wcscmp(argv[1], L"--baseline") == 0) {
            require(sample == 123456789, "baseline fixture value=123456789");
            sample = 987654321;
            require(sample == 987654321, "baseline host changes value=987654321");
            std::printf("BASELINE host alive; DLL not loaded\n");
            return 0;
        }
        // `--hold N` is a debugging aid, not a check: the window lives from the
        // load to the stop of one cycle, which is over before the cover ends, so
        // a plain run never gets past it. Holding the first
        // window open is how a human watches the cover and the page it reveals.
        int hold_seconds = 0;
        const wchar_t* dll_path = nullptr;
        for (int i = 1; i < argc; ++i) {
            if (wcscmp(argv[i], L"--hold") == 0 && i + 1 < argc) hold_seconds = _wtoi(argv[++i]);
            else if (!dll_path) dll_path = argv[i];
        }
        if (!dll_path) {
            std::fprintf(stderr, "usage: runtime_tests.exe DLL_PATH [--hold SECONDS] | --baseline\n");
            return 2;
        }
        const auto address = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&sample));
        wchar_t original_cwd[MAX_PATH]{};
        GetCurrentDirectoryW(MAX_PATH, original_cwd);
        for (int cycle = 0; cycle != 3; ++cycle) {
            sample = 123456789;
            Api api(dll_path);
            for (int i = 0; i != 200 && api.state() == CE_STARTING; ++i) Sleep(25);
            require(api.state() == CE_RUNNING, "automatic runtime startup");
            require(api.version() == 1 && api.pid() == GetCurrentProcessId(), "ABI and default host PID");
            HWND window = nullptr;
            EnumWindows(find_window, reinterpret_cast<LPARAM>(&window));
            require(window && IsWindowVisible(window), "automatic visible UI");
            if (cycle == 0 && hold_seconds > 0) {
                std::printf("holding the first window for %d second(s)\n", hold_seconds);
                Sleep(static_cast<DWORD>(hold_seconds) * 1000);
            }
            if (cycle == 0) {
                auto page = static_cast<int32_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
                require(page != nullptr, "broad-scan fixture allocation");
                page[0] = 135792468;
                CeScanRequest broad{sizeof(CeScanRequest), 4, 0, 0, 135792468, 0};
                require(api.first(&broad) == CE_OK, "full process exact scan");
                uint64_t total = 0;
                require(api.count(&total) == CE_OK, "broad result count");
                bool found = false;
                for (uint64_t i = 0; i < total; ++i) {
                    CeResult item{};
                    require(api.result(i, &item) == CE_OK, "broad result readable");
                    found = found || item.address == reinterpret_cast<uintptr_t>(page);
                }
                VirtualFree(page, 0, MEM_RELEASE);
                require(found, "full scan finds known host allocation");
            }
            CeScanRequest request{sizeof(CeScanRequest), 4, address, address + 4, 123456789, 0};
            require(api.first(&request) == CE_OK, "first exact int32 scan");
            uint64_t count = 0;
            require(api.count(&count) == CE_OK && count == 1, "one fixture result");
            CeResult result{};
            require(api.result(0, &result) == CE_OK && result.address == address, "fixture address preserved");
            sample = 987654321;
            require(api.next(987654321) == CE_OK, "next scan sees host mutation");
            require(api.count(&count) == CE_OK && count == 1, "refined result preserved");
            require(api.write(address, 777) == CE_OK && sample == 777, "write updates fixture");
            require(api.freeze(address, 777, 1) == CE_OK, "enable freeze");
            sample = 222;
            Sleep(300);
            require(sample == 777, "freeze tick restored requested value");
            require(api.freeze(address, 777, 0) == CE_OK, "disable freeze");
            sample = 333;
            Sleep(200);
            require(sample == 333, "disabled freeze leaves host changes");
            wchar_t current_cwd[MAX_PATH]{};
            GetCurrentDirectoryW(MAX_PATH, current_cwd);
            require(wcscmp(original_cwd, current_cwd) == 0, "host current directory unchanged");
            PostMessageW(window, WM_CLOSE, 0, 0);
            for (int i = 0; i != 100 && IsWindowVisible(window); ++i) Sleep(10);
            require(!IsWindowVisible(window) && api.state() == CE_RUNNING, "close hides CE and preserves host");
            require(api.show() == CE_OK, "request reopen");
            for (int i = 0; i != 100 && !IsWindowVisible(window); ++i) Sleep(10);
            require(IsWindowVisible(window), "UI reopened");
            if (cycle == 2) {
                auto region = VirtualAlloc(nullptr, 128 * 1024 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                require(region != nullptr, "concurrent-stop fixture allocation");
                auto begin = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(region));
                CeScanRequest dense{sizeof(CeScanRequest), 1, begin, begin + 128 * 1024 * 1024, 0, 0};
                std::atomic<bool> entered{false};
                std::atomic<int> scan_status{CE_INTERNAL_ERROR};
                std::thread scanning([&] { entered.store(true); scan_status.store(api.first(&dense)); });
                while (!entered.load()) SwitchToThread();
                Sleep(5);
                const auto stop_status = api.stop();
                scanning.join();
                VirtualFree(region, 0, MEM_RELEASE);
                require(stop_status == CE_OK, "stop request during active API scan");
                require(scan_status.load() == CE_CANCELLED || scan_status.load() == CE_NOT_RUNNING,
                        "concurrent API scan cancelled or rejected after stop");
            }
            require(api.stop() == CE_OK && api.wait(10000) == CE_OK, "stop drains runtime worker");
            require(api.state() == CE_STOPPED, "runtime stopped cleanly");
            int32_t value = 0;
            require(api.read(address, &value) == CE_NOT_RUNNING, "reject operations after stop");
            sample = 444;
            Sleep(150);
            require(sample == 444, "host survives stop without residual freeze");
            // Roll back only the known local fixture, after all workers have stopped.
            InterlockedExchange(&sample, 123456789);
            require(sample == 123456789, "fixture rollback restored original value");
            std::printf("CYCLE %d complete\n", cycle + 1);
        }
        require(GetModuleHandleW(L"shadowtrainer.dll") == nullptr, "module unloaded after balanced references");
        std::printf("MODIFIED host alive; 3 load/scan/freeze/close/stop/unload cycles passed\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s (Win32=%lu)\n", error.what(), GetLastError());
        return 1;
    }
}
