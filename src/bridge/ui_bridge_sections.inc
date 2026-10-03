// Section serialization for ui_bridge.cpp. Text-included at the bottom of that
// file, so it is already inside namespace ce::ui; it must not open one.
//
// Every section is cached as its own serialized fragment, so an idle tick
// compares strings and pushes nothing. Sections are the unit the browser merges
// by, which is why the event carries a `changed` list rather than a diff.

void Session::remember(std::wstring& cache, std::wstring value, bool& changed, std::wstring& out) {
    if (value == cache) return;
    cache = std::move(value);
    out = cache;
    changed = true;
}

namespace {

const wchar_t* phase_name(uint32_t phase) noexcept {
    switch (phase) {
    case CE_SCAN_IDLE: return L"Idle";
    case CE_SCAN_ENUMERATING: return L"Enumerating readable regions (total unknown)";
    case CE_SCAN_READING: return L"Reading";
    case CE_SCAN_WRITING: return L"Writing temporary results";
    case CE_SCAN_COMMITTING: return L"Committing results";
    case CE_SCAN_COMPLETED: return L"Completed (committed)";
    case CE_SCAN_CANCELLED: return L"Cancelled (previous results retained)";
    case CE_SCAN_FAILED: return L"Failed (previous results retained)";
    default: return L"Unknown phase";
    }
}

// The pointer page uses its own lowercase vocabulary so the two jobs never read
// alike on screen.
const wchar_t* pointer_phase_name(uint32_t phase) noexcept {
    switch (phase) {
    case CE_SCAN_ENUMERATING: return L"enumerating";
    case CE_SCAN_READING: return L"reading";
    case CE_SCAN_WRITING: return L"walking";
    case CE_SCAN_COMMITTING: return L"committing";
    case CE_SCAN_COMPLETED: return L"completed";
    case CE_SCAN_CANCELLED: return L"cancelled";
    case CE_SCAN_FAILED: return L"failed";
    default: return L"idle";
    }
}

std::wstring scan_label(const CeScanStatusV2& status) {
    std::wstring label = std::wstring(L"Scan: ") + phase_name(status.phase);
    const bool counted = status.unit == CE_SCAN_UNIT_BYTES || status.unit == CE_SCAN_UNIT_CANDIDATES;
    if (counted) {
        label += L" | " + std::to_wstring(status.processed);
        if (status.total_known) label += L" / " + std::to_wstring(status.total);
        label += status.unit == CE_SCAN_UNIT_BYTES ? L" bytes" : L" candidates";
    }
    if (status.cancel_requested) label += L" | cancellation requested";
    label += L" | Op " + std::to_wstring(status.operation_id) + L", generation " +
             std::to_wstring(status.generation);
    if (status.phase == CE_SCAN_FAILED)
        label += std::wstring(L" | ") + status_name(static_cast<int>(status.last_status));
    return label;
}

// Colour is never the only signal: the label always carries the phase and the
// exact work counts, and reading 100% of the input is not commit completion.
void scan_progress(const CeScanStatusV2& status, json::Writer& writer) {
    const bool counted = status.unit == CE_SCAN_UNIT_BYTES || status.unit == CE_SCAN_UNIT_CANDIDATES;
    const bool determinate = counted && status.total_known && status.total != 0 &&
                             status.phase == CE_SCAN_READING;
    const wchar_t* mode = determinate ? L"determinate"
                                      : (status.active ? L"marquee"
                                                       : (status.phase == CE_SCAN_COMPLETED ? L"done"
                                                                                            : L"idle"));
    long long position = status.phase == CE_SCAN_COMPLETED ? 1000 : 0;
    if (determinate) {
        const double ratio = static_cast<double>(status.processed) / static_cast<double>(status.total);
        position = static_cast<long long>((std::min)(990.0, 1000.0 * ratio));
    }
    writer.object().key(L"mode").string(mode).key(L"value").integer(position).end();
}

std::wstring pointer_label(const CePointerScanStatusV2& status) {
    if (!status.size) return L"Pointer scan: idle | No results";
    std::wstring line = std::wstring(L"Pointer scan: ") + pointer_phase_name(status.phase);
    if (status.active) {
        line += L" | " + std::to_wstring(status.processed) + L" / " + std::to_wstring(status.total) +
                L" slots";
    } else if (status.phase == CE_SCAN_COMPLETED) {
        line += L" | " + std::to_wstring(status.total) + L" paths from " +
                std::to_wstring(status.processed) + L" slots";
        // Truncation is a boundary, not a failure: the listed results stay valid.
        if (status.truncated) line += L" | truncated (caps stopped the walk early)";
    }
    if (status.cancel_requested) line += L" | cancellation requested";
    return line;
}

const wchar_t* job_name(int job) noexcept {
    switch (job) {
    case 1: return L"value";
    case 2: return L"pointer";
    default: return L"none";
    }
}

} // namespace

