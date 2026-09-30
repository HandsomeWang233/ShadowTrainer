#pragma once
// The browser-free half of the web UI: everything the old Win32 UI used to do
// between "a button was clicked" and "the core was called", with no window, no
// COM and no WebView2 anywhere in sight.
//
// The host (src/ui/webui.cpp) owns a Session, feeds it one JSON command per
// inbound WebView2 message and calls pump() from its 100 ms timer; it never
// interprets a command itself. Tests own a Session too, with either fake CE_*
// implementations or the real DLL behind GetProcAddress -- which is how every
// button action is exercised without a browser.
//
// Threading: dispatch() and pump() run on the UI thread only. Long jobs (value
// scan, pointer scan) run on one shared worker thread, exactly as the old UI
// did, and report back through worker_done_ plus wake().
//
// The wire protocol is documented in docs/WEBUI.md; the short version is
// {"id":N,"cmd":"...","args":{...}} in and {"kind":"reply","id":N,"ok":...} or
// {"kind":"event","changed":[...]} out, with every 64-bit quantity carried as a
// decimal string.
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "ce/api.h"
#include "ce/api_v2.h"
#include "json_min.hpp"

namespace ce::ui {

// Function-pointer table over the exported CE_* API. Production fills it from
// this DLL's own exports (production_api); tests fill it with fakes, or with a
// loaded shadowtrainer.dll, which is what lets the same Session run headless.
struct CeApi {
    uint32_t (CE_CALL *get_api_version_v2)();
    uint32_t (CE_CALL *get_host_pid)();
    uint32_t (CE_CALL *get_last_error)(wchar_t* buffer, uint32_t capacity);
    int (CE_CALL *request_stop)();

    int (CE_CALL *get_scan_status_v2)(CeScanStatusV2* status);
    int (CE_CALL *first_scan_v2)(const CeScanRequestV2* request);
    int (CE_CALL *next_scan_v2)(const CeScanRequestV2* request);
    int (CE_CALL *get_scan_info_v2)(CeScanInfoV2* info);
    int (CE_CALL *get_result_v2)(uint64_t generation, uint64_t index, CeResultV2* info,
                                 void* bytes, uint32_t capacity, uint32_t* required);
    int (CE_CALL *new_scan_v2)();
    int (CE_CALL *undo_scan_v2)();
    // V1 declares this one void; every call site ignores the result.
    void (CE_CALL *cancel_scan)();
    int (CE_CALL *read_bytes_v2)(uint64_t address, void* bytes, uint32_t length);
    int (CE_CALL *write_bytes_v2)(uint64_t address, const void* bytes, uint32_t length);
    int (CE_CALL *write_value_v2)(uint64_t address, uint32_t type, uint32_t flags,
                                  uint32_t byte_length, const wchar_t* value);
    int (CE_CALL *format_value_v2)(uint32_t type, uint32_t flags, const void* bytes, uint32_t length,
                                   wchar_t* text, uint32_t capacity, uint32_t* required);
    int (CE_CALL *resolve_address_v2)(const wchar_t* expression, uint64_t* address);
    int (CE_CALL *resolve_pointer_v2)(const CeAddressV2* address, uint64_t* resolved);

    int (CE_CALL *upsert_record_v2)(const CeRecordRequestV2* record, uint64_t* id);
    int (CE_CALL *record_count_v2)(uint64_t* count);
    int (CE_CALL *get_record_v2)(uint64_t index, CeRecordInfoV2* info, wchar_t* description,
                                 uint32_t capacity, uint32_t* required);
    int (CE_CALL *remove_record_v2)(uint64_t id);
    int (CE_CALL *write_record_v2)(uint64_t id, const wchar_t* value);
    int (CE_CALL *freeze_record_v2)(uint64_t id, int enabled);
    int (CE_CALL *save_table_v2)(const wchar_t* path);
    int (CE_CALL *load_table_v2)(const wchar_t* path);

    int (CE_CALL *get_regions_v2)(CeRegionInfoV2* regions, uint32_t capacity, uint32_t* required);
    int (CE_CALL *get_modules_v2)(CeModuleInfoV2* modules, uint32_t capacity, uint32_t* required);
    int (CE_CALL *get_threads_v2)(CeThreadInfoV2* threads, uint32_t capacity, uint32_t* required);

    int (CE_CALL *pointer_scan_v2)(const CePointerScanRequestV2* request);
    int (CE_CALL *cancel_pointer_scan_v2)();
    int (CE_CALL *get_pointer_scan_status_v2)(CePointerScanStatusV2* status);
    int (CE_CALL *get_pointer_scan_result_v2)(uint64_t generation, uint64_t index,
                                              CePointerScanResultV2* result);
};

// Filled with this DLL's own exports.
CeApi production_api() noexcept;

// One command in, one reply out. `dispatch` never throws and never returns an
// empty string: malformed input produces a well-formed failure reply. `pump`
// returns an event envelope listing only the sections that changed, or an empty
// string when nothing did.
class Session {
public:
    // Called when a worker finishes, so the host can post its wake message and
    // the completion is noticed immediately instead of on the next tick.
    using Wake = std::function<void()>;

