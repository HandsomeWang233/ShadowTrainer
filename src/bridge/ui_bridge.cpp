#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "ui_bridge.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cwctype>
#include <limits>

#include "json_min.inc"

// The four view structs are the ABI the browser's column decoders are written
// against; a silent layout change would misread every row rather than fail.
static_assert(sizeof(CeScanStatusV2) == 80, "Frozen scan status ABI");
static_assert(sizeof(CeRegionInfoV2) == 48, "Frozen region view ABI");
static_assert(sizeof(CeModuleInfoV2) == 552, "Frozen module view ABI");
static_assert(sizeof(CeThreadInfoV2) == 168, "Frozen thread view ABI");

namespace ce::ui {
namespace {

constexpr uint64_t page_size = 512;
constexpr uint32_t current_value_limit = 256;
constexpr size_t live_row_budget = 16;
constexpr size_t live_byte_budget = 64 * 1024;
constexpr uint64_t live_poll_interval_ms = 500;
constexpr uint64_t live_tick_budget_ms = 12;
constexpr size_t hex_undo_limit = 256;
constexpr size_t memory_history_limit = 64;
constexpr uint32_t memory_page_bytes = 256;

std::wstring trim(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) return {};
    const auto last = text.find_last_not_of(L" \t\r\n");
    return std::wstring(text.substr(first, last - first + 1));
}

// Hex with an 0x prefix and no padding, for sizes.
std::wstring hex_text(uint64_t value) {
    wchar_t text[32]{};
    std::swprintf(text, std::size(text), L"0x%llX", static_cast<unsigned long long>(value));
    return text;
}

std::wstring address_text(uint64_t address) {
    wchar_t text[32]{};
    std::swprintf(text, std::size(text), L"0x%016llX", static_cast<unsigned long long>(address));
    return text;
}

const wchar_t* status_name(int code) noexcept {
    switch (code) {
    case CE_OK: return L"OK";
    case CE_INVALID_ARGUMENT: return L"Invalid argument";
    case CE_NOT_RUNNING: return L"Session is not running";
    case CE_BUSY: return L"Busy";
    case CE_CANCELLED: return L"Cancelled";
    case CE_IO_ERROR: return L"File I/O error";
    case CE_ACCESS_ERROR: return L"Memory access error";
    case CE_UNSUPPORTED: return L"Unsupported";
    default: return L"Internal error";
    }
}

// Same rule as the old UI: leading/trailing blanks tolerated, sign rejected,
// optional 0x prefix in base 16.
bool parse_unsigned(std::wstring_view input, unsigned base, uint64_t& value) {
    const std::wstring text = trim(input);
    if (text.empty() || text[0] == L'-' || text[0] == L'+') return false;
    size_t first = 0;
    if (base == 16 && text.size() > 2 && text[0] == L'0' && (text[1] == L'x' || text[1] == L'X')) first = 2;
    for (size_t i = first; i < text.size(); ++i) {
        if (base == 10 ? (text[i] < L'0' || text[i] > L'9') : !std::iswxdigit(text[i])) return false;
    }
    if (first == text.size()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    value = std::wcstoull(text.c_str(), &end, static_cast<int>(base));
    return errno != ERANGE && end != text.c_str() && *end == L'\0';
}

bool parse_text(std::wstring_view input, uint64_t& value) {
    const std::wstring text = trim(input);
    if (text.empty() || text[0] == L'-' || text[0] == L'+') return false;
    for (const wchar_t c : text) if (c < L'0' || c > L'9') return false;
    wchar_t* end = nullptr;
    errno = 0;
    value = std::wcstoull(text.c_str(), &end, 10);
    return errno != ERANGE && end != text.c_str() && *end == L'\0';
}

constexpr const wchar_t* type_names[] = {L"Byte", L"2 Bytes", L"4 Bytes", L"8 Bytes", L"Float",
                                         L"Double", L"UTF-8", L"UTF-16", L"Array of bytes"};

uint32_t fixed_width(uint32_t type) noexcept {
    constexpr uint32_t widths[] = {1, 2, 4, 8, 4, 8, 0, 0, 0};
    return type <= CE_TYPE_AOB ? widths[type] : 0;
}

// MEM_* decoders for the Regions view, matching what Cheat Engine's
// memory-region window shows for State, Protect, Allocation protect and Type.
const wchar_t* region_state_name(uint32_t state) noexcept {
    switch (state) {
    case MEM_COMMIT: return L"Commit";
    case MEM_FREE: return L"Free";
    case MEM_RESERVE: return L"Reserve";
    default: return L"?";
    }
}

const wchar_t* region_type_name(uint32_t type) noexcept {
    switch (type) {
    case MEM_IMAGE: return L"Image";
    case MEM_MAPPED: return L"Mapped";
    case MEM_PRIVATE: return L"Private";
    case 0: return L"-";
    default: return L"?";
    }
}

std::wstring protect_name(uint32_t protect) {
    if (!protect) return L"-";
    const wchar_t* base = L"?";
    switch (protect & 255) {
    case PAGE_NOACCESS: base = L"No access"; break;
    case PAGE_READONLY: base = L"Read"; break;
    case PAGE_READWRITE: base = L"Read+Write"; break;
    case PAGE_WRITECOPY: base = L"Write copy"; break;
    case PAGE_EXECUTE: base = L"Execute"; break;
    case PAGE_EXECUTE_READ: base = L"Execute+Read"; break;
    case PAGE_EXECUTE_READWRITE: base = L"Execute+Read+Write"; break;
    case PAGE_EXECUTE_WRITECOPY: base = L"Execute+Write copy"; break;
    default: break;
    }
    std::wstring text = base;
    if (protect & PAGE_GUARD) text += L"+Guard";
    if (protect & PAGE_NOCACHE) text += L"+NoCache";
    if (protect & PAGE_WRITECOMBINE) text += L"+WriteCombine";
    return text;
}

std::wstring file_name_of(const wchar_t* path) {
    const std::wstring text = path ? path : L"";
    const auto slash = text.find_last_of(L'\\');
    return slash == std::wstring::npos ? text : text.substr(slash + 1);
}

// `created` is a raw FILETIME; the column header states the UTC epoch so no
// implicit timezone conversion happens here.
std::wstring file_time_text(uint64_t value) {
    if (!value) return L"";
    FILETIME time{static_cast<DWORD>(value & 0xFFFFFFFF), static_cast<DWORD>(value >> 32)};
    SYSTEMTIME utc{};
    if (!FileTimeToSystemTime(&time, &utc)) return L"";
    wchar_t text[64]{};
    std::swprintf(text, std::size(text), L"%04u-%02u-%02u %02u:%02u:%02u", utc.wYear, utc.wMonth,
                  utc.wDay, utc.wHour, utc.wMinute, utc.wSecond);
    return text;
}

std::wstring offsets_text(const CeAddressV2& address) {
    std::wstring text;
    for (uint32_t i = 0; i < address.offset_count && i < CE_V2_MAX_OFFSETS; ++i) {
        const auto offset = address.offsets[i];
        if (i) text += L",";
        if (offset < 0) text += L"-";
        text += hex_text(offset < 0 ? uint64_t{0} - static_cast<uint64_t>(offset)
                                    : static_cast<uint64_t>(offset));
    }
    return text;
}

std::wstring pointer_offset_text(const CePointerScanResultV2& result) {
    std::wstring text;
    for (uint32_t i = 0; i < result.level_count && i < CE_V2_MAX_POINTER_LEVELS; ++i) {
        if (i) text += L", ";
        const int64_t offset = result.offsets[i];
        text += offset < 0 ? L"-" + hex_text(uint64_t{0} - static_cast<uint64_t>(offset))
                           : hex_text(static_cast<uint64_t>(offset));
    }
    return text.empty() ? L"<absolute>" : text;
}

std::wstring bytes_to_hex(const uint8_t* bytes, size_t length) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring text;
    text.reserve(length * 2);
    for (size_t i = 0; i < length; ++i) {
        text += digits[(bytes[i] >> 4) & 0xf];
        text += digits[bytes[i] & 0xf];
    }
    return text;
}

bool hex_to_bytes(std::wstring_view text, std::vector<uint8_t>& out) {
    if (text.size() % 2) return false;
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        uint8_t byte = 0;
        for (int half = 0; half < 2; ++half) {
            const wchar_t c = text[i + static_cast<size_t>(half)];
            uint8_t digit = 0;
            if (c >= L'0' && c <= L'9') digit = static_cast<uint8_t>(c - L'0');
            else if (c >= L'a' && c <= L'f') digit = static_cast<uint8_t>(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') digit = static_cast<uint8_t>(c - L'A' + 10);
            else return false;
            byte = static_cast<uint8_t>((byte << 4) | digit);
        }
        out.push_back(byte);
    }
    return true;
}

const json::Value* member(const json::Value* args, const wchar_t* name) noexcept {
    return args ? args->find(name) : nullptr;
}

std::wstring arg_string(const json::Value* args, const wchar_t* name, std::wstring_view fallback = {}) {
    const auto* value = member(args, name);
    return value ? std::wstring(value->as_string(fallback)) : std::wstring(fallback);
}

uint64_t arg_u64(const json::Value* args, const wchar_t* name, uint64_t fallback = 0) noexcept {
    const auto* value = member(args, name);
    return value ? value->as_u64(fallback) : fallback;
}

uint32_t arg_u32(const json::Value* args, const wchar_t* name, uint32_t fallback = 0) noexcept {
    return static_cast<uint32_t>(arg_u64(args, name, fallback));
}

int32_t arg_i32(const json::Value* args, const wchar_t* name, int32_t fallback = 0) noexcept {
    const auto* value = member(args, name);
    return value ? static_cast<int32_t>(value->as_i64(fallback)) : fallback;
}

bool arg_bool(const json::Value* args, const wchar_t* name, bool fallback = false) noexcept {
    const auto* value = member(args, name);
    return value ? value->as_bool(fallback) : fallback;
}

uint32_t value_flags(const json::Value* args) {
    if (arg_u32(args, L"type") > CE_TYPE_U64) return 0;
    return (arg_bool(args, L"signed") ? CE_VALUE_SIGNED : 0u) |
           (arg_bool(args, L"hex") ? CE_VALUE_HEX : 0u);
}

// The old UI's length_input: fixed widths ignore the field, variable widths are
// validated, and 0 means "infer" only where the core accepts it.
bool length_input(uint32_t type, const std::wstring& text, uint32_t& length, bool required,
                  std::wstring& detail) {
    if (const uint32_t fixed = fixed_width(type)) { length = fixed; return true; }
    uint64_t value = 0;
    if (!parse_text(text, value) || value > CE_V2_MAX_VALUE_BYTES || (required && value == 0) ||
        (type == CE_TYPE_UTF16 && value % 2)) {
        detail = L"Variable width needs a byte count from 1 to 65536 for reads/snapshots; UTF-16 "
                 L"requires an even count. Exact scans/writes may use 0 to infer.";
        return false;
    }
    length = static_cast<uint32_t>(value);
    return true;
}

bool page_address_allowed(uint64_t address, uint64_t host_min, uint64_t host_max) noexcept {
    return address >= host_min && address <= host_max && host_max - address >= memory_page_bytes - 1;
}

} // namespace