bool Session::section_scan(std::wstring& out) {
    json::Writer writer;
    writer.object();
    writer.key(L"valid").boolean(scan_status_valid_);
    writer.key(L"busy").boolean(scan_status_busy_);
    writer.key(L"active").boolean(scan_status_valid_ && scan_status_.active != 0);
    writer.key(L"phase").integer(static_cast<long long>(scan_status_.phase));
    writer.key(L"phaseName").string(scan_status_valid_ ? phase_name(scan_status_.phase) : L"Idle");
    writer.key(L"cancelRequested").boolean(scan_status_.cancel_requested != 0);
    writer.key(L"hasScan").boolean(have_scan_);
    writer.key(L"canUndo").boolean(scan_status_valid_ && scan_status_.can_undo != 0);
    writer.key(L"unit").integer(static_cast<long long>(scan_status_.unit));
    writer.key(L"totalKnown").boolean(scan_status_.total_known != 0);
    writer.key(L"processed").u64(scan_status_.processed);
    writer.key(L"total").u64(scan_status_.total);
    writer.key(L"operationId").u64(scan_status_.operation_id);
    writer.key(L"generation").u64(scan_status_.generation);
    writer.key(L"lastStatus").integer(static_cast<long long>(scan_status_.last_status));
    writer.key(L"label").string(scan_status_valid_ ? scan_label(scan_status_)
                                                   : L"Scan: idle | No committed scan");
    writer.key(L"progress");
    scan_progress(scan_status_, writer);
    writer.end();
    bool changed = false;
    remember(scan_cache_, writer.take(), changed, out);
    return changed;
}

bool Session::section_pointer(std::wstring& out) {
    json::Writer writer;
    writer.object();
    writer.key(L"valid").boolean(pointer_status_valid_);
    writer.key(L"busy").boolean(pointer_status_busy_);
    writer.key(L"active").boolean(pointer_status_valid_ && pointer_status_.active != 0);
    writer.key(L"phase").integer(static_cast<long long>(pointer_status_.phase));
    writer.key(L"truncated").boolean(pointer_status_valid_ && pointer_status_.truncated != 0);
    writer.key(L"cancelRequested").boolean(pointer_status_valid_ && pointer_status_.cancel_requested != 0);
    writer.key(L"processed").u64(pointer_status_.processed);
    writer.key(L"total").u64(pointer_status_.total);
    writer.key(L"generation").u64(pointer_status_.generation);
    writer.key(L"label").string(pointer_status_valid_ ? pointer_label(pointer_status_)
                                                      : L"Pointer scan: idle | No results");
    writer.key(L"progress").object()
        .key(L"mode").string(pointer_status_valid_ && pointer_status_.active ? L"marquee"
                             : (pointer_status_valid_ && pointer_status_.phase == CE_SCAN_COMPLETED
                                    ? L"done" : L"idle"))
        .key(L"value").integer(pointer_status_valid_ && !pointer_status_.active &&
                                       pointer_status_.phase == CE_SCAN_COMPLETED ? 1000 : 0)
        .end();
    writer.end();
    bool changed = false;
    remember(pointer_cache_, writer.take(), changed, out);
    return changed;
}

