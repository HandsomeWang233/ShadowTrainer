// Every button action in the web UI, driven without a browser.
//
// Mode A (always) runs a Session against a fake CE_* implementation: request
// construction, refusals, paging clamps, event shape and the worker handoff are
// all observable here with no host process and no window.
//
// Mode B (when a DLL path is given) points the same Session at the real
// shadowtrainer.dll through GetProcAddress and walks the same commands end to end,
// which is what keeps the bridge and the core from drifting apart.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ui_bridge.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "json_min.inc"   // inline definitions: safe in a second translation unit

using namespace ce::ui;
namespace json = ce::json;

namespace {

int checks = 0;
void check(bool condition, const char* scenario) {
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", scenario);
        std::exit(1);
    }
}

bool contains(const std::wstring& haystack, const wchar_t* needle) {
    return haystack.find(needle) != std::wstring::npos;
}

// ---- reply helpers ---------------------------------------------------------------

// Asserts success and returns the reply's data object (never null on success).
const json::Value* send_ok(Session& session, const wchar_t* command, json::Value& storage) {
    ++checks;
    const std::wstring text = session.dispatch(command);
    if (!json::parse(text, storage)) {
        std::fprintf(stderr, "FAIL: reply is not valid JSON: %ls\n", text.c_str());
        std::exit(1);
    }
    const json::Value* flag = storage.find(L"ok");
    if (!flag || !flag->as_bool()) {
        std::fprintf(stderr, "FAIL: command refused: %ls\n      reply: %ls\n", command, text.c_str());
        std::exit(1);
    }
    const json::Value* payload = storage.find(L"data");
    return payload ? payload : &storage;
}

// Asserts the command is refused with a specific CE status.
void expect_failure(Session& session, const wchar_t* command, int code, const char* scenario) {
    ++checks;
    json::Value parsed;
    const std::wstring text = session.dispatch(command);
    if (!json::parse(text, parsed)) {
        std::fprintf(stderr, "FAIL: %s (reply is not JSON: %ls)\n", scenario, text.c_str());
        std::exit(1);
    }
    const json::Value* flag = parsed.find(L"ok");
    const json::Value* error = parsed.find(L"error");
    const int actual = error && error->find(L"code")
                           ? static_cast<int>(error->find(L"code")->as_i64(-1))
                           : -1;
    if (!flag || flag->as_bool() || actual != code) {
        std::fprintf(stderr, "FAIL: %s (wanted %d, got ok=%d code=%d)\n", scenario,
                     code, flag ? static_cast<int>(flag->as_bool()) : -1, actual);
        std::exit(1);
    }
}

struct Event {
    json::Value root;
    bool present = false;
};

// Drains one pump tick and parses whatever it publishes.
Event tick(Session& session) {
    Event event;
    const std::wstring text = session.pump(true, false);
    if (text.empty()) return event;
    event.present = json::parse(text, event.root);
    return event;
}

// Runs the tick loop until the worker is gone. The tick that observes the
// completion is also the one that publishes the finished scan, so the last
// non-empty event is handed back rather than thrown away.
Event pump_until_idle(Session& session) {
    Event last;
    for (int i = 0; i < 8000 && session.worker_running(); ++i) {
        Event event = tick(session);
        if (event.present) last = std::move(event);
        Sleep(1);
    }
    if (session.worker_running())
        std::fprintf(stderr, "worker still running after 8000 ticks\n");
    check(!session.worker_running(), "worker finished within the test budget");
    return last;
}

// Keeps whatever the wait published, plus anything the next tick adds.
Event settle(Session& session) {
    Event event = pump_until_idle(session);
    if (!event.present) event = tick(session);
    return event;
}

std::wstring field_text(const json::Value* node, const wchar_t* name) {
    if (!node) return L"<no node>";
    const json::Value* value = node->find(name);
    return value ? std::wstring(value->as_string()) : std::wstring(L"<missing>");
}

bool field_bool(const json::Value* node, const wchar_t* name) {
    const json::Value* value = node ? node->find(name) : nullptr;
    return value && value->as_bool();
}

int64_t field_int(const json::Value* node, const wchar_t* name, int64_t fallback = -1) {
    const json::Value* value = node ? node->find(name) : nullptr;
    return value ? value->as_i64(fallback) : fallback;
}

// ---- fake core -------------------------------------------------------------------

struct FakeCore {
    bool hold_scan = false;              // keep the scan "running" for busy tests
    CeScanRequestV2 last_request{};
    // The request's value pointers belong to the worker's captured strings, which
    // die with the thread; the fake keeps copies so assertions are not a
    // use-after-free.
    std::wstring last_value, last_value2;
    bool saw_request = false;
    CeScanStatusV2 status{};
    CeScanInfoV2 info{};
    std::vector<uint64_t> result_addresses;
    int busy_status_calls = 0;           // simulate CE_BUSY snapshot misses

    struct Record {
        uint64_t id = 0;
        uint32_t type = 0, flags = 0, byte_length = 0, frozen = 0, last_status = 0;
        uint64_t resolved = 0;
        CeAddressV2 address{};   // the chain, not just where it landed
        std::wstring description;
    };
    std::vector<Record> records;
    uint64_t next_id = 1;

    static constexpr uint64_t base = 0x100000;
    std::vector<uint8_t> memory = std::vector<uint8_t>(512, 0);

    std::vector<CeRegionInfoV2> regions;
    std::vector<CeModuleInfoV2> modules;
    std::vector<CeThreadInfoV2> threads;

    CePointerScanStatusV2 pointer{};
    CePointerScanResultV2 pointer_result{};

    uint64_t unloaded_base = 0;          // last base handed to unload_module_v2
    int unload_calls = 0;
    int unload_status = CE_OK;           // what the next unload answers

    std::wstring ct_path;
    uint64_t last_write_value_address = 0;
    std::wstring last_write_value_text;
};

FakeCore core;