#ifdef SHADOWTRAINER_BUILD
// Only the DLL itself can name its own exports: outside it they are dllimport,
// and a test that links ui_bridge.obj must supply its own table (bridge_tests
// builds one from GetProcAddress for exactly that reason).
CeApi production_api() noexcept {
    CeApi api{};
    api.get_api_version_v2 = &CE_GetApiVersionV2;
    api.get_host_pid = &CE_GetHostPid;
    api.get_last_error = &CE_GetLastError;
    api.request_stop = &CE_RequestStop;
    api.get_scan_status_v2 = &CE_GetScanStatusV2;
    api.first_scan_v2 = &CE_FirstScanV2;
    api.next_scan_v2 = &CE_NextScanV2;
    api.get_scan_info_v2 = &CE_GetScanInfoV2;
    api.get_result_v2 = &CE_GetResultV2;
    api.new_scan_v2 = &CE_NewScanV2;
    api.undo_scan_v2 = &CE_UndoScanV2;
    api.cancel_scan = &CE_CancelScan;
    api.read_bytes_v2 = &CE_ReadBytesV2;
    api.write_bytes_v2 = &CE_WriteBytesV2;
    api.write_value_v2 = &CE_WriteValueV2;
    api.format_value_v2 = &CE_FormatValueV2;
    api.resolve_address_v2 = &CE_ResolveAddressV2;
    api.resolve_pointer_v2 = &CE_ResolvePointerV2;
    api.upsert_record_v2 = &CE_UpsertRecordV2;
    api.record_count_v2 = &CE_RecordCountV2;
    api.get_record_v2 = &CE_GetRecordV2;
    api.remove_record_v2 = &CE_RemoveRecordV2;
    api.write_record_v2 = &CE_WriteRecordV2;
    api.freeze_record_v2 = &CE_FreezeRecordV2;
    api.save_table_v2 = &CE_SaveTableV2;
    api.load_table_v2 = &CE_LoadTableV2;
    api.get_regions_v2 = &CE_GetRegionsV2;
    api.get_modules_v2 = &CE_GetModulesV2;
    api.get_threads_v2 = &CE_GetThreadsV2;
    api.unload_module_v2 = &CE_UnloadModuleV2;
    api.pointer_scan_v2 = &CE_PointerScanV2;
    api.cancel_pointer_scan_v2 = &CE_CancelPointerScanV2;
    api.get_pointer_scan_status_v2 = &CE_GetPointerScanStatusV2;
    api.get_pointer_scan_result_v2 = &CE_GetPointerScanResultV2;
    return api;
}
#endif // SHADOWTRAINER_BUILD

