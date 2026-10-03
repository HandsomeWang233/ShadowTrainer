#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "ce/api.h"
#include "ce/api_v2.h"
#include "core.hpp"
#include "pointer_scan.hpp"
#include "webui.hpp"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <exception>
#include <thread>

namespace {
constinit std::atomic<uint32_t> state{CE_STARTING};
constinit std::atomic<bool> stop_requested{false};
constinit std::atomic<HWND> main_window{nullptr};
SRWLOCK core_lock = SRWLOCK_INIT;
ce::Core* core = nullptr;
HANDLE runtime_thread = nullptr;
DWORD runtime_thread_id = 0;

struct SharedLock {
    SharedLock() { AcquireSRWLockShared(&core_lock); }
    ~SharedLock() { ReleaseSRWLockShared(&core_lock); }
};
template<class F> int invoke(F&& operation) noexcept {
    try {
        SharedLock lock;
        if (state.load() != CE_RUNNING || !core) return CE_NOT_RUNNING;
        ce::set_error(L"");
        return operation(*core);
    } catch (const std::exception&) {
        return CE_INTERNAL_ERROR;
    } catch (...) {
        return CE_INTERNAL_ERROR;
    }
}

DWORD WINAPI bootstrap(void* parameter) {
    auto module = static_cast<HMODULE>(parameter);
    HMODULE owned_reference = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&bootstrap), &owned_reference)) {
        state.store(CE_FAILED);
        return 1;
    }
    std::thread freeze_thread;
    bool failed = false;
    try {
        auto instance = new ce::Core;
        AcquireSRWLockExclusive(&core_lock);
        core = instance;
        ReleaseSRWLockExclusive(&core_lock);
        freeze_thread = std::thread([] {
            while (!stop_requested.load()) {
                invoke([](ce::Core& c) { c.tick_freezes(); return CE_OK; });
                std::this_thread::sleep_for(std::chrono::milliseconds(80));
            }
        });
        if (!stop_requested.load()) {
            failed = ce::run_ui(module, stop_requested, main_window, state) != 0;
        }
    } catch (...) {
        failed = true;
    }
    stop_requested.store(true);
    state.store(CE_STOPPING);
    {
        SharedLock lock;
        if (core) core->request_stop();
    }
    if (freeze_thread.joinable()) freeze_thread.join();
    // UI has joined its scan worker; the exclusive lock drains exported calls.
    AcquireSRWLockExclusive(&core_lock);
    if (core) {
        try { core->shutdown(); } catch (...) { failed = true; }
        delete core;
        core = nullptr;
    }
    ReleaseSRWLockExclusive(&core_lock);
    main_window.store(nullptr);
    state.store(failed ? CE_FAILED : CE_STOPPED);
    // Caller retains its loading reference until CE_WaitStopped has succeeded.
    FreeLibraryAndExitThread(owned_reference, failed ? 1 : 0);
}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // No joins, UI, symbols, scanning, COM, or nontrivial initialization here.
        runtime_thread = CreateThread(nullptr, 0, bootstrap, module, 0, &runtime_thread_id);
        if (!runtime_thread) state.store(CE_FAILED);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (runtime_thread) CloseHandle(runtime_thread);
    }
    return TRUE;
}