// The section's JSON, always. Commands that change a list reply with this so the
// browser can paint the new page without waiting for the next tick; the cached
// section builder below compares against the same text.
std::wstring Session::results_json() {
    const uint64_t shown = results_valid_ ? results_.size() : 0;
    std::wstring label;
    if (!results_valid_) label = L"Results: 0 total | 512/page";
    else {
        label = L"Results: " + std::to_wstring(results_total_) + L" total | " +
                (shown ? std::to_wstring(results_start_ + 1) + L" - " +
                             std::to_wstring(results_start_ + shown)
                       : L"0") +
                L" | 512/page";
    }
    json::Writer writer;
    writer.object()
        .key(L"valid").boolean(results_valid_)
        .key(L"start").u64(results_start_)
        .key(L"total").u64(results_total_)
        .key(L"count").u64(shown)
        .key(L"pending").boolean(results_next_ < shown)
        .key(L"generation").u64(scan_info_.generation)
        .key(L"type").integer(static_cast<long long>(scan_info_.type))
        .key(L"flags").integer(static_cast<long long>(scan_info_.flags))
        .key(L"byteLength").integer(static_cast<long long>(scan_info_.byte_length))
        .key(L"alignment").integer(static_cast<long long>(scan_info_.alignment))
        .key(L"label").string(label)
        .end();
    return writer.take();
}

bool Session::section_results(std::wstring& out) {
    bool changed = false;
    remember(results_cache_, results_json(), changed, out);
    return changed;
}

bool Session::section_result_rows(std::wstring& out) {
    if (!results_dirty_ && !results_reset_) return false;
    const size_t from = results_reset_ ? 0 : results_dirty_from_;
    json::Writer writer;
    writer.object().key(L"reset").boolean(results_reset_).key(L"from").integer(
        static_cast<long long>(from));
    writer.key(L"rows").array();
    for (size_t i = from; i < results_.size(); ++i) {
        const auto& row = results_[i];
        if (!row.loaded) continue;
        writer.object()
            .key(L"i").integer(static_cast<long long>(i))
            .key(L"number").u64(results_start_ + i + 1)
            .key(L"address").string(address_text(row.address))
            .key(L"value").string(row.value)
            .end();
    }
    writer.end();
    writer.end();
    out = writer.take();
    results_dirty_ = false;
    results_reset_ = false;
    results_dirty_from_ = results_.size();
    return true;
}

std::wstring Session::records_json() {
    const uint64_t shown = records_valid_ ? records_.size() : 0;
    std::wstring label;
    if (!records_valid_) label = L"Records: 0";
    else if (records_stale_)
        label = L"Address records changed externally. Refresh to verify IDs; editor draft retained.";
    else {
        label = L"Records: " + std::to_wstring(records_total_) + L" | " +
                (shown ? std::to_wstring(records_start_ + 1) + L" - " +
                             std::to_wstring(records_start_ + shown)
                       : L"0");
    }
    json::Writer writer;
    writer.object()
        .key(L"valid").boolean(records_valid_)
        .key(L"stale").boolean(records_stale_)
        .key(L"start").u64(records_start_)
        .key(L"total").u64(records_total_)
        .key(L"count").u64(shown)
        .key(L"selected").u64(selected_id_)
        .key(L"selectedIndex").integer(selected_index_)
        .key(L"selectedOpaque").boolean(selection_opaque())
        .key(L"sortColumn").integer(records_sort_column_)
        .key(L"sortDescending").boolean(records_sort_descending_)
        .key(L"label").string(label)
        .end();
    return writer.take();
}

bool Session::section_records(std::wstring& out) {
    bool changed = false;
    remember(records_cache_, records_json(), changed, out);
    return changed;
}