int CE_CALL fake_resolve_address(const wchar_t* expression, uint64_t* address) {
    if (!expression || !address) return CE_INVALID_ARGUMENT;
    if (std::wcscmp(expression, L"buffer") == 0) {
        *address = FakeCore::base;
        return CE_OK;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(expression, &end, 0);
    if (end == expression || (end && *end)) return CE_INVALID_ARGUMENT;
    *address = parsed;
    return CE_OK;
}

int CE_CALL fake_resolve_pointer(const CeAddressV2* chain, uint64_t* resolved) {
    if (!chain || !resolved) return CE_INVALID_ARGUMENT;
    uint64_t value = chain->base;
    // The fake treats every hop as "read a pointer, then add the offset", which
    // is enough to prove the bridge orders and signs offsets the way the core
    // expects.
    for (uint32_t i = 0; i < chain->offset_count && i < CE_V2_MAX_OFFSETS; ++i) {
        if (value < FakeCore::base || value + 8 > FakeCore::base + core.memory.size())
            return CE_ACCESS_ERROR;
        uint64_t hop = 0;
        std::memcpy(&hop, core.memory.data() + (value - FakeCore::base), sizeof(hop));
        value = hop + static_cast<uint64_t>(chain->offsets[i]);
    }
    *resolved = value;
    return CE_OK;
}

int CE_CALL fake_read_bytes(uint64_t address, void* bytes, uint32_t length) {
    if (!bytes || !length) return CE_INVALID_ARGUMENT;
    if (address < FakeCore::base || address + length > FakeCore::base + core.memory.size())
        return CE_ACCESS_ERROR;
    std::memcpy(bytes, core.memory.data() + (address - FakeCore::base), length);
    return CE_OK;
}

int CE_CALL fake_write_bytes(uint64_t address, const void* bytes, uint32_t length) {
    if (!bytes || !length) return CE_INVALID_ARGUMENT;
    if (address < FakeCore::base || address + length > FakeCore::base + core.memory.size())
        return CE_ACCESS_ERROR;
    std::memcpy(core.memory.data() + (address - FakeCore::base), bytes, length);
    return CE_OK;
}

int CE_CALL fake_write_value(uint64_t address, uint32_t, uint32_t, uint32_t, const wchar_t* value) {
    core.last_write_value_address = address;
    core.last_write_value_text = value ? value : L"";
    return CE_OK;
}

int CE_CALL fake_format_value(uint32_t, uint32_t, const void* bytes, uint32_t length, wchar_t* text,
                              uint32_t capacity, uint32_t* required) {
    const uint8_t first = length ? *static_cast<const uint8_t*>(bytes) : 0;
    wchar_t buffer[32]{};
    std::swprintf(buffer, std::size(buffer), L"v%u", static_cast<unsigned>(first));
    const uint32_t needed = static_cast<uint32_t>(std::wcslen(buffer) + 1);
    if (required) *required = needed;
    // A null buffer with zero capacity is the documented size query, not an error.
    if (!text || capacity < needed) return CE_OK;
    std::wmemcpy(text, buffer, needed);
    return CE_OK;
}

int CE_CALL fake_get_scan_status(CeScanStatusV2* status) {
    if (!status) return CE_INVALID_ARGUMENT;
    if (core.busy_status_calls > 0) {
        --core.busy_status_calls;
        return CE_BUSY;
    }
    // Letting go of hold_scan ends the fake scan, the way a real cancellation
    // does: the session must stop treating the job as busy on the next tick.
    if (!core.hold_scan && core.status.active) {
        core.status.active = 0;
        core.status.cancel_requested = 1;
        core.status.phase = CE_SCAN_CANCELLED;
    }
    if (!core.status.size) {
        status->size = sizeof(*status);
        status->version = CE_V2_VERSION;
        status->phase = CE_SCAN_IDLE;
        return CE_OK;
    }
    *status = core.status;
    status->size = sizeof(*status);
    status->version = CE_V2_VERSION;
    return CE_OK;
}

int CE_CALL fake_first_scan(const CeScanRequestV2* request) {
    if (!request) return CE_INVALID_ARGUMENT;
    core.last_request = *request;
    core.last_value = request->value ? request->value : L"";
    core.last_value2 = request->value2 ? request->value2 : L"";
    core.saw_request = true;
    core.status = CeScanStatusV2{};
    core.status.size = sizeof(core.status);
    core.status.version = CE_V2_VERSION;
    core.status.generation = 4;
    core.status.operation_id = 8;
    if (core.hold_scan) {
        core.status.phase = CE_SCAN_READING;
        core.status.active = 1;
        core.status.unit = CE_SCAN_UNIT_BYTES;
        core.status.processed = 1024;
        core.status.total = 4096;
        core.status.total_known = 1;
        return CE_OK;
    }
    core.status.phase = CE_SCAN_COMPLETED;
    core.status.has_scan = 1;
    core.status.can_undo = 1;
    core.info.size = sizeof(core.info);
    core.info.version = CE_V2_VERSION;
    core.info.type = request->type;
    core.info.flags = request->flags;
    core.info.byte_length = 1;
    core.info.alignment = request->alignment;
    core.info.generation = 4;
    core.info.count = core.result_addresses.size();
    return CE_OK;
}

int CE_CALL fake_get_scan_info(CeScanInfoV2* info) {
    if (!info) return CE_INVALID_ARGUMENT;
    *info = core.info;
    info->size = sizeof(*info);
    info->version = CE_V2_VERSION;
    return CE_OK;
}

int CE_CALL fake_get_result(uint64_t generation, uint64_t index, CeResultV2* info, void* bytes,
                            uint32_t capacity, uint32_t* required) {
    if (!info || generation != core.info.generation) return CE_BUSY;
    if (index >= core.result_addresses.size()) return CE_INVALID_ARGUMENT;
    const uint32_t width = core.info.byte_length ? core.info.byte_length : 1;
    if (required) *required = width;
    if (!bytes || capacity < width) return CE_INVALID_ARGUMENT;
    const uint64_t address = core.result_addresses[static_cast<size_t>(index)];
    if (fake_read_bytes(address, bytes, width) != CE_OK) return CE_ACCESS_ERROR;
    info->size = sizeof(*info);
    info->version = CE_V2_VERSION;
    info->type = core.info.type;
    info->flags = core.info.flags;
    info->byte_length = width;
    info->generation = generation;
    info->address = address;
    return CE_OK;
}

int CE_CALL fake_new_scan() {
    core.info = CeScanInfoV2{};
    core.status = CeScanStatusV2{};
    core.result_addresses.clear();
    return CE_OK;
}

int CE_CALL fake_undo_scan() { return CE_OK; }
void CE_CALL fake_cancel_scan() {}

int CE_CALL fake_upsert_record(const CeRecordRequestV2* request, uint64_t* id) {
    if (!request || !id) return CE_INVALID_ARGUMENT;
    if (request->id) {
        for (auto& record : core.records)
            if (record.id == request->id) {
                record.type = request->type;
                record.flags = request->flags;
                record.byte_length = request->byte_length;
                record.description = request->description ? request->description : L"";
                record.address = request->address;
                *id = record.id;
                return CE_OK;
            }
        return CE_INVALID_ARGUMENT;
    }
    FakeCore::Record record;
    record.id = core.next_id++;
    record.type = request->type;
    record.flags = request->flags;
    record.byte_length = request->byte_length;
    record.description = request->description ? request->description : L"";
    record.resolved = request->address.base;
    record.address = request->address;
    core.records.push_back(record);
    *id = record.id;
    return CE_OK;
}

int CE_CALL fake_record_count(uint64_t* count) {
    if (!count) return CE_INVALID_ARGUMENT;
    *count = core.records.size();
    return CE_OK;
}

int CE_CALL fake_get_record(uint64_t index, CeRecordInfoV2* info, wchar_t* description,
                            uint32_t capacity, uint32_t* required) {
    if (!info || index >= core.records.size()) return CE_INVALID_ARGUMENT;
    const auto& record = core.records[static_cast<size_t>(index)];
    const uint32_t needed = static_cast<uint32_t>(record.description.size() + 1);
    if (required) *required = needed;
    // Same size-query rule as the formatter: null and zero asks for the length.
    if (!description || capacity < needed) return CE_OK;
    std::wmemcpy(description, record.description.c_str(), needed);
    info->size = sizeof(*info);
    info->version = CE_V2_VERSION;
    info->id = record.id;
    info->type = record.type;
    info->flags = record.flags;
    info->byte_length = record.byte_length;
    info->frozen = record.frozen;
    info->last_status = record.last_status;
    info->resolved_address = record.resolved;
    info->address = record.address;
    return CE_OK;
}

int CE_CALL fake_remove_record(uint64_t id) {
    for (size_t i = 0; i < core.records.size(); ++i)
        if (core.records[i].id == id) {
            core.records.erase(core.records.begin() + static_cast<ptrdiff_t>(i));
            return CE_OK;
        }
    return CE_INVALID_ARGUMENT;
}

int CE_CALL fake_write_record(uint64_t id, const wchar_t*) {
    for (const auto& record : core.records)
        if (record.id == id) return CE_OK;
    return CE_INVALID_ARGUMENT;
}

int CE_CALL fake_freeze_record(uint64_t id, int enabled) {
    for (auto& record : core.records)
        if (record.id == id) {
            record.frozen = enabled ? 1u : 0u;
            return CE_OK;
        }
    return CE_INVALID_ARGUMENT;
}

int CE_CALL fake_save_table(const wchar_t* path) {
    core.ct_path = path ? path : L"";
    return CE_OK;
}

int CE_CALL fake_table_dialog(const wchar_t* path) {
    core.ct_path = path ? path : L"";
    return CE_OK;
}

int CE_CALL fake_get_regions(CeRegionInfoV2* items, uint32_t capacity, uint32_t* required) {
    if (!required) return CE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(core.regions.size());
    if (!items || capacity < core.regions.size()) return CE_OK;
    for (size_t i = 0; i < core.regions.size(); ++i) {
        items[i] = core.regions[i];
        items[i].size = sizeof(CeRegionInfoV2);
        items[i].version = CE_V2_VERSION;
    }
    return CE_OK;
}

int CE_CALL fake_get_modules(CeModuleInfoV2* items, uint32_t capacity, uint32_t* required) {
    if (!required) return CE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(core.modules.size());
    if (!items || capacity < core.modules.size()) return CE_OK;
    for (size_t i = 0; i < core.modules.size(); ++i) {
        items[i] = core.modules[i];
        items[i].size = sizeof(CeModuleInfoV2);
        items[i].version = CE_V2_VERSION;
    }
    return CE_OK;
}

int CE_CALL fake_unload_module(uint64_t base) {
    ++core.unload_calls;
    if (core.unload_status != CE_OK) return core.unload_status;
    core.unloaded_base = base;
    // A real unload takes the module out of the next enumeration, so the reply's
    // re-read list is one row shorter.
    core.modules.erase(std::remove_if(core.modules.begin(), core.modules.end(),
                                      [base](const CeModuleInfoV2& module) { return module.base == base; }),
                       core.modules.end());
    return CE_OK;
}

int CE_CALL fake_get_threads(CeThreadInfoV2* items, uint32_t capacity, uint32_t* required) {
    if (!required) return CE_INVALID_ARGUMENT;
    *required = static_cast<uint32_t>(core.threads.size());
    if (!items || capacity < core.threads.size()) return CE_OK;
    for (size_t i = 0; i < core.threads.size(); ++i) {
        items[i] = core.threads[i];
        items[i].size = sizeof(CeThreadInfoV2);
        items[i].version = CE_V2_VERSION;
    }
    return CE_OK;
}

int CE_CALL fake_pointer_scan(const CePointerScanRequestV2* request) {
    if (!request) return CE_INVALID_ARGUMENT;
    core.pointer.size = sizeof(core.pointer);
    core.pointer.version = CE_V2_VERSION;
    core.pointer.phase = CE_SCAN_COMPLETED;
    core.pointer.active = 0;
    core.pointer.total = 1;
    core.pointer.processed = 12;
    core.pointer.generation = 21;
    core.pointer.operation_id = 5;
    core.pointer_result.size = sizeof(core.pointer_result);
    core.pointer_result.version = CE_V2_VERSION;
    core.pointer_result.level_count = 2;
    core.pointer_result.base = 0x7FF00000;
    core.pointer_result.offsets[0] = 0x10;
    core.pointer_result.offsets[1] = -0x8;
    return CE_OK;
}

int CE_CALL fake_cancel_pointer_scan() { return CE_OK; }

int CE_CALL fake_pointer_status(CePointerScanStatusV2* status) {
    if (!status) return CE_INVALID_ARGUMENT;
    *status = core.pointer;
    status->size = sizeof(*status);
    status->version = CE_V2_VERSION;
    return CE_OK;
}

int CE_CALL fake_pointer_result(uint64_t generation, uint64_t index, CePointerScanResultV2* result) {
    if (!result || generation != core.pointer.generation || index != 0) return CE_INVALID_ARGUMENT;
    *result = core.pointer_result;
    return CE_OK;
}

uint32_t CE_CALL fake_get_host_pid() { return 4242; }
uint32_t CE_CALL fake_get_api_version() { return 2; }
uint32_t CE_CALL fake_get_last_error(wchar_t* buffer, uint32_t capacity) {
    if (buffer && capacity) buffer[0] = L'\0';
    return 1;
}
int CE_CALL fake_request_stop() { return CE_OK; }

CeApi fake_api() {
    CeApi api{};
    api.get_api_version_v2 = &fake_get_api_version;
    api.get_host_pid = &fake_get_host_pid;
    api.get_last_error = &fake_get_last_error;
    api.request_stop = &fake_request_stop;
    api.get_scan_status_v2 = &fake_get_scan_status;
    api.first_scan_v2 = &fake_first_scan;
    api.next_scan_v2 = &fake_first_scan;
    api.get_scan_info_v2 = &fake_get_scan_info;
    api.get_result_v2 = &fake_get_result;
    api.new_scan_v2 = &fake_new_scan;
    api.undo_scan_v2 = &fake_undo_scan;
    api.cancel_scan = &fake_cancel_scan;
    api.read_bytes_v2 = &fake_read_bytes;
    api.write_bytes_v2 = &fake_write_bytes;
    api.write_value_v2 = &fake_write_value;
    api.format_value_v2 = &fake_format_value;
    api.resolve_address_v2 = &fake_resolve_address;
    api.resolve_pointer_v2 = &fake_resolve_pointer;
    api.upsert_record_v2 = &fake_upsert_record;
    api.record_count_v2 = &fake_record_count;
    api.get_record_v2 = &fake_get_record;
    api.remove_record_v2 = &fake_remove_record;
    api.write_record_v2 = &fake_write_record;
    api.freeze_record_v2 = &fake_freeze_record;
    api.save_table_v2 = &fake_save_table;
    api.load_table_v2 = &fake_table_dialog;
    api.get_regions_v2 = &fake_get_regions;
    api.get_modules_v2 = &fake_get_modules;
    api.get_threads_v2 = &fake_get_threads;
    api.unload_module_v2 = &fake_unload_module;
    api.pointer_scan_v2 = &fake_pointer_scan;
    api.cancel_pointer_scan_v2 = &fake_cancel_pointer_scan;
    api.get_pointer_scan_status_v2 = &fake_pointer_status;
    api.get_pointer_scan_result_v2 = &fake_pointer_result;
    return api;
}

// ---- mode A ----------------------------------------------------------------------

void mode_a() {
    Session session(fake_api());
    int64_t host_min = 0, host_max = 0;
    Session::Host host;
    int window_commands = 0;
    host.window = [&](const std::wstring&, const json::Value*) { ++window_commands; };
    session.host() = host;

    // ---- envelope handling
    expect_failure(session, L"not json at all", CE_INVALID_ARGUMENT, "garbage is refused");
    expect_failure(session, L"{\"id\":1}", CE_INVALID_ARGUMENT, "a command without a name is refused");
    expect_failure(session, L"{\"id\":2,\"cmd\":\"nope\"}", CE_INVALID_ARGUMENT,
                   "an unknown command is refused");

    // ---- hello
    {
        json::Value storage;
        const json::Value* data = send_ok(session, L"{\"id\":1,\"cmd\":\"hello\"}", storage);
        check(field_bool(data, L"full"), "hello marks the model as full");
        check(!field_text(data, L"status").empty(), "hello carries a status line");
        check(field_text(data, L"status") != L"<missing>", "the status field is present");
        const json::Value* host_info = data->find(L"host");
        check(host_info && field_int(host_info, L"pid") == 4242, "hello carries the host pid");
        host_min = field_int(host_info, L"hostMin");
        host_max = field_int(host_info, L"hostMax");
        check(host_min > 0 && host_max > host_min, "hello carries this host's address range");
        const json::Value* pid = host_info ? host_info->find(L"pid") : nullptr;
        check(pid && pid->kind == json::Value::Kind::String,
              "64-bit values cross the bridge as strings");
        check(field_bool(data->find(L"controls"), L"idle"), "a fresh session is idle");
        check(field_bool(data->find(L"scan"), L"hasScan") == false, "a fresh session has no scan");
    }

    // ---- window commands reach the host and nothing else
    {
        json::Value storage;
        send_ok(session, L"{\"id\":2,\"cmd\":\"window.drag\"}", storage);
        send_ok(session, L"{\"id\":3,\"cmd\":\"window.minimize\"}", storage);
        check(window_commands == 2, "both window commands reached the host hook");
    }

    // ---- scan request construction
    core = FakeCore{};
    core.result_addresses = {FakeCore::base + 3, FakeCore::base + 9};
    core.memory[3] = 42;
    core.memory[9] = 43;
    const wchar_t* between =
        L"{\"id\":4,\"cmd\":\"scan.first\",\"args\":{\"type\":2,\"signed\":true,\"hex\":false,"
        L"\"comparison\":4,\"alignment\":4,\"rounding\":0,\"byteLength\":\"0\",\"begin\":\"1000\","
        L"\"end\":\"2000\",\"value\":\"100\",\"second\":\"200\"}}";
    {
        json::Value storage;
        send_ok(session, between, storage);
    }
    // The tick that sees the worker finish is the one that publishes the scan,
    // so its event is kept rather than drained and dropped.
    const Event done = pump_until_idle(session);
    {
        check(core.saw_request, "the core saw a scan request");
        check(core.last_request.type == CE_TYPE_U32, "the value type is forwarded");
        check(core.last_request.flags == CE_VALUE_SIGNED, "signed becomes CE_VALUE_SIGNED");
        check(core.last_request.comparison == CE_CMP_BETWEEN, "the comparison is forwarded");
        check(core.last_request.alignment == 4, "the alignment is forwarded");
        // A fixed-width type ignores the field entirely, exactly as the old UI
        // did; only a variable-width type carries a typed width through.
        check(core.last_request.byte_length == 4, "a fixed-width type supplies its own width");
        check(core.last_request.begin == 0x1000 && core.last_request.end == 0x2000,
              "a hex range is parsed");
        check(core.last_request.rounding == CE_ROUND_EXACT,
              "rounding is normalised away for a non-float type");
        check(core.last_value == L"100", "the value text reaches the core");
        check(core.last_value2 == L"200", "the second value reaches the core");
    }
    {
        check(done.present, "the pump publishes the finished scan");
        check(field_text(&done.root, L"kind") == L"event", "the envelope says event");
        const json::Value* rows = done.root.find(L"resultRows");
        const json::Value* list = rows ? rows->find(L"rows") : nullptr;
        check(list && list->items.size() == 2, "both result rows are published");
        check(field_text(&list->items[0], L"address") == L"0x0000000000100003",
              "row addresses are packed");
        check(field_text(&list->items[0], L"value") == L"v42", "row values are core-formatted");
        check(field_text(&list->items[0], L"number") == L"1", "row numbers are 1-based");
    }
    check(session.pump(true, false).empty(), "an idle tick publishes nothing");

    // A variable-width type is the one case where the typed width is forwarded.
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":5,\"cmd\":\"scan.first\",\"args\":{\"type\":6,\"byteLength\":\"12\","
                L"\"comparison\":0,\"value\":\"text\"}}",
                storage);
    }
    pump_until_idle(session);
    check(core.last_request.byte_length == 12, "a variable width is forwarded as typed");
    check(core.last_request.type == CE_TYPE_UTF8, "the UTF-8 type is forwarded");

    // ---- busy refusal
    core.hold_scan = true;
    {
        json::Value storage;
        send_ok(session, between, storage);
    }
    expect_failure(session, L"{\"id\":6,\"cmd\":\"scan.first\",\"args\":{\"type\":2}}", CE_BUSY,
                   "a scan started while one runs is refused as busy");
    expect_failure(session, L"{\"id\":7,\"cmd\":\"scan.new\"}", CE_BUSY,
                   "new scan is refused as busy");
    {
        json::Value storage;
        send_ok(session, L"{\"id\":8,\"cmd\":\"scan.cancel\"}", storage);
    }
    pump_until_idle(session);
    core.hold_scan = false;

    // A CE_BUSY status snapshot is a miss, not a state: the session must not
    // claim the scan ended, and it must not touch the data APIs that tick.
    core.busy_status_calls = 1;
    (void)tick(session);
    check(core.busy_status_calls == 0, "the busy snapshot was consumed exactly once");
    // The following tick sees a real snapshot again: a miss must not latch.
    (void)tick(session);

    // ---- scan format validation
    expect_failure(session, L"{\"id\":9,\"cmd\":\"scan.first\",\"args\":{\"type\":99}}",
                   CE_INVALID_ARGUMENT, "an unknown value type is refused");
    expect_failure(session,
                   L"{\"id\":10,\"cmd\":\"scan.first\",\"args\":{\"type\":7,\"byteLength\":\"3\"}}",
                   CE_INVALID_ARGUMENT, "a UTF-16 width must be even");
    expect_failure(session,
                   L"{\"id\":11,\"cmd\":\"scan.first\",\"args\":{\"type\":2,\"begin\":\"2000\","
                   L"\"end\":\"1000\"}}",
                   CE_INVALID_ARGUMENT, "an inverted range is refused");

    // ---- results paging and activation
    core = FakeCore{};
    core.result_addresses.assign(1200, FakeCore::base);
    core.memory.assign(512, 7);
    {
        json::Value storage;
        send_ok(session, L"{\"id\":12,\"cmd\":\"scan.first\",\"args\":{\"type\":0,\"byteLength\":\"1\"}}",
                storage);
    }
    (void)settle(session);
    {
        json::Value storage;
        const json::Value* results =
            send_ok(session, L"{\"id\":13,\"cmd\":\"results.page\",\"args\":{\"delta\":-512}}", storage);
        check(field_int(results, L"start") == 0, "paging backwards from the first page is clamped");
        check(field_int(results, L"total") == 1200, "the total comes from the scan info");
        check(contains(field_text(results, L"label"), L"1200 total"),
              "the page label names the total");
        check(contains(field_text(results, L"label"), L"512/page"), "the page label names the size");
    }
    {
        json::Value storage;
        const json::Value* last =
            send_ok(session, L"{\"id\":14,\"cmd\":\"results.page\",\"args\":{\"start\":\"99999\"}}", storage);
        check(field_int(last, L"start") == 1024, "an out-of-range page lands on the last one");
        const json::Value* back =
            send_ok(session, L"{\"id\":14,\"cmd\":\"results.page\",\"args\":{\"start\":\"0\"}}", storage);
        check(field_int(back, L"start") == 0, "paging back to the first page works");
    }
    core.memory[0] = 77;
    {
        json::Value storage;
        send_ok(session, L"{\"id\":15,\"cmd\":\"results.activate\",\"args\":{\"index\":0}}", storage);
        const json::Value* data = storage.find(L"data");
        check(field_text(data, L"base") == L"0x0000000000100000", "activation fills the base");
        check(field_text(data, L"value") == L"v77", "activation reads the live value");
        check(contains(field_text(data, L"current"), L"(snapshot)"),
              "activation reports the read as a snapshot");
    }
    expect_failure(session, L"{\"id\":16,\"cmd\":\"results.activate\",\"args\":{\"index\":99999}}",
                   CE_INVALID_ARGUMENT, "activating a missing row is refused");

    // ---- records
    core = FakeCore{};
    core.memory[0] = 5;
    expect_failure(session, L"{\"id\":17,\"cmd\":\"record.freeze\",\"args\":{\"enabled\":true}}",
                   CE_INVALID_ARGUMENT, "freezing without a selection is refused");
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":17,\"cmd\":\"record.add\",\"args\":{\"base\":\"0x100000\",\"offsets\":\"\","
                L"\"type\":0,\"signed\":true,\"byteLength\":\"1\",\"description\":\"health\","
                L"\"value\":\"5\",\"snapshot\":false}}",
                storage);
        check(core.records.size() == 1, "the core holds one record");
        check(core.records[0].type == CE_TYPE_U8, "the record keeps its type");
        check(core.records[0].description == L"health", "the record keeps its description");
    }
    {
        json::Value storage;
        // Adding selected the new record, so Freeze is live without a click.
        const json::Value* data =
            send_ok(session, L"{\"id\":19,\"cmd\":\"record.select\",\"args\":{\"index\":0}}", storage);
        check(field_text(data, L"description") == L"health", "selection returns the description");
        check(field_text(data, L"base") == L"0x0000000000100000", "selection returns the base");
        check(field_text(data, L"offsets").empty(), "an absolute record has no offsets");
        check(field_text(data, L"value") == L"v5", "selection reads the current value");
        check(field_bool(data, L"opaque") == false, "a normal record is not opaque");
    }
    {
        json::Value storage;
        send_ok(session, L"{\"id\":20,\"cmd\":\"record.freeze\",\"args\":{\"enabled\":true}}", storage);
        check(core.records[0].frozen == 1, "the core saw the freeze");
        send_ok(session, L"{\"id\":21,\"cmd\":\"record.write\",\"args\":{\"value\":\"9\"}}", storage);
        send_ok(session, L"{\"id\":22,\"cmd\":\"record.remove\"}", storage);
        check(core.records.empty(), "the record is gone");
    }

    // An imported CT row can be removed but never edited or frozen.
    {
        FakeCore::Record opaque;
        opaque.id = 7;
        opaque.type = CE_TYPE_U32;
        opaque.last_status = CE_UNSUPPORTED;
        opaque.resolved = 0x2000;
        opaque.description = L"script";
        opaque.address.base = 0x2000;
        core.records.push_back(opaque);
    }
    {
        json::Value storage;
        send_ok(session, L"{\"id\":23,\"cmd\":\"record.refresh\"}", storage);
        const json::Value* data =
            send_ok(session, L"{\"id\":24,\"cmd\":\"record.select\",\"args\":{\"index\":0}}", storage);
        check(field_bool(data, L"opaque"), "an imported row is flagged opaque");
        check(field_text(data, L"base").empty(), "an opaque row does not fill the editor");
        send_ok(session, L"{\"id\":25,\"cmd\":\"record.remove\"}", storage);
        check(core.records.empty(), "an opaque row can still be removed");
    }

    // ---- record sorting
    {
        // Two records in the wrong alphabetical order, so the sort has something
        // to change and the change is visible in what a display index means.
        FakeCore::Record beta;
        beta.id = 11;
        beta.type = CE_TYPE_U32;
        beta.resolved = 0x3000;
        beta.description = L"beta";
        beta.address.base = 0x3000;
        core.records.push_back(beta);
        FakeCore::Record alpha;
        alpha.id = 12;
        alpha.type = CE_TYPE_U32;
        alpha.resolved = 0x2000;
        alpha.description = L"alpha";
        alpha.address.base = 0x2000;
        core.records.push_back(alpha);
        json::Value storage;
        send_ok(session, L"{\"id\":62,\"cmd\":\"record.refresh\"}", storage);
        const json::Value* data =
            send_ok(session, L"{\"id\":63,\"cmd\":\"record.sort\",\"args\":{\"column\":1}}", storage);
        check(field_int(data, L"sortColumn") == 1 && !field_bool(data, L"sortDescending"),
              "the record sort reports its column and direction");
        // Display position 0 is now the alphabetically first record, not the first
        // one the core holds.
        const json::Value* selected =
            send_ok(session, L"{\"id\":64,\"cmd\":\"record.select\",\"args\":{\"index\":0}}", storage);
        check(field_text(selected, L"description") == L"alpha",
              "the order decides what a display position means");
        data = send_ok(session, L"{\"id\":65,\"cmd\":\"record.sort\",\"args\":{\"column\":1}}", storage);
        check(field_bool(data, L"sortDescending"), "clicking the same column again reverses it");
        selected = send_ok(session, L"{\"id\":66,\"cmd\":\"record.select\",\"args\":{\"index\":0}}", storage);
        check(field_text(selected, L"description") == L"beta", "and the reversed order is shown");
        core.records.clear();
        send_ok(session, L"{\"id\":67,\"cmd\":\"record.refresh\"}", storage);
    }

    // ---- CT file round trip through an explicit path
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":26,\"cmd\":\"record.saveCT\",\"args\":{\"path\":\"C:\\\\tmp\\\\t.CT\"}}",
                storage);
        check(core.ct_path == L"C:\\tmp\\t.CT", "the CT path survives the JSON round trip");
    }

    // ---- memory
    core = FakeCore{};
    core.memory[0] = 0xAB;
    core.memory[1] = 0xCD;
    core.memory[256] = 0x11;
    {
        json::Value storage;
        const json::Value* data = send_ok(
            session,
            L"{\"id\":27,\"cmd\":\"memory.resolve\",\"args\":{\"base\":\"0x100000\",\"offsets\":\"\"}}",
            storage);
        check(field_text(data, L"bytes").rfind(L"abcd", 0) == 0, "page bytes are lowercase hex");
        check(field_text(data, L"readable").size() == 256, "the readable mask covers 256 bytes");
        check(field_bool(data, L"canNext"), "the next page is allowed");
        check(field_bool(data, L"canBack") == false, "a fresh page has no history to go back to");
    }
    {
        json::Value storage;
        const json::Value* moved =
            send_ok(session, L"{\"id\":28,\"cmd\":\"memory.move\",\"args\":{\"delta\":256}}", storage);
        check(field_text(moved, L"bytes").rfind(L"11", 0) == 0, "moving reads the next page");
        check(field_bool(moved, L"canBack"), "history now has a back entry");
        const json::Value* back = send_ok(session, L"{\"id\":29,\"cmd\":\"memory.back\"}", storage);
        check(field_text(back, L"bytes").rfind(L"abcd", 0) == 0, "going back returns to the first page");
        const json::Value* forward = send_ok(session, L"{\"id\":30,\"cmd\":\"memory.forward\"}", storage);
        check(field_text(forward, L"bytes").rfind(L"11", 0) == 0, "going forward returns again");
    }
    expect_failure(session, L"{\"id\":31,\"cmd\":\"memory.forward\"}", CE_INVALID_ARGUMENT,
                   "forward past the end is refused");
    {
        json::Value storage;
        const json::Value* data = send_ok(
            session,
            L"{\"id\":32,\"cmd\":\"memory.readBytes\",\"args\":{\"address\":\"1048576\",\"length\":\"2\"}}",
            storage);
        check(field_text(data, L"hex") == L"abcd", "bytes read back in order");
    }
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":33,\"cmd\":\"memory.writeBytes\",\"args\":{\"address\":\"1048576\","
                L"\"hex\":\"0a0b\"}}",
                storage);
        check(core.memory[0] == 0x0a && core.memory[1] == 0x0b, "the written bytes landed");
        send_ok(session, L"{\"id\":34,\"cmd\":\"memory.undo\"}", storage);
        check(core.memory[0] == 0xab && core.memory[1] == 0xcd, "undo restored the old bytes");
    }
    expect_failure(session, L"{\"id\":35,\"cmd\":\"memory.undo\"}", CE_INVALID_ARGUMENT,
                   "undo with an empty stack is refused");
    expect_failure(session,
                   L"{\"id\":36,\"cmd\":\"memory.writeBytes\",\"args\":{\"address\":\"1048576\","
                   L"\"hex\":\"zz\"}}",
                   CE_INVALID_ARGUMENT, "a non-hex payload is refused");
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":37,\"cmd\":\"memory.fill\",\"args\":{\"address\":\"1048576\",\"length\":\"4\","
                L"\"byte\":\"ff\"}}",
                storage);
        check(core.memory[0] == 0xff && core.memory[3] == 0xff, "the fill covered the range");
        send_ok(session, L"{\"id\":38,\"cmd\":\"memory.undo\"}", storage);
        check(core.memory[0] == 0xab, "undo covers a fill too");
    }
    expect_failure(session,
                   L"{\"id\":39,\"cmd\":\"memory.fill\",\"args\":{\"address\":\"1048576\",\"length\":\"4\","
                   L"\"byte\":\"1ff\"}}",
                   CE_INVALID_ARGUMENT, "a fill byte above 0xff is refused");
    {
        json::Value storage;
        const json::Value* data = send_ok(
            session,
            L"{\"id\":40,\"cmd\":\"memory.page\",\"args\":{\"address\":\"1048880\"}}", storage);
        check(field_int(data, L"unreadable") > 0, "a half-readable page counts its unreadable bytes");
        check(field_text(data, L"readable").size() == 256, "the mask still covers the whole page");
    }
    {
        wchar_t outside[160]{};
        std::swprintf(outside, std::size(outside),
                      L"{\"id\":41,\"cmd\":\"memory.page\",\"args\":{\"address\":\"%lld\"}}",
                      static_cast<long long>(host_min - 256));
        expect_failure(session, outside, CE_INVALID_ARGUMENT,
                       "a page below the host range is refused");
        std::swprintf(outside, std::size(outside),
                      L"{\"id\":41,\"cmd\":\"memory.page\",\"args\":{\"address\":\"%lld\"}}",
                      static_cast<long long>(host_max - 32));
        expect_failure(session, outside, CE_INVALID_ARGUMENT,
                       "a page that would run past the host range is refused");
    }
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":42,\"cmd\":\"memory.writeValue\",\"args\":{\"address\":\"1048576\",\"type\":2,"
                L"\"flags\":1,\"byteLength\":\"4\",\"text\":\"1234\"}}",
                storage);
        check(core.last_write_value_address == FakeCore::base, "the typed write reaches the core");
        check(core.last_write_value_text == L"1234", "the literal reaches the core untouched");
    }

    // ---- views
    core = FakeCore{};
    CeRegionInfoV2 region{};
    region.base = 0x100000;
    region.region_size = 0x1000;
    region.state = MEM_COMMIT;
    region.protect = PAGE_READWRITE;
    region.type = MEM_PRIVATE;
    core.regions.push_back(region);
    CeModuleInfoV2 module{};
    module.base = 0x7FF00000;
    module.module_size = 0x2000;
    std::swprintf(module.path, std::size(module.path), L"%s", L"C:\\host\\host.exe");
    core.modules.push_back(module);
    CeThreadInfoV2 thread{};
    thread.thread_id = 1234;
    thread.valid_mask = 3;
    thread.priority = 8;
    thread.current = 1;
    std::swprintf(thread.description, std::size(thread.description), L"%s", L"main");
    core.threads.push_back(thread);
    {
        json::Value storage;
        send_ok(session, L"{\"id\":43,\"cmd\":\"view.load\",\"args\":{\"kind\":\"regions\"}}", storage);
        send_ok(session, L"{\"id\":44,\"cmd\":\"view.load\",\"args\":{\"kind\":\"modules\"}}", storage);
        send_ok(session, L"{\"id\":45,\"cmd\":\"view.load\",\"args\":{\"kind\":\"threads\"}}", storage);
    }
    expect_failure(session, L"{\"id\":46,\"cmd\":\"view.load\",\"args\":{\"kind\":\"nope\"}}",
                   CE_INVALID_ARGUMENT, "an unknown view kind is refused");
    {
        Event event = settle(session);
        check(event.present, "the views are published");
        const json::Value* views = event.root.find(L"views");
        check(views != nullptr, "the event carries a views section");
        const json::Value* region_rows = views->find(L"regions")->find(L"rows");
        check(region_rows->items.size() == 1, "the region row was published");
        check(field_text(&region_rows->items[0].items[0], L"") != L"", "region cells are strings");
        check(region_rows->items[0].items[0].as_string() == L"0x0000000000100000",
              "the region base is rendered");
        check(region_rows->items[0].items[2].as_string() == L"Commit", "the region state is decoded");
        check(region_rows->items[0].items[3].as_string() == L"Read+Write",
              "the region protection is decoded");
        check(region_rows->items[0].items[5].as_string() == L"Private", "the region type is decoded");
        const json::Value* module_rows = views->find(L"modules")->find(L"rows");
        check(module_rows->items[0].items[2].as_string() == L"host.exe", "the module name is split");
        const json::Value* thread_rows = views->find(L"threads")->find(L"rows");
        check(thread_rows->items[0].items[1].as_string() == L"8", "the thread priority is shown");
        check(thread_rows->items[0].items[4].as_string() == L"Current", "the current thread is marked");
        check(contains(field_text(views->find(L"regions"), L"label"), L"1 total"),
              "the view label counts its rows");
        check(views->find(L"modules")->find(L"ids")->items[0].as_string() == L"2146435072",
              "a module row carries the base its commands will name it by");
    }

    // ---- module unload
    {
        json::Value storage;
        // A stale page can only ask for a row it was actually shown, and only a
        // module can be asked for at all.
        expect_failure(session, L"{\"id\":49,\"cmd\":\"view.unload\",\"args\":{\"kind\":\"modules\",\"base\":\"4096\"}}",
                       CE_INVALID_ARGUMENT, "a base that is not in the list is refused");
        expect_failure(session, L"{\"id\":50,\"cmd\":\"view.unload\",\"args\":{\"kind\":\"regions\",\"base\":\"1048576\"}}",
                       CE_INVALID_ARGUMENT, "only a module can be unloaded");
        check(core.unload_calls == 0, "neither refusal reached the core");
        core.unload_status = CE_ACCESS_ERROR;
        expect_failure(session, L"{\"id\":51,\"cmd\":\"view.unload\",\"args\":{\"kind\":\"modules\",\"base\":\"2146435072\"}}",
                       CE_ACCESS_ERROR, "a refusal from the core is passed through");
        core.unload_status = CE_OK;
        const json::Value* data = send_ok(
            session, L"{\"id\":52,\"cmd\":\"view.unload\",\"args\":{\"kind\":\"modules\",\"base\":\"2146435072\"}}", storage);
        check(core.unloaded_base == 0x7FF00000, "the unload names the module by its base");
        check(data && field_int(data, L"count") == 0, "the reply is the module list, re-read after the unload");
        check(contains(field_text(data, L"label"), L"0 total"), "the unloaded module is gone from it");
        // The fake erased the module the way the loader would. Put it back: the
        // pointer test below needs a static base to find.
        core.modules.push_back(module);
    }

    // ---- sorting
    {
        // A second module gives the order something to change: the enumeration
        // runs by base, and the names are deliberately the other way round.
        CeModuleInfoV2 second{};
        second.base = 0x7FF10000;
        second.module_size = 0x1000;
        std::swprintf(second.path, std::size(second.path), L"%s", L"C:\\host\\alpha.dll");
        core.modules.push_back(second);
        json::Value storage;
        // The unload left the loaded view holding an empty list, and a load is
        // once per page: a refresh is what re-reads the host.
        send_ok(session, L"{\"id\":53,\"cmd\":\"view.refresh\",\"args\":{\"kind\":\"modules\"}}", storage);
        const json::Value* data = send_ok(
            session, L"{\"id\":54,\"cmd\":\"view.sort\",\"args\":{\"kind\":\"modules\",\"column\":2}}", storage);
        check(field_int(data, L"sortColumn") == 2 && !field_bool(data, L"sortDescending"),
              "the reply reports the column and the direction");
        const json::Value* rows = data->find(L"rows");
        check(rows && rows->items.size() == 2 &&
                  rows->items[0].items[2].as_string() == L"alpha.dll",
              "sorting by name puts the alphabetically first module on top");
        data = send_ok(session, L"{\"id\":55,\"cmd\":\"view.sort\",\"args\":{\"kind\":\"modules\",\"column\":2}}", storage);
        rows = data->find(L"rows");
        check(field_bool(data, L"sortDescending") && rows->items[0].items[2].as_string() == L"host.exe",
              "clicking the same header again reverses it");
        data = send_ok(session, L"{\"id\":56,\"cmd\":\"view.sort\",\"args\":{\"kind\":\"modules\",\"column\":0}}", storage);
        check(field_int(data, L"sortColumn") == 0 && !field_bool(data, L"sortDescending"),
              "a different column starts ascending");
        core.modules.pop_back();
    }

    // ---- pointer scan
    {
        json::Value storage;
        send_ok(session,
                L"{\"id\":47,\"cmd\":\"ptr.scan\",\"args\":{\"target\":\"0x100000\",\"levels\":3,"
                L"\"maxOffset\":\"1000\",\"alignment\":8,\"staticOnly\":true}}",
                storage);
    }
    (void)settle(session);
    {
        json::Value storage;
        const json::Value* data = send_ok(session, L"{\"id\":48,\"cmd\":\"ptr.refresh\"}", storage);
        const json::Value* rows = data->find(L"rows");
        check(rows && rows->items.size() == 1, "one pointer result is listed");
        check(field_text(&rows->items[0], L"base") == L"0x000000007FF00000",
              "the pointer base is rendered");
        check(field_text(&rows->items[0], L"offsets") == L"0x10, -0x8",
              "signed offsets keep their order and sign");
        check(field_bool(&rows->items[0], L"static"), "a module base is marked static");
        check(contains(field_text(data, L"label"), L"1 total"), "the pointer label counts the results");
        send_ok(session, L"{\"id\":49,\"cmd\":\"ptr.addToList\",\"args\":{\"index\":0}}", storage);
        check(core.records.size() == 1, "a pointer result became a record");
        check(core.records[0].description == L"pointer scan, 2 level(s)",
              "the added record describes the chain");
        check(core.records[0].resolved == 0x7FF00000, "the added record keeps the chain base");
        check(core.records[0].type == CE_TYPE_U32 && core.records[0].byte_length == 4,
              "the added record is a 4-byte value");
    }
    expect_failure(session, L"{\"id\":50,\"cmd\":\"ptr.scan\",\"args\":{\"target\":\"\"}}",
                   CE_INVALID_ARGUMENT, "a pointer scan without a target is refused");
    expect_failure(session,
                   L"{\"id\":51,\"cmd\":\"ptr.scan\",\"args\":{\"target\":\"0x100000\","
                   L"\"maxOffset\":\"200000\"}}",
                   CE_INVALID_ARGUMENT, "a max offset above 0x100000 is refused");
    expect_failure(session, L"{\"id\":52,\"cmd\":\"ptr.addToList\",\"args\":{\"index\":9}}",
                   CE_INVALID_ARGUMENT, "adding a missing pointer row is refused");

    // ---- a modal dialog suspends commands
    session.set_dialog_active(true);
    expect_failure(session, L"{\"id\":53,\"cmd\":\"scan.new\"}", CE_BUSY,
                   "a command during a modal dialog is refused");
    session.set_dialog_active(false);

    session.request_stop();
    check(!session.worker_running(), "stopping leaves no worker behind");
}