uint32_t CE_CALL CE_GetApiVersion() { return 1; }
uint32_t CE_CALL CE_GetState() { return state.load(); }
uint32_t CE_CALL CE_GetHostPid() { return GetCurrentProcessId(); }
int CE_CALL CE_ShowWindow() {
    if (state.load() != CE_RUNNING) return CE_NOT_RUNNING;
    auto window = main_window.load();
    return window && PostMessageW(window, WM_APP + 1, 0, 0) ? CE_OK : CE_NOT_RUNNING;
}
int CE_CALL CE_RequestStop() {
    stop_requested.store(true);
    auto previous = state.load();
    while ((previous == CE_RUNNING || previous == CE_STARTING) &&
           !state.compare_exchange_weak(previous, CE_STOPPING)) {}
    {
        SharedLock lock;
        if (core) core->request_stop();
    }
    if (auto window = main_window.load()) PostMessageW(window, WM_APP + 2, 0, 0);
    return CE_OK;
}
int CE_CALL CE_WaitStopped(uint32_t timeout_ms) {
    if (GetCurrentThreadId() == runtime_thread_id) return CE_BUSY;
    if (!runtime_thread) return state.load() == CE_FAILED ? CE_OK : CE_NOT_RUNNING;
    auto status = WaitForSingleObject(runtime_thread, timeout_ms);
    if (status == WAIT_OBJECT_0) return CE_OK;
    return status == WAIT_TIMEOUT ? CE_BUSY : CE_INTERNAL_ERROR;
}
int CE_CALL CE_FirstScan(const CeScanRequest* request) {
    if (!request || request->size != sizeof(CeScanRequest) || request->reserved) return CE_INVALID_ARGUMENT;
    const auto copy = *request;
    return invoke([&](ce::Core& c) { return c.first_scan(copy); });
}
int CE_CALL CE_NextScan(int32_t value) {
    return invoke([&](ce::Core& c) { return c.next_scan(value); });
}
void CE_CALL CE_CancelScan() {
    SharedLock lock;
    if (core) core->cancel_scan();
}
int CE_CALL CE_ResultCount(uint64_t* count) {
    if (!count) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.result_count(*count); });
}
int CE_CALL CE_GetResult(uint64_t index, CeResult* result) {
    if (!result) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.result(index, *result); });
}
int CE_CALL CE_ReadInt32(uint64_t address, int32_t* value) {
    if (!value) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.read(address, *value); });
}
int CE_CALL CE_WriteInt32(uint64_t address, int32_t value) {
    return invoke([&](ce::Core& c) { return c.write(address, value); });
}
int CE_CALL CE_SetFreeze(uint64_t address, int32_t value, int enabled) {
    if (enabled != 0 && enabled != 1) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.freeze(address, value, enabled != 0); });
}
int CE_CALL CE_SaveTable(const wchar_t* path) {
    if (!path || !*path) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.save_table(path); });
}
int CE_CALL CE_LoadTable(const wchar_t* path) {
    if (!path || !*path) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.load_table(path); });
}
namespace {
template<class T> bool valid_v2(const T* value) {
    return value && value->size == sizeof(T) && value->version == CE_V2_VERSION;
}
int copy_text_v2(const std::wstring& text, wchar_t* out, uint32_t capacity, uint32_t* required) {
    if (!required || (!out && capacity)) return CE_INVALID_ARGUMENT;
    if (text.size() >= UINT32_MAX) return CE_INTERNAL_ERROR;
    *required = static_cast<uint32_t>(text.size() + 1);
    if (!out && !capacity) return CE_OK;
    if (capacity < *required) { ce::set_error(L"Output buffer too small; use required capacity."); return CE_INVALID_ARGUMENT; }
    memcpy(out, text.c_str(), *required * sizeof(wchar_t));
    return CE_OK;
}
}
uint32_t CE_CALL CE_GetApiVersionV2() { return CE_V2_VERSION; }
int CE_CALL CE_FirstScanV2(const CeScanRequestV2* request) {
    if (!valid_v2(request) || request->rounding > CE_ROUND_TRUNCATED) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.first_scan_v2(*request); });
}
int CE_CALL CE_NextScanV2(const CeScanRequestV2* request) {
    if (!valid_v2(request) || request->rounding > CE_ROUND_TRUNCATED) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.next_scan_v2(*request); });
}
int CE_CALL CE_GetScanStatusV2(CeScanStatusV2* status) {
    if (!valid_v2(status) || status->reserved1 || status->reserved2) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.scan_status_v2(*status); });
}
int CE_CALL CE_GetScanInfoV2(CeScanInfoV2* info) {
    if (!valid_v2(info)) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.scan_info_v2(*info); });
}
int CE_CALL CE_GetResultV2(uint64_t generation, uint64_t index, CeResultV2* info,
                         void* bytes, uint32_t capacity, uint32_t* required) {
    if (!valid_v2(info) || !required || (!bytes && capacity)) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) {
        std::vector<uint8_t> data;
        const auto status = c.result_v2(generation, index, *info, data);
        if (status != CE_OK) return status;
        *required = static_cast<uint32_t>(data.size());
        if (!bytes && !capacity) return static_cast<int>(CE_OK);
        if (capacity < data.size()) { ce::set_error(L"Result buffer too small."); return static_cast<int>(CE_INVALID_ARGUMENT); }
        if (!data.empty()) memcpy(bytes, data.data(), data.size());
        return static_cast<int>(CE_OK);
    });
}
int CE_CALL CE_NewScanV2() { return invoke([](ce::Core& c) { return c.new_scan_v2(); }); }
int CE_CALL CE_UndoScanV2() { return invoke([](ce::Core& c) { return c.undo_scan_v2(); }); }
int CE_CALL CE_ReadBytesV2(uint64_t address, void* bytes, uint32_t length) {
    if (!bytes || !length || length > CE_V2_MAX_VALUE_BYTES) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.read_bytes_v2(address, bytes, length); });
}
int CE_CALL CE_WriteBytesV2(uint64_t address, const void* bytes, uint32_t length) {
    if (!bytes || !length || length > CE_V2_MAX_VALUE_BYTES) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.write_bytes_v2(address, bytes, length); });
}
int CE_CALL CE_WriteValueV2(uint64_t address, uint32_t type, uint32_t flags, uint32_t length, const wchar_t* value) {
    if (!value) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.write_value_v2(address, type, flags, length, value); });
}
int CE_CALL CE_FormatValueV2(uint32_t type, uint32_t flags, const void* bytes, uint32_t length,
                           wchar_t* text, uint32_t capacity, uint32_t* required) {
    if (!bytes || !length || length > CE_V2_MAX_VALUE_BYTES || !required || (!text && capacity)) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) {
        std::wstring formatted;
        const auto status = c.format_value_v2(type, flags, bytes, length, formatted);
        return status == CE_OK ? copy_text_v2(formatted, text, capacity, required) : status;
    });
}
int CE_CALL CE_ResolveAddressV2(const wchar_t* expression, uint64_t* address) {
    if (!expression || !address) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.resolve_address_v2(expression, *address); });
}
int CE_CALL CE_ResolvePointerV2(const CeAddressV2* address, uint64_t* resolved) {
    if (!address || !resolved || address->reserved || address->offset_count > CE_V2_MAX_OFFSETS) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.resolve_pointer_v2(*address, *resolved); });
}
int CE_CALL CE_UpsertRecordV2(const CeRecordRequestV2* record, uint64_t* id) {
    if (!valid_v2(record) || !id || record->reserved) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.upsert_record_v2(*record, *id); });
}
int CE_CALL CE_RecordCountV2(uint64_t* count) {
    if (!count) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.record_count_v2(*count); });
}
int CE_CALL CE_GetRecordV2(uint64_t index, CeRecordInfoV2* info, wchar_t* description,
                         uint32_t capacity, uint32_t* required) {
    if (!valid_v2(info) || !required || (!description && capacity)) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) {
        std::wstring text;
        const auto status = c.record_v2(index, *info, text);
        return status == CE_OK ? copy_text_v2(text, description, capacity, required) : status;
    });
}
int CE_CALL CE_RemoveRecordV2(uint64_t id) { return invoke([&](ce::Core& c) { return c.remove_record_v2(id); }); }
int CE_CALL CE_WriteRecordV2(uint64_t id, const wchar_t* value) {
    if (!value) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.write_record_v2(id, value); });
}
int CE_CALL CE_FreezeRecordV2(uint64_t id, int enabled) {
    if (enabled != 0 && enabled != 1) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.freeze_record_v2(id, enabled != 0); });
}
int CE_CALL CE_SaveTableV2(const wchar_t* path) {
    if (!path || !*path) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.save_table_v2(path); });
}
int CE_CALL CE_LoadTableV2(const wchar_t* path) {
    if (!path || !*path) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.load_table_v2(path); });
}
int CE_CALL CE_ToggleWindowV2() {
    if (state.load() != CE_RUNNING) return CE_NOT_RUNNING;
    auto window = main_window.load();
    return window && PostMessageW(window, WM_APP + 3, 0, 0) ? CE_OK : CE_NOT_RUNNING;
}