bool Session::section_record_rows(std::wstring& out) {
    if (!records_dirty_ && !records_reset_) return false;
    const size_t from = records_reset_ ? 0 : records_dirty_from_;
    json::Writer writer;
    writer.object().key(L"reset").boolean(records_reset_).key(L"from").integer(
        static_cast<long long>(from));
    writer.key(L"rows").array();
    for (size_t i = from; i < records_.size(); ++i) {
        const auto& row = records_[i];
        writer.object()
            .key(L"i").integer(static_cast<long long>(i))
            .key(L"id").u64(row.id)
            .key(L"description").string(row.description)
            .key(L"resolved").string(row.opaque ? L"Unsupported" : address_text(row.resolved))
            .key(L"type").integer(static_cast<long long>(row.type))
            .key(L"typeName").string(row.opaque ? L"CT opaque (not representable)"
                                                : (row.type <= CE_TYPE_AOB ? type_names[row.type]
                                                                           : L"Unknown"))
            .key(L"frozen").boolean(row.frozen != 0)
            .key(L"status").integer(static_cast<long long>(row.last_status))
            .key(L"statusText").string(std::wstring(status_name(static_cast<int>(row.last_status))) +
                                       L" (" + std::to_wstring(row.last_status) + L")")
            .key(L"opaque").boolean(row.opaque)
            .key(L"hasValue").boolean(row.has_value)
            .key(L"value").string(row.value)
            .end();
    }
    writer.end();
    writer.end();
    out = writer.take();
    records_dirty_ = false;
    records_reset_ = false;
    records_dirty_from_ = records_.size();
    return true;
}

bool Session::section_views(std::wstring& out) {
    json::Writer writer;
    writer.object();
    const wchar_t* names[] = {L"regions", L"modules", L"threads"};
    for (int kind = 0; kind < 3; ++kind) writer.key(names[kind]).raw(render_view(kind));
    writer.end();
    bool changed = false;
    remember(views_cache_, writer.take(), changed, out);
    return changed;
}

// The view's own object: the current page's rows plus the label that says which
// slice they are. The snapshot itself stays on this side, so paging never
// re-enumerates the host and rows cannot shift under the reader.
std::wstring Session::render_view(int kind) {
    const View* target = view(kind);
    const wchar_t* names[] = {L"Regions", L"Modules", L"Threads"};
    const uint64_t shown =
        target->loaded ? (std::min)(static_cast<uint64_t>(page_size),
                                    target->total > target->start ? target->total - target->start : 0)
                       : 0;
    const std::wstring label =
        target->loaded
            ? std::wstring(names[kind]) + L": " + std::to_wstring(target->total) + L" total | " +
                  (shown ? std::to_wstring(target->start + 1) + L" - " +
                               std::to_wstring(target->start + shown)
                         : L"0") +
                  L" | 512/page"
            : std::wstring(names[kind]) + L": not loaded";
    json::Writer writer;
    writer.object()
        .key(L"loaded").boolean(target->loaded)
        .key(L"start").u64(target->start)
        .key(L"total").u64(target->total)
        .key(L"count").u64(shown)
        .key(L"sortColumn").integer(target->sort_column)
        .key(L"sortDescending").boolean(target->sort_descending)
        .key(L"label").string(label);
    writer.key(L"rows").array();
    for (uint64_t i = 0; i < shown; ++i) {
        const auto& row = target->rows[static_cast<size_t>(target->start + i)];
        writer.array();
        for (const auto& cell : row.cells) writer.string(cell);
        writer.end();
    }
    writer.end();
    // What each row names, parallel to `rows`, for the commands that act on one.
    // Only the modules view has any: regions and threads have nothing to act on.
    writer.key(L"ids").array();
    for (uint64_t i = 0; i < shown; ++i)
        writer.u64(target->rows[static_cast<size_t>(target->start + i)].id);
    writer.end();
    writer.end();
    return writer.take();
}