// ---- mode B: the real DLL --------------------------------------------------------

template <class T>
T symbol(HMODULE dll, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(dll, name));
}

CeApi loaded_api(HMODULE dll) {
    CeApi api{};
    api.get_api_version_v2 = symbol<uint32_t(CE_CALL*)()>(dll, "CE_GetApiVersionV2");
    api.get_host_pid = symbol<uint32_t(CE_CALL*)()>(dll, "CE_GetHostPid");
    api.get_last_error = symbol<uint32_t(CE_CALL*)(wchar_t*, uint32_t)>(dll, "CE_GetLastError");
    api.request_stop = symbol<int(CE_CALL*)()>(dll, "CE_RequestStop");
    api.get_scan_status_v2 = symbol<int(CE_CALL*)(CeScanStatusV2*)>(dll, "CE_GetScanStatusV2");
    api.first_scan_v2 = symbol<int(CE_CALL*)(const CeScanRequestV2*)>(dll, "CE_FirstScanV2");
    api.next_scan_v2 = symbol<int(CE_CALL*)(const CeScanRequestV2*)>(dll, "CE_NextScanV2");
    api.get_scan_info_v2 = symbol<int(CE_CALL*)(CeScanInfoV2*)>(dll, "CE_GetScanInfoV2");
    api.get_result_v2 = symbol<int(CE_CALL*)(uint64_t, uint64_t, CeResultV2*, void*, uint32_t,
                                             uint32_t*)>(dll, "CE_GetResultV2");
    api.new_scan_v2 = symbol<int(CE_CALL*)()>(dll, "CE_NewScanV2");
    api.undo_scan_v2 = symbol<int(CE_CALL*)()>(dll, "CE_UndoScanV2");
    api.cancel_scan = symbol<void(CE_CALL*)()>(dll, "CE_CancelScan");
    api.read_bytes_v2 = symbol<int(CE_CALL*)(uint64_t, void*, uint32_t)>(dll, "CE_ReadBytesV2");
    api.write_bytes_v2 = symbol<int(CE_CALL*)(uint64_t, const void*, uint32_t)>(dll, "CE_WriteBytesV2");
    api.write_value_v2 = symbol<int(CE_CALL*)(uint64_t, uint32_t, uint32_t, uint32_t, const wchar_t*)>(
        dll, "CE_WriteValueV2");
    api.format_value_v2 = symbol<int(CE_CALL*)(uint32_t, uint32_t, const void*, uint32_t, wchar_t*,
                                               uint32_t, uint32_t*)>(dll, "CE_FormatValueV2");
    api.resolve_address_v2 = symbol<int(CE_CALL*)(const wchar_t*, uint64_t*)>(dll, "CE_ResolveAddressV2");
    api.resolve_pointer_v2 = symbol<int(CE_CALL*)(const CeAddressV2*, uint64_t*)>(dll, "CE_ResolvePointerV2");
    api.upsert_record_v2 = symbol<int(CE_CALL*)(const CeRecordRequestV2*, uint64_t*)>(dll, "CE_UpsertRecordV2");
    api.record_count_v2 = symbol<int(CE_CALL*)(uint64_t*)>(dll, "CE_RecordCountV2");
    api.get_record_v2 = symbol<int(CE_CALL*)(uint64_t, CeRecordInfoV2*, wchar_t*, uint32_t, uint32_t*)>(
        dll, "CE_GetRecordV2");
    api.remove_record_v2 = symbol<int(CE_CALL*)(uint64_t)>(dll, "CE_RemoveRecordV2");
    api.write_record_v2 = symbol<int(CE_CALL*)(uint64_t, const wchar_t*)>(dll, "CE_WriteRecordV2");
    api.freeze_record_v2 = symbol<int(CE_CALL*)(uint64_t, int)>(dll, "CE_FreezeRecordV2");
    api.save_table_v2 = symbol<int(CE_CALL*)(const wchar_t*)>(dll, "CE_SaveTableV2");
    api.load_table_v2 = symbol<int(CE_CALL*)(const wchar_t*)>(dll, "CE_LoadTableV2");
    api.get_regions_v2 = symbol<int(CE_CALL*)(CeRegionInfoV2*, uint32_t, uint32_t*)>(dll, "CE_GetRegionsV2");
    api.get_modules_v2 = symbol<int(CE_CALL*)(CeModuleInfoV2*, uint32_t, uint32_t*)>(dll, "CE_GetModulesV2");
    api.get_threads_v2 = symbol<int(CE_CALL*)(CeThreadInfoV2*, uint32_t, uint32_t*)>(dll, "CE_GetThreadsV2");
    api.unload_module_v2 = symbol<int(CE_CALL*)(uint64_t)>(dll, "CE_UnloadModuleV2");
    api.pointer_scan_v2 = symbol<int(CE_CALL*)(const CePointerScanRequestV2*)>(dll, "CE_PointerScanV2");
    api.cancel_pointer_scan_v2 = symbol<int(CE_CALL*)()>(dll, "CE_CancelPointerScanV2");
    api.get_pointer_scan_status_v2 = symbol<int(CE_CALL*)(CePointerScanStatusV2*)>(
        dll, "CE_GetPointerScanStatusV2");
    api.get_pointer_scan_result_v2 = symbol<int(CE_CALL*)(uint64_t, uint64_t, CePointerScanResultV2*)>(
        dll, "CE_GetPointerScanResultV2");
    return api;
}