// The three process views share one shape: validate, enumerate into a vector, then
// either report the element count or copy the whole thing. A short buffer is never
// partially written, matching copy_text_v2 and CE_GetResultV2.
template<class T, class Walk>
int view_export(T* items, uint32_t capacity, uint32_t* required, Walk walk) {
    if (!required || (!items && capacity) || capacity > CE_V2_MAX_VIEW_ITEMS) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) {
        std::vector<T> all;
        const auto status = walk(c, all);
        if (status != CE_OK) return status;
        if (all.size() > CE_V2_MAX_VIEW_ITEMS) return int(CE_UNSUPPORTED);
        *required = static_cast<uint32_t>(all.size());
        // A null buffer with zero capacity is the size query, not a short buffer.
        if (!items && !capacity) return int(CE_OK);
        if (capacity < all.size()) {
            ce::set_error(L"Output buffer too small; use required capacity.");
            return int(CE_INVALID_ARGUMENT);
        }
        if (!all.empty()) memcpy(items, all.data(), all.size() * sizeof(T));
        return int(CE_OK);
    });
}
int CE_CALL CE_GetRegionsV2(CeRegionInfoV2* regions, uint32_t capacity, uint32_t* required) {
    return view_export(regions, capacity, required, [](ce::Core& c, std::vector<CeRegionInfoV2>& out) { return c.regions_v2(out); });
}
int CE_CALL CE_GetModulesV2(CeModuleInfoV2* modules, uint32_t capacity, uint32_t* required) {
    return view_export(modules, capacity, required, [](ce::Core& c, std::vector<CeModuleInfoV2>& out) { return c.modules_v2(out); });
}
int CE_CALL CE_GetThreadsV2(CeThreadInfoV2* threads, uint32_t capacity, uint32_t* required) {
    return view_export(threads, capacity, required, [](ce::Core& c, std::vector<CeThreadInfoV2>& out) { return c.threads_v2(out); });
}
int CE_CALL CE_UnloadModuleV2(uint64_t base) {
    return invoke([&](ce::Core& c) { return c.unload_module_v2(base); });
}