bool Session::section_controls(std::wstring& out) {
    const bool scanning = scan_status_valid_ && scan_status_.active != 0;
    const bool can_refresh = !stopping_ && !dialog_active_ && job_ != Job::value_scan && !scanning;
    json::Writer writer;
    writer.object()
        .key(L"idle").boolean(idle())
        .key(L"stopping").boolean(stopping_)
        .key(L"dialog").boolean(dialog_active_)
        .key(L"busy").boolean(scan_busy())
        .key(L"job").string(job_name(static_cast<int>(job_)))
        .key(L"workerRunning").boolean(job_ != Job::none)
        .key(L"scanning").boolean(scanning)
        .key(L"cancelRequested").boolean(cancel_requested_)
        .key(L"haveScan").boolean(have_scan_)
        .key(L"canUndo").boolean(scan_status_valid_ && scan_status_.can_undo != 0)
        .key(L"canRefresh").boolean(can_refresh)
        .key(L"recordsStale").boolean(records_stale_)
        .key(L"selected").u64(selected_id_)
        .key(L"selectedIndex").integer(selected_index_)
        .key(L"selectedOpaque").boolean(selection_opaque())
        .key(L"resultsStart").u64(results_start_)
        .key(L"resultsTotal").u64(results_total_)
        .key(L"recordsStart").u64(records_start_)
        .key(L"recordsTotal").u64(records_total_)
        .key(L"pointerActive").boolean(pointer_status_valid_ && pointer_status_.active != 0)
        .key(L"pointerStart").u64(pointer_status_valid_ ? pointer_status_.total : 0)
        .key(L"hostMin").u64(host_min_)
        .key(L"hostMax").u64(host_max_);
    // Paging bounds the browser cannot derive: it is told the delta to send, not
    // the new offset, so the clamp has to happen here.
    writer.key(L"resultsPrev").boolean(idle() && results_start_ != 0);
    writer.key(L"resultsNext").boolean(idle() && results_total_ > results_start_ &&
                                       results_total_ - results_start_ > page_size);
    writer.key(L"recordsPrev").boolean(idle() && records_start_ != 0);
    writer.key(L"recordsNext").boolean(idle() && records_total_ > records_start_ &&
                                       records_total_ - records_start_ > page_size);
    writer.key(L"ptrPrev").boolean(idle() && job_ != Job::pointer_scan && pointer_start_ != 0);
    writer.key(L"ptrNext").boolean(idle() && job_ != Job::pointer_scan &&
                                   pointer_total_ > pointer_start_ &&
                                   pointer_total_ - pointer_start_ > page_size);
    writer.key(L"ptrAdd").boolean(!stopping_ && !dialog_active_ && !pointer_page_.empty());
    writer.key(L"memoryValid").boolean(memory_valid_);
    writer.key(L"memoryBack").boolean(idle() && memory_history_at_ > 0);
    writer.key(L"memoryForward").boolean(idle() && memory_history_at_ + 1 < memory_history_.size());
    writer.key(L"memoryPrev").boolean(idle() && can_move_memory(false));
    writer.key(L"memoryNext").boolean(idle() && can_move_memory(true));
    writer.key(L"memoryRefresh").boolean(idle() && memory_valid_);
    writer.key(L"hexUndo").boolean(!stopping_ && !dialog_active_ && !hex_undo_.empty());
    writer.key(L"hexFill").boolean(!stopping_ && !dialog_active_ && memory_valid_);
    writer.key(L"views").object();
    for (int kind = 0; kind < 3; ++kind) {
        const View* target = view(kind);
        const wchar_t* names[] = {L"regions", L"modules", L"threads"};
        writer.key(names[kind]).object()
            .key(L"prev").boolean(idle() && target->start != 0)
            .key(L"next").boolean(idle() && target->total > target->start &&
                                  target->total - target->start > page_size)
            .end();
    }
    writer.end();
    writer.end();
    bool changed = false;
    remember(controls_cache_, writer.take(), changed, out);
    return changed;
}