bool api_complete(const CeApi& api) {
    return api.get_api_version_v2 && api.get_host_pid && api.get_last_error && api.request_stop &&
           api.get_scan_status_v2 && api.first_scan_v2 && api.next_scan_v2 && api.get_scan_info_v2 &&
           api.get_result_v2 && api.new_scan_v2 && api.undo_scan_v2 && api.cancel_scan &&
           api.read_bytes_v2 && api.write_bytes_v2 && api.write_value_v2 && api.format_value_v2 &&
           api.resolve_address_v2 && api.resolve_pointer_v2 && api.upsert_record_v2 &&
           api.record_count_v2 && api.get_record_v2 && api.remove_record_v2 && api.write_record_v2 &&
           api.freeze_record_v2 && api.save_table_v2 && api.load_table_v2 && api.get_regions_v2 &&
           api.get_modules_v2 && api.get_threads_v2 && api.unload_module_v2 && api.pointer_scan_v2 &&
           api.cancel_pointer_scan_v2 && api.get_pointer_scan_status_v2 &&
           api.get_pointer_scan_result_v2;
}

WCHAR* wide(const char* text) {
    static WCHAR buffer[32768];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

void mode_b(const char* dll_path) {
    HMODULE dll = LoadLibraryW(wide(dll_path));
    check(dll != nullptr, "the DLL loads");
    const CeApi api = loaded_api(dll);
    check(api_complete(api), "every CE_* entry point the bridge needs is exported");

    // The session's own UI thread needs a moment to publish CE_RUNNING before
    // any export will do anything but return CE_NOT_RUNNING.
    for (int i = 0; i < 400 && symbol<uint32_t(CE_CALL*)()>(dll, "CE_GetState")() != CE_RUNNING; ++i)
        Sleep(25);
    check(symbol<uint32_t(CE_CALL*)()>(dll, "CE_GetState")() == CE_RUNNING,
          "the session reaches CE_RUNNING");

    Session session(api);
    {
        json::Value storage;
        const json::Value* data = send_ok(session, L"{\"id\":1,\"cmd\":\"hello\"}", storage);
        const json::Value* host = data->find(L"host");
        check(field_int(host, L"pid") == GetCurrentProcessId(), "hello reports this process");
        check(field_int(host, L"apiVersion") == 2, "the V2 API answers");
    }

    // A scan of a known range: deterministic and confined to this process.
    static volatile uint32_t magic[64];
    for (int i = 0; i < 64; ++i) magic[i] = 0x11111111u;
    magic[10] = 0xCAFEBABEu;
    wchar_t scan_command[512]{};
    std::swprintf(scan_command, std::size(scan_command),
                  L"{\"id\":2,\"cmd\":\"scan.first\",\"args\":{\"type\":2,\"signed\":false,"
                  L"\"hex\":false,\"comparison\":0,\"alignment\":4,\"rounding\":0,"
                  L"\"byteLength\":\"4\",\"begin\":\"%llX\",\"end\":\"%llX\",\"value\":\"3405691582\","
                  L"\"second\":\"\"}}",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&magic[0])),
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&magic[64])));
    {
        json::Value storage;
        send_ok(session, scan_command, storage);
    }
    {
        Event event = settle(session);
        check(!event.present || event.root.find(L"resultRows") != nullptr,
              "the scan completion publishes result rows");
    }
    {
        json::Value storage;
        const json::Value* results =
            send_ok(session, L"{\"id\":3,\"cmd\":\"results.page\",\"args\":{\"start\":\"0\"}}", storage);
        check(field_int(results, L"total") == 1, "the scan found exactly the planted value");
        check(field_bool(results, L"valid"), "the result page is valid");
    }
    {
        // Ask for the page again: the pump above may have published the rows
        // already, and a reply is deterministic where an event is not.
        json::Value storage;
        send_ok(session, L"{\"id\":3,\"cmd\":\"results.page\",\"args\":{\"start\":\"0\"}}", storage);
    }
    {
        // The event has to outlive the search: nodes point into it, so a loop-local
        // Event would leave `list` dangling the moment the iteration ends.
        Event event;
        const json::Value* list = nullptr;
        for (int i = 0; i < 200 && !list; ++i) {
            event = tick(session);
            if (!event.present) { Sleep(5); continue; }
            const json::Value* section = event.root.find(L"resultRows");
            const json::Value* rows = section ? section->find(L"rows") : nullptr;
            if (rows && !rows->items.empty()) list = rows;
        }
        check(list != nullptr, "the planted value is published as a row");
        if (list) {
            check(field_text(&list->items[0], L"address").size() == 18,
                  "the address is rendered as 0x plus 16 digits");
        }
    }

    // Reading the same range through the memory page proves the byte path.
    wchar_t memory_command[512]{};
    std::swprintf(memory_command, std::size(memory_command),
                  L"{\"id\":4,\"cmd\":\"memory.resolve\",\"args\":{\"base\":\"%llX\",\"offsets\":\"\"}}",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&magic[0])));
    {
        json::Value storage;
        const json::Value* data = send_ok(session, memory_command, storage);
        check(field_text(data, L"readable").find(L'0') == std::wstring::npos,
              "the whole page is readable");
        check(field_int(data, L"unreadable") == 0, "no byte is reported unreadable");
        // magic[10] is 40 bytes into the page; its little-endian bytes spell the
        // planted dword at hex offset 80.
        const std::wstring bytes = field_text(data, L"bytes");
        check(bytes.size() == 512, "the page carries 256 bytes as hex");
        check(bytes.substr(80, 8) == L"bebafeca", "byte order matches the host layout");
    }

    // Records: add, select, freeze on/off, remove.
    wchar_t record_command[512]{};
    std::swprintf(record_command, std::size(record_command),
                  L"{\"id\":5,\"cmd\":\"record.add\",\"args\":{\"base\":\"%llX\",\"offsets\":\"\","
                  L"\"type\":2,\"signed\":false,\"byteLength\":\"4\",\"description\":\"bridge test\","
                  L"\"value\":\"1\",\"snapshot\":true}}",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&magic[0])));
    {
        json::Value storage;
        send_ok(session, record_command, storage);
        const json::Value* data =
            send_ok(session, L"{\"id\":6,\"cmd\":\"record.select\",\"args\":{\"index\":0}}", storage);
        check(field_text(data, L"description") == L"bridge test", "the record round-trips");
        send_ok(session, L"{\"id\":7,\"cmd\":\"record.freeze\",\"args\":{\"enabled\":true}}", storage);
        send_ok(session, L"{\"id\":8,\"cmd\":\"record.freeze\",\"args\":{\"enabled\":false}}", storage);
        send_ok(session, L"{\"id\":9,\"cmd\":\"record.remove\"}", storage);
    }
    {
        json::Value storage;
        const json::Value* records = send_ok(session, L"{\"id\":10,\"cmd\":\"record.refresh\"}", storage);
        check(field_int(records, L"total") == 0, "the record is gone from the core");
    }

    // The three process views, against the real host.
    {
        json::Value storage;
        send_ok(session, L"{\"id\":11,\"cmd\":\"view.load\",\"args\":{\"kind\":\"regions\"}}", storage);
        send_ok(session, L"{\"id\":12,\"cmd\":\"view.load\",\"args\":{\"kind\":\"modules\"}}", storage);
        send_ok(session, L"{\"id\":13,\"cmd\":\"view.load\",\"args\":{\"kind\":\"threads\"}}", storage);
        Event event = settle(session);
        check(event.present, "the views are published for the real host");
        const json::Value* views = event.root.find(L"views");
        check(views && views->find(L"regions")->find(L"loaded")->as_bool(), "regions loaded");
        check(views && views->find(L"modules")->find(L"total")->as_u64() > 0,
              "the host has at least one module");
        check(views && views->find(L"threads")->find(L"total")->as_u64() > 0,
              "the host has at least one thread");
        // Sorting the real host's regions by size: the reply is the re-ordered
        // page, and the column and direction come back for the header.
        const json::Value* sorted = send_ok(
            session, L"{\"id\":15,\"cmd\":\"view.sort\",\"args\":{\"kind\":\"regions\",\"column\":1}}", storage);
        check(sorted && field_int(sorted, L"sortColumn") == 1 && !field_bool(sorted, L"sortDescending"),
              "the real view sorts and reports its column");
        const json::Value* rows = sorted ? sorted->find(L"rows") : nullptr;
        check(rows && rows->items.size() > 0, "the sorted page has rows");
    }

    // The unload rails, against the real core. Every one of these is refused, so
    // nothing in this process is actually unmapped by the test.
    check(api.unload_module_v2(0) == CE_INVALID_ARGUMENT, "a zero module base is refused");
    check(api.unload_module_v2(1) == CE_INVALID_ARGUMENT, "an address that is not a module is refused");
    check(api.unload_module_v2(reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr))) == CE_UNSUPPORTED,
          "the host executable is refused");
    check(api.unload_module_v2(reinterpret_cast<uint64_t>(dll)) == CE_UNSUPPORTED,
          "the running DLL's own module is refused");

    // A pointer scan over the planted chain, then a CT save.
    wchar_t pointer_command[512]{};
    std::swprintf(pointer_command, std::size(pointer_command),
                  L"{\"id\":14,\"cmd\":\"ptr.scan\",\"args\":{\"target\":\"%llX\",\"levels\":1,"
                  L"\"maxOffset\":\"100\",\"alignment\":\"4\",\"staticOnly\":false}}",
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&magic[0])));
    {
        json::Value storage;
        send_ok(session, pointer_command, storage);
    }
    (void)settle(session);
    {
        json::Value storage;
        const json::Value* data = send_ok(session, L"{\"id\":15,\"cmd\":\"ptr.refresh\"}", storage);
        check(data->find(L"rows") != nullptr, "the pointer page answers");
    }
    {
        WCHAR temp_path[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp_path);
        std::wstring path = std::wstring(temp_path) + L"bridge_tests.CT";
        std::wstring command = L"{\"id\":16,\"cmd\":\"record.saveCT\",\"args\":{\"path\":\"";
        for (const wchar_t c : path) {
            if (c == L'\\') command += L"\\\\";
            else command += c;
        }
        command += L"\"}}";
        json::Value storage;
        send_ok(session, command.c_str(), storage);
        check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES,
              "saving a CT writes the file");
        DeleteFileW(path.c_str());
    }

    session.request_stop();
    check(symbol<int(CE_CALL*)(uint32_t)>(dll, "CE_WaitStopped")(10000) == CE_OK,
          "the session stops within the timeout");
    check(FreeLibrary(dll) != 0, "the DLL unloads");
}

} // namespace

int main(int argc, char** argv) {
    // Progress has to survive a crash in mode B, so nothing here is buffered.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    mode_a();
    std::printf("MODE_A_PASS (%d checks)\n", checks);

    if (argc < 2) {
        std::printf("SKIP mode B: no DLL path given\n");
        std::printf("BRIDGE_TESTS_PASS (%d checks)\n", checks);
        return 0;
    }
    mode_b(argv[1]);
    std::printf("MODE_B_PASS (%d checks)\n", checks);
    std::printf("BRIDGE_TESTS_PASS (%d checks)\n", checks);
    return 0;
}