    Session(const CeApi& api, Wake wake = {});
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void set_wake(Wake wake) { wake_ = std::move(wake); }
    // While a modal dialog is open the UI thread is inside a nested message
    // loop, so commands are refused and pump() skips core work.
    void set_dialog_active(bool active) noexcept { dialog_active_ = active; }
    bool dialog_active() const noexcept { return dialog_active_; }

    std::wstring dispatch(const std::wstring& command);
    std::wstring pump(bool visible, bool minimized);
    std::wstring request_stop();

    // True while a value scan or pointer scan worker is still running.
    bool worker_running() const noexcept { return job_ != Job::none; }

    // Host callbacks. The window commands and the CT file dialogs are the host's
    // business -- native window state and a native file picker -- so the bridge
    // forwards them instead of pretending it could do them itself. Tests install
    // their own and observe the calls.
    struct Host {
        std::function<void(const std::wstring& command, const json::Value* args)> window;
        // Returns the chosen path, or empty when the user cancelled.
        std::function<std::wstring(bool save)> table_dialog;
    };
    Host& host() noexcept { return host_; }

private:
    enum class Job { none, value_scan, pointer_scan };

    // A handler's answer: the reply payload plus, on failure, the CE status and
    // its diagnostic. Handlers never build the envelope themselves.
    struct Answer {
        int code = CE_OK;
        std::wstring detail;
        std::wstring data;   // valid JSON when code == CE_OK; empty means null
    };

    std::wstring handle(long long id, const std::wstring& name, const json::Value* args);
    // The whole model, caches bypassed. Sent once, in reply to `hello`.
    std::wstring full_model();
    std::wstring reply(long long id, const std::wstring& data);
    std::wstring failure(long long id, int code, const std::wstring& detail);
    std::wstring event();
    std::wstring diagnostic_of(int code) const;

    // Command groups. Each returns its payload, or a failure Answer built from
    // the CE status the core returned.
    Answer cmd_window(const std::wstring& name, const json::Value* args);
    Answer cmd_session(const std::wstring& name, const json::Value* args);
    Answer cmd_scan(const std::wstring& name, const json::Value* args);
    Answer cmd_results(const std::wstring& name, const json::Value* args);
    Answer cmd_record(const std::wstring& name, const json::Value* args);
    Answer cmd_memory(const std::wstring& name, const json::Value* args);
    Answer cmd_view(const std::wstring& name, const json::Value* args);
    Answer cmd_pointer(const std::wstring& name, const json::Value* args);

    Host host_;

    // Sections. Each returns true when the content differs from the cached copy,
    // which is what keeps an idle tick from pushing anything at all.
    bool section_status(std::wstring& out);
    bool section_scan(std::wstring& out);
    bool section_pointer(std::wstring& out);
    bool section_controls(std::wstring& out);
    std::wstring results_json();
    std::wstring records_json();
    bool section_results(std::wstring& out);
    bool section_result_rows(std::wstring& out);
    bool section_records(std::wstring& out);
    bool section_record_rows(std::wstring& out);
    bool section_views(std::wstring& out);
    static void remember(std::wstring& cache, std::wstring value, bool& changed, std::wstring& out);

    void poll_status();
    void poll_pointer_status();
    void finish_worker();
    void refresh_pointer_results();
    void cancel();
    void set_status(std::wstring text);
    bool scan_busy() const noexcept;
    bool idle() const noexcept { return !stopping_ && !scan_busy() && !dialog_active_; }
    bool busy_for_command() const noexcept { return stopping_ || dialog_active_; }
    bool can_move_memory(bool next) const noexcept;
    std::wstring memory_label() const;

    // Row builders. `passive` applies the old UI's budgets: live_row_budget rows
    // and live_byte_budget bytes per tick, stopping after 12 ms once the first
    // screenful is in.
    void load_result_rows(bool passive);
    bool refresh_results(bool passive);
    void refresh_records();
    void poll_record_values(bool visible, bool minimized);
    bool load_view(int kind, bool refresh);
    std::wstring render_view(int kind);
    std::wstring pointer_rows_payload();

    // Rows are defined further down where their caches live; the helpers above
    // them only need the declaration.
    struct RecordRow;
    struct ResultRow;