Session::Session(const CeApi& api, Wake wake) : api_(api), wake_(std::move(wake)) {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    host_min_ = reinterpret_cast<uint64_t>(info.lpMinimumApplicationAddress);
    host_max_ = reinterpret_cast<uint64_t>(info.lpMaximumApplicationAddress);
    status_ = L"Ready. Values belong to this host process; scan results are snapshots.";
}

Session::~Session() {
    request_stop();
}

void Session::set_status(std::wstring text) {
    status_ = std::move(text);
}

std::wstring Session::diagnostic_of(int code) const {
    if (code == CE_OK) return {};
    wchar_t buffer[1024]{};
    api_.get_last_error(buffer, static_cast<uint32_t>(std::size(buffer)));
    return buffer;
}

bool Session::scan_busy() const noexcept {
    return job_ == Job::value_scan || scan_status_busy_ ||
           (scan_status_valid_ && scan_status_.active != 0);
}

// ---- envelope -------------------------------------------------------------------

std::wstring Session::reply(long long id, const std::wstring& data) {
    json::Writer writer;
    writer.object()
        .key(L"kind").string(L"reply")
        .key(L"id").integer(id)
        .key(L"ok").boolean(true)
        .key(L"data");
    if (data.empty()) writer.null();
    else writer.raw(data);
    writer.end();
    return writer.take();
}