int CE_CALL CE_PointerScanV2(const CePointerScanRequestV2* request) {
    if (!valid_v2(request) || !request->levels || request->levels > CE_V2_MAX_POINTER_LEVELS) return CE_INVALID_ARGUMENT;
    if (request->alignment != 1 && request->alignment != 2 && request->alignment != 4 && request->alignment != 8) return CE_INVALID_ARGUMENT;
    if (request->max_offset > ce::pointerscan::max_offset_limit || !request->target) return CE_INVALID_ARGUMENT;
    if (request->flags & ~static_cast<uint32_t>(CE_PTRSCAN_STATIC_ONLY | CE_PTRSCAN_NO_LOOP)) return CE_INVALID_ARGUMENT;
    if (request->reserved1 || request->reserved2) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.pointer_scan_v2(*request); });
}
// The cancellation epoch is shared with the value scan, so this is the same request; only
// one of the two jobs can be running at a time.
int CE_CALL CE_CancelPointerScanV2() {
    CE_CancelScan();
    return CE_OK;
}
int CE_CALL CE_GetPointerScanStatusV2(CePointerScanStatusV2* status) {
    if (!valid_v2(status) || status->reserved1 || status->reserved2) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.pointer_scan_status_v2(*status); });
}
int CE_CALL CE_GetPointerScanResultV2(uint64_t generation, uint64_t index, CePointerScanResultV2* result) {
    if (!valid_v2(result) || result->reserved) return CE_INVALID_ARGUMENT;
    return invoke([&](ce::Core& c) { return c.pointer_scan_result_v2(generation, index, *result); });
}

uint32_t CE_CALL CE_GetLastError(wchar_t* buffer, uint32_t capacity) {
    try {
        const auto& text = ce::last_error();
        const auto required = static_cast<uint32_t>(text.size() + 1);
        if (buffer && capacity) {
            auto n = (std::min)(text.size(), static_cast<size_t>(capacity - 1));
            memcpy(buffer, text.data(), n * sizeof(wchar_t));
            buffer[n] = L'\0';
        }
        return required;
    } catch (...) { return 0; }
}