std::wstring Session::event() {
    std::wstring status, scan, pointer, results, result_rows, records, record_rows, views, controls;
    bool changed = false;
    if (status_ != status_cache_) {
        status_cache_ = status_;
        status = status_;
        changed = true;
    }
    if (section_scan(scan)) changed = true;
    if (section_pointer(pointer)) changed = true;
    if (section_results(results)) changed = true;
    if (section_result_rows(result_rows)) changed = true;
    if (section_records(records)) changed = true;
    if (section_record_rows(record_rows)) changed = true;
    if (section_views(views)) changed = true;
    if (section_controls(controls)) changed = true;
    if (!changed) return {};

    json::Writer writer;
    writer.object()
        .key(L"kind").string(L"event")
        .key(L"seq").integer(static_cast<long long>(++event_seq_));
    writer.key(L"changed").array();
    if (!status.empty()) writer.string(L"status");
    if (!scan.empty()) writer.string(L"scan");
    if (!pointer.empty()) writer.string(L"pointer");
    if (!results.empty()) writer.string(L"results");
    if (!result_rows.empty()) writer.string(L"resultRows");
    if (!records.empty()) writer.string(L"records");
    if (!record_rows.empty()) writer.string(L"recordRows");
    if (!views.empty()) writer.string(L"views");
    if (!controls.empty()) writer.string(L"controls");
    writer.end();
    if (!status.empty()) writer.key(L"status").string(status);
    if (!scan.empty()) writer.key(L"scan").raw(scan);
    if (!pointer.empty()) writer.key(L"pointer").raw(pointer);
    if (!results.empty()) writer.key(L"results").raw(results);
    if (!result_rows.empty()) writer.key(L"resultRows").raw(result_rows);
    if (!records.empty()) writer.key(L"records").raw(records);
    if (!record_rows.empty()) writer.key(L"recordRows").raw(record_rows);
    if (!views.empty()) writer.key(L"views").raw(views);
    if (!controls.empty()) writer.key(L"controls").raw(controls);
    writer.end();
    return writer.take();
}

std::wstring Session::full_model() {
    // Caches are cleared so every section reports its current state regardless of
    // what the previous page instance had already been told.
    status_cache_.clear();
    scan_cache_.clear();
    pointer_cache_.clear();
    controls_cache_.clear();
    results_cache_.clear();
    records_cache_.clear();
    views_cache_.clear();
    results_reset_ = true;
    records_reset_ = true;
    std::wstring status, scan, pointer, results, result_rows, records, record_rows, views, controls;
    section_scan(scan);
    section_pointer(pointer);
    section_results(results);
    section_result_rows(result_rows);
    section_records(records);
    section_record_rows(record_rows);
    section_views(views);
    section_controls(controls);
    status_cache_ = status_;
    status = status_;

    json::Writer writer;
    writer.object()
        .key(L"full").boolean(true)
        .key(L"host").object()
            .key(L"pid").u64(api_.get_host_pid())
            .key(L"apiVersion").integer(static_cast<long long>(api_.get_api_version_v2()))
            .key(L"hostMin").u64(host_min_)
            .key(L"hostMax").u64(host_max_)
            .end()
        .key(L"limits").object()
            .key(L"pageSize").integer(static_cast<long long>(page_size))
            .key(L"currentValueLimit").integer(static_cast<long long>(current_value_limit))
            .key(L"maxPointerLevels").integer(static_cast<long long>(CE_V2_MAX_POINTER_LEVELS))
            .key(L"maxOffsets").integer(static_cast<long long>(CE_V2_MAX_OFFSETS))
            .key(L"memoryPageBytes").integer(static_cast<long long>(memory_page_bytes))
            .key(L"maxValueBytes").integer(static_cast<long long>(CE_V2_MAX_VALUE_BYTES))
            .end()
        .key(L"session").object()
            .key(L"stopping").boolean(stopping_)
            .key(L"dialog").boolean(dialog_active_)
            .end()
        .key(L"status").string(status)
        .key(L"scan").raw(scan)
        .key(L"pointer").raw(pointer)
        .key(L"results").raw(results)
        .key(L"resultRows").raw(result_rows)
        .key(L"records").raw(records)
        .key(L"recordRows").raw(record_rows)
        .key(L"views").raw(views)
        .key(L"controls").raw(controls)
        .key(L"memory").object()
            .key(L"valid").boolean(memory_valid_)
            .key(L"base").u64(memory_base_)
            .key(L"page").u64(memory_page_)
            .key(L"label").string(memory_label())
            .end()
        .end();
    return writer.take();
}