std::wstring Session::failure(long long id, int code, const std::wstring& detail) {
    std::wstring text;
    if (code != CE_OK) text = std::wstring(L"Request failed: ") + status_name(code) + L" (" +
                              std::to_wstring(code) + L")";
    if (code != CE_OK && !detail.empty()) text += L" - " + detail;
    if (code != CE_OK) set_status(text);
    json::Writer writer;
    writer.object()
        .key(L"kind").string(L"reply")
        .key(L"id").integer(id)
        .key(L"ok").boolean(false)
        .key(L"error").object()
            .key(L"code").integer(code)
            .key(L"name").string(status_name(code))
            .key(L"detail").string(detail)
        .end()
        .key(L"status").string(status_)
        .end();
    return writer.take();
}

std::wstring Session::dispatch(const std::wstring& command) {
    json::Value root;
    if (!json::parse(command, root) || root.kind != json::Value::Kind::Object) {
        // No id to echo: the browser matches replies by id, and a malformed
        // envelope has none, so id 0 is the documented "unattributable" reply.
        return failure(0, CE_INVALID_ARGUMENT, L"Command is not a JSON object.");
    }
    const auto* id_value = root.find(L"id");
    const long long id = id_value ? static_cast<long long>(id_value->as_i64(0)) : 0;
    const auto* name = root.find(L"cmd");
    if (!name || name->kind != json::Value::Kind::String)
        return failure(id, CE_INVALID_ARGUMENT, L"Command has no name.");
    return handle(id, name->text, root.find(L"args"));
}

std::wstring Session::handle(long long id, const std::wstring& name, const json::Value* args) {
    if (name == L"hello") {
        return reply(id, full_model());
    }
    Answer answer;
    if (name == L"session.stop") answer = cmd_session(name, args);
    else if (name == L"scan.first" || name == L"scan.next" || name == L"scan.new" ||
             name == L"scan.undo" || name == L"scan.cancel") answer = cmd_scan(name, args);
    else if (name.compare(0, 8, L"results.") == 0) answer = cmd_results(name, args);
    else if (name.compare(0, 7, L"record.") == 0) answer = cmd_record(name, args);
    else if (name.compare(0, 7, L"memory.") == 0) answer = cmd_memory(name, args);
    else if (name.compare(0, 5, L"view.") == 0) answer = cmd_view(name, args);
    else if (name.compare(0, 4, L"ptr.") == 0) answer = cmd_pointer(name, args);
    else if (name.compare(0, 7, L"window.") == 0) answer = cmd_window(name, args);
    else return failure(id, CE_INVALID_ARGUMENT, L"Unknown command: " + name);
    if (answer.code != CE_OK) return failure(id, answer.code, answer.detail);
    return reply(id, answer.data);
}