    // Shared helpers.
    bool resolve(const std::wstring& expression, const std::wstring& offsets, CeAddressV2& address,
                 uint64_t& resolved, int& code);
    bool format_bytes(uint32_t type, uint32_t flags, const void* bytes, uint32_t length,
                      std::wstring& out, int& code);
    std::wstring live_value(const RecordRow& row);
    bool memory_payload(uint64_t address, Answer& answer);
    bool record_at(size_t index, RecordRow& row, std::wstring& description) const;
    bool validate_selection(int& index, Answer& answer) const;
    uint64_t selected_id() const noexcept { return selected_id_; }
    bool selection_opaque() const noexcept;

    // Held by value: a Session outlives the expression that built its table,
    // and a dangling reference here would be a use-after-free on every call.
    CeApi api_;
    Wake wake_;
    std::atomic<bool> worker_done_{false};
    std::thread worker_;
    Job job_ = Job::none;
    std::wstring completion_operation_;
    int completion_status_ = CE_OK;
    std::wstring completion_detail_;
    bool scan_was_first_ = false;
    bool pointer_added_ = false;

    bool cancel_requested_ = false;
    bool dialog_active_ = false;
    bool stopping_ = false;

    std::wstring status_;
    std::wstring status_cache_;
    std::wstring scan_cache_;
    std::wstring pointer_cache_;
    std::wstring controls_cache_;
    std::wstring results_cache_;
    std::wstring records_cache_;
    std::wstring views_cache_;

    CeScanStatusV2 scan_status_{};
    bool scan_status_valid_ = false;
    bool scan_status_busy_ = false;
    CeScanInfoV2 scan_info_{};
    bool have_scan_ = false;

    CePointerScanStatusV2 pointer_status_{};
    bool pointer_status_valid_ = false;
    bool pointer_status_busy_ = false;
    // The pointer page's own copy of the results it is showing; paging never
    // re-reads the core, so rows cannot shift under the user.
    std::vector<CePointerScanResultV2> pointer_page_;
    uint64_t pointer_start_ = 0;
    uint64_t pointer_total_ = 0;
    uint64_t pointer_generation_ = 0;
    bool pointer_page_valid_ = false;

    // Results page. Cells fill in as they load so a passive tick hands the
    // browser only the rows that appeared since the last one.
    struct ResultRow {
        bool loaded = false;
        uint64_t address = 0;
        std::wstring value;
    };
    std::vector<ResultRow> results_;
    uint64_t results_total_ = 0;
    uint64_t results_start_ = 0;
    bool results_valid_ = false;
    bool results_reset_ = false;
    size_t results_next_ = 0;
    size_t results_dirty_from_ = 0;
    bool results_dirty_ = false;
    uint64_t results_selected_ = 0;   // kept across rebuilds, by address
    uint64_t results_focused_ = 0;

    struct RecordRow {
        uint64_t id = 0;
        uint64_t resolved = 0;
        uint32_t type = 0, flags = 0, byte_length = 0, frozen = 0, last_status = 0;
        CeAddressV2 address{};
        std::wstring description;
        std::wstring value;
        bool has_value = false;
        bool opaque = false;
    };
    std::vector<RecordRow> records_;
    uint64_t records_total_ = 0;
    uint64_t records_start_ = 0;
    bool records_valid_ = false;
    bool records_stale_ = false;
    bool records_reset_ = false;
    int64_t selected_index_ = -1;
    uint64_t selected_id_ = 0;
    size_t records_dirty_from_ = 0;
    bool records_dirty_ = false;
    uint64_t last_live_poll_ = 0;   // GetTickCount64 at the last live sample
    size_t live_cursor_ = 0;        // round-robin position inside the visible band
    size_t live_first_ = 0;         // visible band the browser last reported
    size_t live_last_ = 0;

    struct ViewRow {
        std::vector<std::wstring> cells;
    };
    struct View {
        std::vector<ViewRow> rows;
        uint64_t start = 0;
        uint64_t total = 0;
        bool loaded = false;
        bool refresh_requested = false;
        std::wstring label;
    };
    View regions_, modules_, threads_;
    View* view(int kind) noexcept;
    View* view_for_kind(const std::wstring& kind) noexcept;

    // Memory preview. The page itself is the browser's business: the bridge
    // serves bytes and remembers only where the user has been and how to undo.
    uint64_t memory_base_ = 0;
    uint64_t memory_page_ = 0;
    bool memory_valid_ = false;
    struct MemoryStep {
        uint64_t base = 0, page = 0;
    };
    std::vector<MemoryStep> memory_history_;
    size_t memory_history_at_ = 0;
    struct HexUndo {
        uint64_t address = 0;
        std::vector<uint8_t> before, after;
    };
    std::vector<HexUndo> hex_undo_;
    void remember_page();

    uint64_t host_min_ = 0, host_max_ = 0;
    bool deferred_refresh_ = false;
    bool deferred_record_refresh_ = false;
    bool deferred_view_refresh_ = false;
    uint64_t event_seq_ = 0;
    bool hello_seen_ = false;
};

} // namespace ce::ui