// ---- the 100 ms tick ------------------------------------------------------------

std::wstring Session::pump(bool visible, bool minimized) {
    if (stopping_) return event();
    poll_status();
    if (cancel_requested_ && (job_ != Job::none || (scan_status_valid_ && scan_status_.active)))
        api_.cancel_scan();
    if (job_ == Job::pointer_scan || pointer_status_valid_) poll_pointer_status();
    finish_worker();
    if (scan_busy() || dialog_active_) return event();
    if (deferred_refresh_) {
        deferred_refresh_ = false;
        if (refresh_results(false)) set_status(L"Refresh scan results: OK (0)");
    }
    if (deferred_record_refresh_) {
        deferred_record_refresh_ = false;
        refresh_records();
    }
    if (deferred_view_refresh_) {
        deferred_view_refresh_ = false;
        for (int kind = 0; kind < 3; ++kind) {
            View* target = view(kind);
            if (target && target->refresh_requested) {
                target->refresh_requested = false;
                load_view(kind, false);
            }
        }
    }
    if (visible && !minimized) {
        load_result_rows(true);
        poll_record_values(visible, minimized);
    }
    return event();
}

std::wstring Session::request_stop() {
    if (!stopping_) {
        stopping_ = true;
        cancel_requested_ = true;
        api_.cancel_scan();
        // Ask the runtime to stop too. This is the same private message the
        // public CE_RequestStop posts, and it is idempotent, so a host that
        // already began stopping just sees it twice.
        api_.request_stop();
        set_status(L"Stopping this DLL session: cancelling scan and joining worker. The host stays "
                   L"running.");
    }
    if (worker_.joinable()) {
        // A cancellation may precede actual scan entry, so repeat until exit.
        while (!worker_done_.load()) {
            api_.cancel_scan();
            Sleep(10);
        }
        worker_.join();
    }
    job_ = Job::none;
    return status_;
}

void Session::poll_status() {
    CeScanStatusV2 latest{};
    latest.size = sizeof(latest);
    latest.version = CE_V2_VERSION;
    const int result = api_.get_scan_status_v2(&latest);
    if (result != CE_OK) {
        // CE_BUSY is a nonblocking snapshot miss, not a new idle/error state:
        // retain the last good telemetry and touch no data API this tick.
        scan_status_busy_ = true;
        return;
    }
    scan_status_ = latest;
    scan_status_valid_ = true;
    scan_status_busy_ = false;
    if (job_ != Job::value_scan && !latest.active) cancel_requested_ = false;
}

void Session::poll_pointer_status() {
    CePointerScanStatusV2 latest{};
    latest.size = sizeof(latest);
    latest.version = CE_V2_VERSION;
    const int result = api_.get_pointer_scan_status_v2(&latest);
    if (result != CE_OK) {
        pointer_status_busy_ = true;
        return;
    }
    pointer_status_busy_ = false;
    pointer_status_valid_ = true;
    pointer_status_ = latest;
}

void Session::cancel() {
    if (stopping_) return;
    cancel_requested_ = true;
    // Both jobs share the cancellation epoch, so one call reaches whichever runs.
    api_.cancel_scan();
    set_status(L"Cancellation requested; waiting for scan exit. Committed results are retained.");
}

void Session::finish_worker() {
    if (job_ == Job::none || !worker_done_.load()) return;
    if (worker_.joinable()) worker_.join();
    const Job finished = job_;
    job_ = Job::none;
    cancel_requested_ = false;
    if (stopping_) return;
    if (finished == Job::pointer_scan) {
        poll_pointer_status();
        // The published generation is what the rows are read through, so the
        // page is rebuilt here rather than left showing the previous run.
        refresh_pointer_results();
        set_status(completion_operation_ + L": " + status_name(completion_status_) + L" (" +
                   std::to_wstring(completion_status_) + L")" +
                   (completion_status_ != CE_OK && !completion_detail_.empty()
                        ? L" - " + completion_detail_
                        : L""));
        return;
    }
    poll_status();
    refresh_results(false);
    set_status(completion_operation_ + L": " + status_name(completion_status_) + L" (" +
               std::to_wstring(completion_status_) + L")" +
               (completion_status_ != CE_OK && !completion_detail_.empty()
                    ? L" - " + completion_detail_
                    : L""));
}

#include "ui_bridge_sections.inc"
#include "ui_bridge_commands.inc"

} // namespace ce::ui
