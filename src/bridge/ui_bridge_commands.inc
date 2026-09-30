// Command handlers for ui_bridge.cpp. Text-included at the bottom of that file,
// so it is already inside namespace ce::ui; it must not open one.
//
// One handler per control the old UI had. Everything a handler needs travels in
// `args` -- the bridge keeps no copy of the form, so a page reload cannot leave
// the two sides disagreeing about what is in a field.

namespace {

// The editor payload shared by "activate a scan result" and "select a record":
// what the Address list tab needs to show the user that address.
std::wstring editor_payload(uint64_t base, const std::wstring& offsets, const std::wstring& description,
                            uint32_t type, uint32_t flags, uint32_t byte_length,
                            const std::wstring& value, uint64_t resolved, uint64_t selected,
                            const std::wstring& current, bool opaque = false) {
    json::Writer writer;
    writer.object()
        .key(L"base").string(base ? address_text(base) : L"")
        .key(L"offsets").string(offsets)
        .key(L"description").string(description)
        .key(L"type").integer(static_cast<long long>(type))
        .key(L"flags").integer(static_cast<long long>(flags))
        .key(L"signed").boolean((flags & CE_VALUE_SIGNED) != 0)
        .key(L"hex").boolean((flags & CE_VALUE_HEX) != 0)
        .key(L"byteLength").integer(static_cast<long long>(byte_length))
        .key(L"value").string(value)
        .key(L"resolved").string(address_text(resolved))
        .key(L"selected").u64(selected)
        .key(L"current").string(current)
        .key(L"opaque").boolean(opaque)
        .end();
    return writer.take();
}

} // namespace

Session::View* Session::view(int kind) noexcept {
    switch (kind) {
    case 0: return &regions_;
    case 1: return &modules_;
    case 2: return &threads_;
    default: return nullptr;
    }
}

Session::View* Session::view_for_kind(const std::wstring& kind) noexcept {
    if (kind == L"regions") return &regions_;
    if (kind == L"modules") return &modules_;
    if (kind == L"threads") return &threads_;
    return nullptr;
}

bool Session::selection_opaque() const noexcept {
    if (selected_index_ < 0 || static_cast<size_t>(selected_index_) >= records_.size()) return false;
    return records_[static_cast<size_t>(selected_index_)].opaque;
}

bool Session::record_at(size_t index, RecordRow& row, std::wstring& description) const {
    if (index >= records_.size()) return false;
    row = records_[index];
    description = row.description;
    return true;
}

bool Session::validate_selection(int& index, Answer& answer) const {
    if (records_stale_ || !selected_id_ || selected_index_ < 0 ||
        static_cast<size_t>(selected_index_) >= records_.size() ||
        records_[static_cast<size_t>(selected_index_)].id != selected_id_) {
        answer.code = CE_INVALID_ARGUMENT;
        answer.detail = L"Select an address-list record, or Refresh a changed listing first.";
        return false;
    }
    index = static_cast<int>(selected_index_);
    return true;
}

// ---- shared core helpers ---------------------------------------------------------

bool Session::resolve(const std::wstring& expression, const std::wstring& offsets, CeAddressV2& address,
                      uint64_t& resolved, int& code) {
    code = api_.resolve_address_v2(expression.c_str(), &address.base);
    if (code != CE_OK) return false;
    // Up to 16 comma-separated signed hexadecimal offsets, execution order.
    const std::wstring text = trim(offsets);
    if (!text.empty()) {
        size_t at = 0;
        for (;;) {
            const auto comma = text.find(L',', at);
            std::wstring token =
                trim(std::wstring_view(text).substr(at, comma == std::wstring::npos ? comma : comma - at));
            const bool negative = !token.empty() && token[0] == L'-';
            if (!token.empty() && (token[0] == L'-' || token[0] == L'+')) token.erase(0, 1);
            uint64_t magnitude = 0;
            const uint64_t limit = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
            if (address.offset_count == CE_V2_MAX_OFFSETS || !parse_unsigned(token, 16, magnitude) ||
                magnitude > limit + (negative ? 1u : 0u)) {
                code = CE_INVALID_ARGUMENT;
                return false;
            }
            address.offsets[address.offset_count++] =
                negative ? (magnitude == limit + 1 ? (std::numeric_limits<int64_t>::min)()
                                                   : -static_cast<int64_t>(magnitude))
                         : static_cast<int64_t>(magnitude);
            if (comma == std::wstring::npos) break;
            at = comma + 1;
        }
    }
    code = api_.resolve_pointer_v2(&address, &resolved);
    return code == CE_OK;
}

bool Session::format_bytes(uint32_t type, uint32_t flags, const void* bytes, uint32_t length,
                           std::wstring& out, int& code) {
    uint32_t required = 0;
    code = api_.format_value_v2(type, flags, bytes, length, nullptr, 0, &required);
    if (code != CE_OK) return false;
    if (!required || required > CE_V2_MAX_VALUE_BYTES * 4u + 64u) {
        code = CE_INTERNAL_ERROR;
        return false;
    }
    std::vector<wchar_t> buffer(required);
    code = api_.format_value_v2(type, flags, bytes, length, buffer.data(), required, &required);
    if (code != CE_OK) return false;
    out.assign(buffer.data());
    return true;
}

std::wstring Session::live_value(const RecordRow& row) {
    if (row.opaque) return L"<opaque CT record: preserved, not editable>";
    if (row.type > CE_TYPE_AOB || !row.byte_length || row.byte_length > CE_V2_MAX_VALUE_BYTES)
        return L"<unsupported value width/type>";
    const uint32_t length = (std::min)(row.byte_length, current_value_limit);
    std::vector<uint8_t> bytes(length);
    if (!row.resolved || api_.read_bytes_v2(row.resolved, bytes.data(), length) != CE_OK)
        return L"<unreadable>";
    std::wstring value;
    int code = CE_OK;
    const bool limited = length < row.byte_length;
    if (!format_bytes(row.type, row.flags, bytes.data(), length, value, code))
        return limited ? L"<decode error at 256-byte preview boundary; truncated>"
                       : L"<invalid encoded value>";
    if (limited) value += L" ... [truncated: first 256 / " + std::to_wstring(row.byte_length) + L" bytes]";
    return value;
}

// ---- window and session ----------------------------------------------------------

Session::Answer Session::cmd_window(const std::wstring& name, const json::Value* args) {
    if (host_.window) host_.window(name, args);
    return Answer{};
}

Session::Answer Session::cmd_session(const std::wstring& name, const json::Value* args) {
    (void)name;
    (void)args;
    // The host's window procedure owns the actual stop sequence; this posts the
    // same private message the public CE_RequestStop export does.
    const int code = api_.request_stop();
    if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
    return Answer{};
}

// ---- scan ------------------------------------------------------------------------

Session::Answer Session::cmd_scan(const std::wstring& name, const json::Value* args) {
    if (name == L"scan.cancel") {
        cancel();
        return Answer{};
    }
    if (name == L"scan.new") {
        if (!idle()) return Answer{CE_BUSY, L"The session is busy; wait for the scan to finish.", {}};
        const int code = api_.new_scan_v2();
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        have_scan_ = false;
        scan_info_ = CeScanInfoV2{};
        results_start_ = 0;
        results_total_ = 0;
        set_status(L"New scan: OK (0)");
        deferred_refresh_ = true;
        return Answer{};
    }
    if (name == L"scan.undo") {
        if (!idle()) return Answer{CE_BUSY, L"The session is busy; wait for the scan to finish.", {}};
        const int code = api_.undo_scan_v2();
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        set_status(L"Undo scan: OK (0)");
        deferred_refresh_ = true;
        return Answer{};
    }
    if (!idle()) return Answer{CE_BUSY, L"The session is busy; wait for the scan to finish.", {}};

    const bool first = name == L"scan.first";
    CeScanRequestV2 request{};
    request.size = sizeof(request);
    request.version = CE_V2_VERSION;
    request.type = arg_u32(args, L"type");
    request.flags = value_flags(args);
    request.comparison = arg_u32(args, L"comparison");
    request.alignment = arg_u32(args, L"alignment");
    if (request.type > CE_TYPE_AOB) return Answer{CE_INVALID_ARGUMENT, L"Unknown value type.", {}};
    // Rounding is a CE concept only for exact float/double scans; every other
    // combination is rejected by the core, so it is normalised away here.
    const bool rounding_applicable = (request.type == CE_TYPE_FLOAT || request.type == CE_TYPE_DOUBLE) &&
                                     request.comparison == CE_CMP_EXACT;
    request.rounding = rounding_applicable ? arg_u32(args, L"rounding") : CE_ROUND_EXACT;
    std::wstring detail;
    if (!length_input(request.type, arg_string(args, L"byteLength"), request.byte_length, false, detail))
        return Answer{CE_INVALID_ARGUMENT, detail, {}};
    const std::wstring value = arg_string(args, L"value");
    const std::wstring second = arg_string(args, L"second");
    const std::wstring begin = trim(arg_string(args, L"begin"));
    const std::wstring end = trim(arg_string(args, L"end"));
    if (!begin.empty() || !end.empty()) {
        if (!parse_unsigned(begin, 16, request.begin) || !parse_unsigned(end, 16, request.end) ||
            request.begin >= request.end) {
            return Answer{CE_INVALID_ARGUMENT,
                          L"Range needs two hexadecimal addresses with start < end, or leave both blank.",
                          {}};
        }
    }
    // A next scan cannot change the format; the committed scan owns it.
    if (!first && have_scan_) {
        request.type = scan_info_.type;
        request.flags = scan_info_.flags;
        request.alignment = scan_info_.alignment;
        request.byte_length = scan_info_.byte_length;
    }

    scan_was_first_ = first;
    completion_operation_ = first ? L"First scan" : L"Next scan";
    completion_status_ = CE_OK;
    completion_detail_.clear();
    job_ = Job::value_scan;
    cancel_requested_ = false;
    worker_done_.store(false);
    struct Input {
        CeScanRequestV2 request{};
        std::wstring value, second;
    };
    Input input;
    input.request = request;
    input.value = value;
    input.second = second;
    Wake* wake = &wake_;
    Session* self = this;
    try {
        worker_ = std::thread([self, first, input = std::move(input), wake]() mutable noexcept {
            int status = CE_INTERNAL_ERROR;
            std::wstring diagnostic;
            try {
                if (self->stopping_) {
                    status = CE_CANCELLED;
                    diagnostic = L"Stop requested before scan entry.";
                } else {
                    // Point the request at the captured strings only now: their
                    // storage is final here, so the pointers stay valid for the
                    // whole call.
                    input.request.value = input.value.c_str();
                    input.request.value2 = input.second.c_str();
                    status = first ? self->api_.first_scan_v2(&input.request)
                                   : self->api_.next_scan_v2(&input.request);
                    diagnostic = self->diagnostic_of(status);
                }
            } catch (...) {
                status = CE_INTERNAL_ERROR;
                diagnostic = L"Unexpected scan worker exception.";
            }
            self->completion_status_ = status;
            self->completion_detail_ = std::move(diagnostic);
            self->worker_done_.store(true);
            if (*wake) (*wake)();
        });
    } catch (...) {
        job_ = Job::none;
        return Answer{CE_INTERNAL_ERROR, L"Cannot create scan worker thread.", {}};
    }
    set_status(first ? L"First scan running asynchronously. Cancel and window toggle remain available."
                     : L"Next scan running asynchronously. Cancel and window toggle remain available.");
    return Answer{};
}

// ---- results ---------------------------------------------------------------------

void Session::load_result_rows(bool passive) {
    if (!results_valid_ || results_next_ >= results_.size() || scan_busy() || dialog_active_) return;
    // Wide snapshots cannot be filled synchronously without blocking the UI, so
    // the visible window loads eagerly and only off-screen rows fall back to the
    // throttled passive budget. That keeps placeholder churn off the screen the
    // user is reading.
    const size_t eager = passive ? (std::min)(results_.size(), live_last_ ? live_last_ : live_row_budget + 1)
                                 : results_.size();
    size_t rows_left = passive ? eager + live_row_budget : results_.size();
    uint64_t bytes_left = passive ? static_cast<uint64_t>(eager) * CE_V2_MAX_VALUE_BYTES + live_byte_budget
                                  : static_cast<uint64_t>(results_.size()) * CE_V2_MAX_VALUE_BYTES;
    const uint64_t started = GetTickCount64();
    while (results_next_ < results_.size() && rows_left && scan_info_.byte_length <= bytes_left) {
        const size_t index = results_next_;
        CeResultV2 entry{};
        entry.size = sizeof(entry);
        entry.version = CE_V2_VERSION;
        std::vector<uint8_t> bytes(scan_info_.byte_length);
        uint32_t required = 0;
        // GetResult's size query also reads disk; the known scan width avoids
        // that duplicate I/O.
        const int result = api_.get_result_v2(scan_info_.generation, results_start_ + index, &entry,
                                              bytes.data(), static_cast<uint32_t>(bytes.size()),
                                              &required);
        ResultRow& row = results_[index];
        if (result != CE_OK || required != bytes.size()) {
            row.loaded = true;
            row.value = result == CE_BUSY ? L"<generation changed; refresh pending>" : L"<unavailable>";
            results_dirty_ = true;
            results_dirty_from_ = (std::min)(results_dirty_from_, index);
            results_next_ = results_.size();
            return;
        }
        std::wstring value;
        if (bytes.size() > current_value_limit)
            value = L"<long snapshot: " + std::to_wstring(bytes.size()) +
                    L" bytes; activate to read full value>";
        else {
            int code = CE_OK;
            if (!format_bytes(entry.type, entry.flags, bytes.data(),
                              static_cast<uint32_t>(bytes.size()), value, code))
                value = L"<invalid typed value>";
        }
        row.loaded = true;
        row.address = entry.address;
        row.value = std::move(value);
        results_dirty_ = true;
        results_dirty_from_ = (std::min)(results_dirty_from_, index);
        ++results_next_;
        --rows_left;
        bytes_left -= bytes.size();
        // The visible window always finishes this tick; the time budget only
        // throttles rows beyond the first screenful.
        if (passive && results_next_ >= eager && GetTickCount64() - started >= live_tick_budget_ms) break;
    }
}

bool Session::refresh_results(bool passive) {
    if (scan_busy() || stopping_) return false;
    CeScanInfoV2 info{};
    info.size = sizeof(info);
    info.version = CE_V2_VERSION;
    int code = api_.get_scan_info_v2(&info);
    if (code != CE_OK) return false;
    if (info.count && (!info.byte_length || info.byte_length > CE_V2_MAX_VALUE_BYTES)) return false;
    scan_info_ = info;
    results_total_ = info.count;
    have_scan_ = info.byte_length != 0;
    if (!results_total_) results_start_ = 0;
    else if (results_start_ >= results_total_) results_start_ = ((results_total_ - 1) / page_size) * page_size;
    const size_t wanted =
        static_cast<size_t>((std::min)(page_size, results_total_ - results_start_));
    results_.assign(wanted, ResultRow{});
    results_valid_ = true;
    results_next_ = 0;
    results_reset_ = true;
    results_dirty_ = true;
    results_dirty_from_ = 0;
    load_result_rows(passive || info.byte_length > current_value_limit);
    (void)passive;
    return true;
}

Session::Answer Session::cmd_results(const std::wstring& name, const json::Value* args) {
    if (name == L"results.refresh") {
        if (!idle()) {
            deferred_refresh_ = true;
            return Answer{};
        }
        if (!refresh_results(false)) return Answer{CE_BUSY, diagnostic_of(CE_BUSY), {}};
        set_status(L"Refresh scan results: OK (0)");
        Answer answer;
        answer.data = results_json();
        return answer;
    }
    if (name == L"results.page") {
        if (scan_busy()) return Answer{CE_BUSY, L"The scan is still running.", {}};
        const int32_t delta = arg_i32(args, L"delta");
        int64_t start = static_cast<int64_t>(results_total_ ? results_start_ : 0);
        if (delta) start += delta;
        else start = static_cast<int64_t>(arg_u64(args, L"start"));
        if (start < 0) start = 0;
        if (results_total_) {
            const int64_t last_page = static_cast<int64_t>(((results_total_ - 1) / page_size) * page_size);
            if (start > last_page) start = last_page;
        } else start = 0;
        results_start_ = static_cast<uint64_t>(start);
        results_valid_ = false;
        if (!refresh_results(false)) return Answer{CE_BUSY, diagnostic_of(CE_BUSY), {}};
        Answer answer;
        answer.data = results_json();
        return answer;
    }
    if (name == L"results.activate") {
        if (scan_busy()) return Answer{CE_BUSY, L"The scan is still running.", {}};
        const uint64_t index = arg_u64(args, L"index");
        if (index >= results_.size()) return Answer{CE_INVALID_ARGUMENT, L"No such result row.", {}};
        const ResultRow& row = results_[index];
        if (!row.loaded) return Answer{CE_BUSY, L"That row is still loading; try again in a moment.", {}};
        CeAddressV2 address{};
        address.base = row.address;
        uint64_t resolved = 0;
        const int code = api_.resolve_pointer_v2(&address, &resolved);
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        std::wstring read_value;
        uint32_t length = scan_info_.byte_length;
        if (length && length <= CE_V2_MAX_VALUE_BYTES) {
            std::vector<uint8_t> bytes(length);
            int read_code = api_.read_bytes_v2(resolved, bytes.data(), length);
            if (read_code == CE_OK) format_bytes(scan_info_.type, scan_info_.flags, bytes.data(), length, read_value, read_code);
        }
        // Only a successfully read value may replace the previous target's
        // writable draft.
        const bool read_ok = !read_value.empty();
        const std::wstring current = read_ok
            ? L"Read " + address_text(resolved) + L" = " + read_value + L" (snapshot)"
            : L"Current value: unavailable.";
        selected_id_ = 0;
        selected_index_ = -1;
        set_status(L"Read typed value: OK (0)");
        Answer answer;
        answer.data = editor_payload(row.address, L"", L"", scan_info_.type, scan_info_.flags,
                                     scan_info_.byte_length, read_ok ? read_value : L"", resolved, 0,
                                     current);
        return answer;
    }
    return Answer{CE_INVALID_ARGUMENT, L"Unknown results command: " + name, {}};
}

// ---- records ---------------------------------------------------------------------

void Session::refresh_records() {
    if (scan_busy() || stopping_) return;
    uint64_t count = 0;
    if (api_.record_count_v2(&count) != CE_OK) return;
    records_total_ = count;
    if (!count) records_start_ = 0;
    else if (records_start_ >= count) records_start_ = ((count - 1) / page_size) * page_size;
    records_.clear();
    records_reset_ = true;
    records_dirty_ = true;
    records_dirty_from_ = 0;
    bool success = true;
    selected_index_ = -1;
    const uint64_t wanted = (std::min)(static_cast<uint64_t>(page_size), count - records_start_);
    for (uint64_t i = 0; i < wanted; ++i) {
        CeRecordInfoV2 entry{};
        entry.size = sizeof(entry);
        entry.version = CE_V2_VERSION;
        uint32_t required = 0;
        if (api_.get_record_v2(records_start_ + i, &entry, nullptr, 0, &required) != CE_OK) {
            success = false;
            break;
        }
        if (!required || required > 1048576) {
            success = false;
            break;
        }
        std::vector<wchar_t> description(required);
        if (api_.get_record_v2(records_start_ + i, &entry, description.data(), required, &required) !=
            CE_OK) {
            success = false;
            break;
        }
        RecordRow row;
        row.id = entry.id;
        row.resolved = entry.resolved_address;
        row.type = entry.type;
        row.flags = entry.flags;
        row.byte_length = entry.byte_length;
        row.frozen = entry.frozen;
        row.last_status = entry.last_status;
        row.address = entry.address;
        row.description = description.data();
        // A CT record whose type this build cannot represent: preserved on export,
        // never editable. A normal record always has a nonzero width, so this pair
        // is unambiguous.
        row.opaque = entry.last_status == CE_UNSUPPORTED && entry.byte_length == 0;
        row.value = L"<pending live read>";
        records_.push_back(std::move(row));
        if (records_.back().id == selected_id_) selected_index_ = static_cast<int>(i);
    }
    if (selected_index_ < 0) {
        selected_id_ = 0;
        selected_index_ = -1;
    }
    records_stale_ = !success;
    records_valid_ = true;
    live_cursor_ = 0;
    last_live_poll_ = 0;
}

void Session::poll_record_values(bool visible, bool minimized) {
    if (scan_busy() || stopping_ || dialog_active_ || records_stale_ || !visible || minimized) return;
    if (!records_valid_ || records_.empty()) return;
    const uint64_t now = GetTickCount64();
    if (now - last_live_poll_ < live_poll_interval_ms) return;
    last_live_poll_ = now;
    uint64_t count = 0;
    if (api_.record_count_v2(&count) != CE_OK) return;
    if (count != records_total_) {
        records_stale_ = true;
        return;
    }
    const size_t first = (std::min)(live_first_, records_.size());
    const size_t last = (std::min)(live_last_ ? live_last_ : records_.size(), records_.size());
    if (first >= last) return;
    if (live_cursor_ < first || live_cursor_ >= last) live_cursor_ = first;
    const size_t wanted = (std::min)(live_row_budget, last - first);
    uint64_t bytes_left = live_byte_budget;
    for (size_t n = 0; n < wanted; ++n) {
        const size_t index = live_cursor_;
        live_cursor_ = index + 1 < last ? index + 1 : first;
        CeRecordInfoV2 entry{};
        entry.size = sizeof(entry);
        entry.version = CE_V2_VERSION;
        uint32_t required = 0;
        if (api_.get_record_v2(records_start_ + index, &entry, nullptr, 0, &required) != CE_OK) return;
        if (entry.id != records_[index].id) {
            records_stale_ = true;
            return;
        }
        const size_t bytes = (std::min)(entry.byte_length, current_value_limit);
        if (bytes > bytes_left) break;
        RecordRow& row = records_[index];
        row.resolved = entry.resolved_address;
        row.frozen = entry.frozen;
        row.last_status = entry.last_status;
        row.value = live_value(row);
        row.has_value = true;
        records_dirty_ = true;
        records_dirty_from_ = (std::min)(records_dirty_from_, index);
        bytes_left -= bytes;
        if (GetTickCount64() - now >= live_tick_budget_ms) break;
    }
}

Session::Answer Session::cmd_record(const std::wstring& name, const json::Value* args) {
    if (name == L"record.viewport") {
        // The browser owns the scroll position, so it tells the bridge which rows
        // are on screen and live sampling follows.
        live_first_ = static_cast<size_t>(arg_u64(args, L"first"));
        live_last_ = static_cast<size_t>(arg_u64(args, L"last"));
        return Answer{};
    }
    if (name == L"record.refresh") {
        if (!idle()) {
            deferred_record_refresh_ = true;
            return Answer{};
        }
        refresh_records();
        Answer answer;
        answer.data = records_json();
        return answer;
    }
    if (name == L"record.page") {
        const int32_t delta = arg_i32(args, L"delta");
        int64_t start = static_cast<int64_t>(records_start_);
        if (delta) start += delta;
        else start = static_cast<int64_t>(arg_u64(args, L"start"));
        if (start < 0) start = 0;
        if (records_total_) {
            const int64_t last_page = static_cast<int64_t>(((records_total_ - 1) / page_size) * page_size);
            if (start > last_page) start = last_page;
        } else start = 0;
        records_start_ = static_cast<uint64_t>(start);
        refresh_records();
        Answer answer;
        answer.data = records_json();
        return answer;
    }
    if (name == L"record.select") {
        const uint64_t index = arg_u64(args, L"index");
        if (index >= records_.size()) return Answer{CE_INVALID_ARGUMENT, L"No such record row.", {}};
        const RecordRow& row = records_[index];
        selected_id_ = row.id;
        selected_index_ = static_cast<int>(index);
        if (row.opaque) {
            // Filling the editor would let "Add record" re-point a record the user
            // cannot see or edit, so only the description is echoed back and the
            // payload is flagged read-only.
            Answer answer;
            answer.data = editor_payload(0, L"", row.description, 0, 0, 0, L"", 0, row.id,
                                         L"Selected record ID: " + std::to_wstring(row.id) +
                                             L" - imported CT record; read-only",
                                         true);
            return answer;
        }
        uint64_t resolved = 0;
        CeAddressV2 address = row.address;
        const int code = api_.resolve_pointer_v2(&address, &resolved);
        const int resolve_code = code;
        std::wstring value;
        uint32_t length = row.byte_length;
        if (resolve_code == CE_OK && length && length <= CE_V2_MAX_VALUE_BYTES) {
            std::vector<uint8_t> bytes(length);
            int read_code = api_.read_bytes_v2(resolved, bytes.data(), length);
            if (read_code == CE_OK)
                format_bytes(row.type, row.flags, bytes.data(), length, value, read_code);
        }
        const std::wstring current = value.empty()
            ? L"Current value: unavailable."
            : L"Read " + address_text(resolved) + L" = " + value + L" (snapshot)";
        Answer answer;
        answer.data = editor_payload(row.address.base, offsets_text(row.address), row.description,
                                     row.type, row.flags, row.byte_length, value,
                                     resolve_code == CE_OK ? resolved : 0, row.id, current);
        return answer;
    }
    if (name == L"record.read") {
        CeAddressV2 address{};
        uint64_t resolved = 0;
        int code = CE_OK;
        if (!resolve(arg_string(args, L"base"), arg_string(args, L"offsets"), address, resolved, code)) {
            set_status(L"Resolve base expression: " + std::wstring(status_name(code)) + L" (" +
                       std::to_wstring(code) + L")" +
                       (code != CE_OK && !diagnostic_of(code).empty() ? L" - " + diagnostic_of(code) : L""));
            return Answer{code, diagnostic_of(code), {}};
        }
        const uint32_t type = arg_u32(args, L"type");
        uint32_t length = 0;
        std::wstring detail;
        if (!length_input(type, arg_string(args, L"byteLength"), length, true, detail))
            return Answer{CE_INVALID_ARGUMENT, detail, {}};
        std::vector<uint8_t> bytes(length);
        code = api_.read_bytes_v2(resolved, bytes.data(), length);
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        std::wstring value;
        if (!format_bytes(type, value_flags(args), bytes.data(), length, value, code))
            return Answer{code, diagnostic_of(code), {}};
        set_status(L"Read typed value: OK (0)");
        Answer answer;
        answer.data = editor_payload(address.base, offsets_text(address), L"", type, value_flags(args),
                                     length, value, resolved, selected_id_,
                                     L"Read " + address_text(resolved) + L" = " + value + L" (snapshot)");
        return answer;
    }
    if (name == L"record.writeAddress") {
        CeAddressV2 address{};
        uint64_t resolved = 0;
        int code = CE_OK;
        if (!resolve(arg_string(args, L"base"), arg_string(args, L"offsets"), address, resolved, code))
            return Answer{code, diagnostic_of(code), {}};
        const uint32_t type = arg_u32(args, L"type");
        uint32_t length = 0;
        std::wstring detail;
        if (!length_input(type, arg_string(args, L"byteLength"), length, false, detail))
            return Answer{CE_INVALID_ARGUMENT, detail, {}};
        const std::wstring value = arg_string(args, L"value");
        code = api_.write_value_v2(resolved, type, value_flags(args), length, value.c_str());
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        set_status(L"Write typed address: OK (0)");
        return Answer{};
    }
    if (name == L"record.add" || name == L"record.update") {
        const bool update = name == L"record.update";
        CeRecordRequestV2 request{};
        request.size = sizeof(request);
        request.version = CE_V2_VERSION;
        request.id = update ? selected_id_ : 0;
        if (update) {
            int index = 0;
            Answer problem;
            if (!validate_selection(index, problem)) return problem;
            if (records_[static_cast<size_t>(index)].opaque)
                return Answer{CE_UNSUPPORTED, L"An imported CT record cannot be updated.", {}};
        }
        uint64_t resolved = 0;
        int code = CE_OK;
        if (!resolve(arg_string(args, L"base"), arg_string(args, L"offsets"), request.address, resolved, code))
            return Answer{code, diagnostic_of(code), {}};
        request.type = arg_u32(args, L"type");
        request.flags = value_flags(args);
        const bool snapshot = arg_bool(args, L"snapshot", true);
        std::wstring detail;
        if (!length_input(request.type, arg_string(args, L"byteLength"), request.byte_length, snapshot,
                          detail))
            return Answer{CE_INVALID_ARGUMENT, detail, {}};
        const std::wstring description = arg_string(args, L"description");
        const std::wstring value = arg_string(args, L"value");
        request.description = description.c_str();
        request.value = snapshot ? nullptr : value.c_str();
        uint64_t id = 0;
        code = api_.upsert_record_v2(&request, &id);
        if (code != CE_OK) {
            if (update) refresh_records();
            return Answer{code, diagnostic_of(code), {}};
        }
        selected_id_ = id;
        refresh_records();
        set_status(std::wstring(update ? L"Update address record: OK (0)" : L"Add address record: OK (0)"));
        Answer answer;
        answer.data = std::wstring(L"{\"selected\":\"") + std::to_wstring(id) + L"\"}";
        return answer;
    }
    if (name == L"record.write" || name == L"record.freeze" || name == L"record.remove") {
        int index = 0;
        Answer problem;
        // Removal only needs an identity, not an editable record: an imported
        // opaque row can be discarded but never written or frozen.
        if (name != L"record.remove" && records_stale_) return Answer{CE_BUSY, L"Refresh the record list first.", {}};
        if (!selected_id_ || selected_index_ < 0 ||
            static_cast<size_t>(selected_index_) >= records_.size() ||
            records_[static_cast<size_t>(selected_index_)].id != selected_id_) {
            return Answer{CE_INVALID_ARGUMENT,
                          L"Select an address-list record, or Refresh a changed listing first.", {}};
        }
        if (name != L"record.remove" && !validate_selection(index, problem)) return problem;
        bool removed = false;
        if (name == L"record.remove") {
            const int code = api_.remove_record_v2(selected_id_);
            if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
            set_status(L"Remove record: OK (0)");
            // Only removal takes the selection away; writing and freezing leave
            // the record selected so the next click has the same target.
            removed = true;
        } else if (name == L"record.write") {
            const std::wstring value = arg_string(args, L"value");
            const int code = api_.write_record_v2(selected_id_, value.c_str());
            if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
            set_status(L"Write record: OK (0)");
        } else {
            if (records_[static_cast<size_t>(index)].opaque)
                return Answer{CE_UNSUPPORTED, L"An imported CT record cannot be frozen.", {}};
            const int code = api_.freeze_record_v2(selected_id_, arg_bool(args, L"enabled") ? 1 : 0);
            if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
            set_status(std::wstring(L"Freeze record: OK (0)"));
        }
        if (removed) {
            selected_id_ = 0;
            selected_index_ = -1;
        }
        refresh_records();
        Answer answer;
        answer.data = records_json();
        return answer;
    }
    if (name == L"record.saveCT" || name == L"record.openCT") {
        const bool save = name == L"record.saveCT";
        // An explicit path keeps this testable headlessly; otherwise the host
        // opens its native file picker.
        std::wstring path = arg_string(args, L"path");
        if (path.empty()) {
            if (!host_.table_dialog)
                return Answer{CE_INVALID_ARGUMENT, L"No file dialog is available.", {}};
            path = host_.table_dialog(save);
            if (path.empty()) {
                set_status(L"CT file dialog: cancelled.");
                Answer cancelled;
                cancelled.data = L"{\"cancelled\":true}";
                return cancelled;
            }
        }
        const int code = save ? api_.save_table_v2(path.c_str()) : api_.load_table_v2(path.c_str());
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        selected_id_ = 0;
        selected_index_ = -1;
        records_start_ = 0;
        records_stale_ = false;
        refresh_records();
        set_status(std::wstring(save ? L"Save CT: OK (0)" : L"Open CT: OK (0)"));
        Answer answer;
        answer.data = records_json();
        return answer;
    }
    return Answer{CE_INVALID_ARGUMENT, L"Unknown record command: " + name, {}};
}

// ---- views -----------------------------------------------------------------------

bool Session::load_view(int kind, bool refresh) {
    View* target = view(kind);
    if (!target) return false;
    // Both calls enumerate, so a host that changed in between needs another
    // attempt. Three is plenty and stops a hostile host from spinning here.
    for (int attempt = 0; attempt < 3; ++attempt) {
        uint32_t required = 0;
        int code = CE_OK;
        switch (kind) {
        case 0: code = api_.get_regions_v2(nullptr, 0, &required); break;
        case 1: code = api_.get_modules_v2(nullptr, 0, &required); break;
        default: code = api_.get_threads_v2(nullptr, 0, &required); break;
        }
        if (code != CE_OK) return false;
        if (required > CE_V2_MAX_VIEW_ITEMS) return false;
        std::vector<std::wstring> rendered;
        target->rows.clear();
        if (kind == 0) {
            std::vector<CeRegionInfoV2> items(required);
            code = required ? api_.get_regions_v2(items.data(), required, &required) : CE_OK;
            if (code == CE_INVALID_ARGUMENT) continue;
            if (code != CE_OK) return false;
            items.resize(required);
            for (const auto& entry : items) {
                ViewRow row;
                row.cells = {address_text(entry.base), hex_text(entry.region_size),
                             region_state_name(entry.state), protect_name(entry.protect),
                             protect_name(entry.allocation_protect), region_type_name(entry.type)};
                target->rows.push_back(std::move(row));
            }
        } else if (kind == 1) {
            std::vector<CeModuleInfoV2> items(required);
            code = required ? api_.get_modules_v2(items.data(), required, &required) : CE_OK;
            if (code == CE_INVALID_ARGUMENT) continue;
            if (code != CE_OK) return false;
            items.resize(required);
            for (const auto& entry : items) {
                ViewRow row;
                row.cells = {address_text(entry.base), hex_text(entry.module_size),
                             file_name_of(entry.path), entry.path};
                target->rows.push_back(std::move(row));
            }
        } else {
            std::vector<CeThreadInfoV2> items(required);
            code = required ? api_.get_threads_v2(items.data(), required, &required) : CE_OK;
            if (code == CE_INVALID_ARGUMENT) continue;
            if (code != CE_OK) return false;
            items.resize(required);
            for (const auto& entry : items) {
                const bool has_priority = (entry.valid_mask & (1u << 1)) != 0;
                ViewRow row;
                row.cells = {std::to_wstring(entry.thread_id),
                             has_priority ? std::to_wstring(static_cast<int32_t>(entry.priority))
                                          : L"<denied>",
                             file_time_text(entry.created),
                             (entry.valid_mask & 1u) ? std::wstring(entry.description)
                                                     : std::wstring(L"<unnamed>"),
                             entry.current ? L"Current" : L""};
                target->rows.push_back(std::move(row));
            }
        }
        target->total = target->rows.size();
        target->loaded = true;
        if (!target->total) target->start = 0;
        else if (target->start >= target->total)
            target->start = ((target->total - 1) / page_size) * page_size;
        if (refresh) target->refresh_requested = false;
        return true;
    }
    return false;
}

// ---- memory ----------------------------------------------------------------------

void Session::remember_page() {
    const MemoryStep step{memory_base_, memory_page_};
    if (memory_history_at_ < memory_history_.size() &&
        memory_history_[memory_history_at_].page == step.page)
        return;
    // Truncate the redo tail before appending. Doing it with resize() on an empty
    // history would create a phantom entry and make "Back" light up on the first
    // page anyone ever visited.
    if (!memory_history_.empty()) memory_history_.resize(memory_history_at_ + 1);
    memory_history_.push_back(step);
    if (memory_history_.size() > memory_history_limit)
        memory_history_.erase(memory_history_.begin());
    memory_history_at_ = memory_history_.size() - 1;
}

bool Session::can_move_memory(bool next) const noexcept {
    if (!memory_valid_) return false;
    if (next)
        return memory_page_ <= host_max_ && host_max_ - memory_page_ >= memory_page_bytes &&
               page_address_allowed(memory_page_ + memory_page_bytes, host_min_, host_max_);
    return memory_page_ >= memory_page_bytes &&
           page_address_allowed(memory_page_ - memory_page_bytes, host_min_, host_max_);
}

std::wstring Session::memory_label() const {
    if (!memory_valid_)
        return L"Resolved base: none | Page: none (Resolve + preview required)";
    return L"Resolved base: " + address_text(memory_base_) + L" | Page: " + address_text(memory_page_) +
           L" | 256 bytes";
}

// Reads one 256-byte page. A failed 16-byte read falls back to byte-at-a-time so
// a single unreadable byte does not blank the whole row, and the first failure
// is reported alongside the bytes.
bool Session::memory_payload(uint64_t address, Answer& answer) {
    std::wstring hex;
    std::wstring readable;
    hex.reserve(memory_page_bytes * 2);
    readable.reserve(memory_page_bytes);
    unsigned unreadable = 0;
    int first_error = CE_OK;
    std::wstring first_detail;
    for (uint64_t row = 0; row < memory_page_bytes; row += 16) {
        uint8_t bytes[16]{};
        bool ok[16]{};
        const int row_result = api_.read_bytes_v2(address + row, bytes, 16);
        for (int i = 0; i < 16; ++i) {
            if (row_result == CE_OK) {
                ok[i] = true;
                continue;
            }
            uint8_t byte = 0;
            const int result = api_.read_bytes_v2(address + row + static_cast<uint64_t>(i), &byte, 1);
            if (result != CE_OK) {
                if (first_error == CE_OK) {
                    first_error = result;
                    first_detail = diagnostic_of(result);
                }
                ++unreadable;
                continue;
            }
            bytes[i] = byte;
            ok[i] = true;
        }
        hex += bytes_to_hex(bytes, 16);
        for (int i = 0; i < 16; ++i) readable += ok[i] ? L'1' : L'0';
    }
    json::Writer writer;
    writer.object()
        .key(L"base").u64(memory_base_)
        .key(L"page").u64(address)
        .key(L"address").string(address_text(address))
        .key(L"label").string(memory_label())
        .key(L"bytes").string(hex)
        .key(L"readable").string(readable)
        .key(L"unreadable").integer(static_cast<long long>(unreadable))
        .key(L"firstError").integer(static_cast<long long>(first_error))
        .key(L"firstDetail").string(first_detail)
        .key(L"canBack").boolean(memory_history_at_ > 0)
        .key(L"canForward").boolean(memory_history_at_ + 1 < memory_history_.size())
        .key(L"canPrev").boolean(can_move_memory(false))
        .key(L"canNext").boolean(can_move_memory(true))
        .end();
    answer.data = writer.take();
    return true;
}

// ---- pointer scan ----------------------------------------------------------------

void Session::refresh_pointer_results() {
    pointer_total_ = pointer_status_valid_ ? pointer_status_.total : 0;
    pointer_generation_ = pointer_status_valid_ ? pointer_status_.generation : 0;
    if (!pointer_total_) pointer_start_ = 0;
    else if (pointer_start_ >= pointer_total_)
        pointer_start_ = ((pointer_total_ - 1) / page_size) * page_size;
    pointer_page_.clear();
    // One module enumeration for the whole page rather than one per row.
    std::vector<std::pair<uint64_t, uint64_t>> module_ranges;
    uint32_t required = 0;
    if (api_.get_modules_v2(nullptr, 0, &required) == CE_OK && required) {
        std::vector<CeModuleInfoV2> loaded(required);
        if (api_.get_modules_v2(loaded.data(), required, &required) == CE_OK)
            for (const auto& entry : loaded)
                module_ranges.emplace_back(entry.base, entry.base + entry.module_size);
    }
    const uint64_t wanted = (std::min)(static_cast<uint64_t>(page_size),
                                       pointer_total_ > pointer_start_ ? pointer_total_ - pointer_start_ : 0);
    for (uint64_t i = 0; i < wanted; ++i) {
        CePointerScanResultV2 result{};
        result.size = sizeof(result);
        result.version = CE_V2_VERSION;
        if (!pointer_generation_ ||
            api_.get_pointer_scan_result_v2(pointer_generation_, pointer_start_ + i, &result) != CE_OK)
            break;
        pointer_page_.push_back(result);
    }
    pointer_page_valid_ = true;
}

// ---- dispatch --------------------------------------------------------------------

Session::Answer Session::cmd_view(const std::wstring& name, const json::Value* args) {
    View* target = view_for_kind(arg_string(args, L"kind"));
    if (!target) return Answer{CE_INVALID_ARGUMENT, L"Unknown view kind.", {}};
    const int kind = target == &regions_ ? 0 : (target == &modules_ ? 1 : 2);
    if (name == L"view.refresh") {
        if (!idle()) {
            target->refresh_requested = true;
            deferred_view_refresh_ = true;
            return Answer{};
        }
        if (!load_view(kind, true)) return Answer{CE_BUSY, diagnostic_of(CE_BUSY), {}};
        Answer answer;
        answer.data = render_view(kind);
        return answer;
    }
    if (name == L"view.page") {
        const int32_t delta = arg_i32(args, L"delta");
        int64_t start = static_cast<int64_t>(target->start);
        if (delta) start += delta;
        else if (member(args, L"start")) start = static_cast<int64_t>(arg_u64(args, L"start"));
        if (start < 0) start = 0;
        if (target->total) {
            const int64_t last_page = static_cast<int64_t>(((target->total - 1) / page_size) * page_size);
            if (start > last_page) start = last_page;
        } else start = 0;
        target->start = static_cast<uint64_t>(start);
        Answer answer;
        answer.data = render_view(kind);
        return answer;
    }
    if (name == L"view.load") {
        // Entering a page loads it once; paging afterwards is local, so rows
        // cannot shift under the user and a full walk is never repeated.
        if (!target->loaded) {
            if (!idle()) {
                target->refresh_requested = true;
                deferred_view_refresh_ = true;
                return Answer{};
            }
            if (!load_view(kind, true)) return Answer{CE_BUSY, diagnostic_of(CE_BUSY), {}};
        }
        Answer answer;
        answer.data = render_view(kind);
        return answer;
    }
    return Answer{CE_INVALID_ARGUMENT, L"Unknown view command: " + name, {}};
}

Session::Answer Session::cmd_memory(const std::wstring& name, const json::Value* args) {
    if (name == L"memory.resolve" || name == L"memory.page" || name == L"memory.move" ||
        name == L"memory.back" || name == L"memory.forward") {
        uint64_t address = 0;
        if (name == L"memory.resolve") {
            CeAddressV2 chain{};
            uint64_t resolved = 0;
            int code = CE_OK;
            if (!resolve(arg_string(args, L"base"), arg_string(args, L"offsets"), chain, resolved, code))
                return Answer{code, diagnostic_of(code), {}};
            if (!page_address_allowed(resolved, host_min_, host_max_))
                return Answer{CE_INVALID_ARGUMENT,
                              L"A 256-byte preview must fit within this host's application address "
                              L"range.",
                              {}};
            memory_base_ = resolved;
            address = resolved;
            memory_valid_ = true;
        } else if (name == L"memory.move") {
            if (!memory_valid_) return Answer{CE_INVALID_ARGUMENT, L"Resolve a page first.", {}};
            const int64_t delta = arg_i32(args, L"delta", 256);
            const bool next = delta >= 0;
            if (!can_move_memory(next))
                return Answer{CE_INVALID_ARGUMENT,
                              next ? L"The next 256-byte page is outside the host address range."
                                   : L"The previous 256-byte page is outside the host address range.",
                              {}};
            address = next ? memory_page_ + 256 : memory_page_ - 256;
        } else if (name == L"memory.back" || name == L"memory.forward") {
            const bool forward = name == L"memory.forward";
            const int64_t next_at = static_cast<int64_t>(memory_history_at_) + (forward ? 1 : -1);
            if (next_at < 0 || static_cast<size_t>(next_at) >= memory_history_.size())
                return Answer{CE_INVALID_ARGUMENT, L"No further page history.", {}};
            memory_history_at_ = static_cast<size_t>(next_at);
            memory_base_ = memory_history_[memory_history_at_].base;
            address = memory_history_[memory_history_at_].page;
            memory_valid_ = true;
            Answer answer;
            memory_page_ = address;
            if (!memory_payload(address, answer)) return Answer{CE_INTERNAL_ERROR, L"Read failed.", {}};
            return answer;
        } else {
            address = arg_u64(args, L"address");
            if (!page_address_allowed(address, host_min_, host_max_))
                return Answer{CE_INVALID_ARGUMENT,
                              L"A 256-byte preview must fit within this host's application address "
                              L"range.",
                              {}};
            memory_valid_ = true;
        }
        memory_page_ = address;
        remember_page();
        Answer answer;
        if (!memory_payload(address, answer)) return Answer{CE_INTERNAL_ERROR, L"Read failed.", {}};
        return answer;
    }
    if (name == L"memory.readBytes") {
        const uint64_t address = arg_u64(args, L"address");
        uint64_t length = arg_u64(args, L"length", 1);
        if (!length || length > CE_V2_MAX_VALUE_BYTES)
            return Answer{CE_INVALID_ARGUMENT, L"Read length must be 1 to 65536 bytes.", {}};
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        const int code = api_.read_bytes_v2(address, bytes.data(), static_cast<uint32_t>(length));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        Answer answer;
        json::Writer writer;
        writer.object()
            .key(L"address").u64(address)
            .key(L"hex").string(bytes_to_hex(bytes.data(), bytes.size()))
            .end();
        answer.data = writer.take();
        return answer;
    }
    if (name == L"memory.writeBytes") {
        const uint64_t address = arg_u64(args, L"address");
        std::vector<uint8_t> bytes;
        if (!hex_to_bytes(arg_string(args, L"hex"), bytes) || bytes.empty() || bytes.size() > CE_V2_MAX_VALUE_BYTES)
            return Answer{CE_INVALID_ARGUMENT, L"Hex payload must be an even number of hex digits.", {}};
        // Read the bytes being replaced first: that is what Undo restores, and a
        // failure here means the range is not fully readable, so nothing is written.
        std::vector<uint8_t> before(bytes.size());
        int code = api_.read_bytes_v2(address, before.data(), static_cast<uint32_t>(before.size()));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        code = api_.write_bytes_v2(address, bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        if (arg_bool(args, L"undo", true)) {
            HexUndo entry;
            entry.address = address;
            entry.before = std::move(before);
            entry.after = bytes;
            hex_undo_.push_back(std::move(entry));
            if (hex_undo_.size() > hex_undo_limit) hex_undo_.erase(hex_undo_.begin());
        }
        set_status(L"Wrote " + std::to_wstring(bytes.size()) + L" byte(s) at " + address_text(address) + L".");
        return Answer{};
    }
    if (name == L"memory.writeValue") {
        const uint64_t address = arg_u64(args, L"address");
        const uint32_t type = arg_u32(args, L"type");
        uint32_t length = 0;
        std::wstring detail;
        if (!length_input(type, arg_string(args, L"byteLength"), length, false, detail))
            return Answer{CE_INVALID_ARGUMENT, detail, {}};
        const std::wstring text = arg_string(args, L"text");
        const int code = api_.write_value_v2(address, type, arg_u32(args, L"flags"), length, text.c_str());
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        set_status(L"Hex edit cell: OK (0)");
        return Answer{};
    }
    if (name == L"memory.undo") {
        if (hex_undo_.empty()) return Answer{CE_INVALID_ARGUMENT, L"Nothing to undo.", {}};
        HexUndo entry = std::move(hex_undo_.back());
        hex_undo_.pop_back();
        // The host may have changed those bytes since; the write is still undone,
        // and the status says so rather than silently reverting their change.
        std::vector<uint8_t> current(entry.after.size());
        const int read = api_.read_bytes_v2(entry.address, current.data(),
                                            static_cast<uint32_t>(current.size()));
        const bool interfered = read == CE_OK && current != entry.after;
        const int code = api_.write_bytes_v2(entry.address, entry.before.data(),
                                             static_cast<uint32_t>(entry.before.size()));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        set_status(interfered ? L"Hex write undone; the host had changed those bytes since the edit."
                              : L"Hex write undone.");
        return Answer{};
    }
    if (name == L"memory.fill") {
        const uint64_t address = arg_u64(args, L"address");
        const uint64_t length = arg_u64(args, L"length");
        const std::wstring fill_text = trim(arg_string(args, L"byte"));
        uint64_t fill = 0;
        if (!parse_unsigned(fill_text, 16, fill) || fill > 255 || !length ||
            length > CE_V2_MAX_VALUE_BYTES)
            return Answer{CE_INVALID_ARGUMENT,
                          L"Fill needs 1-2 hex digits and a selection of 1 to 65536 bytes.", {}};
        std::vector<uint8_t> before(static_cast<size_t>(length));
        int code = api_.read_bytes_v2(address, before.data(), static_cast<uint32_t>(length));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        std::vector<uint8_t> after(before.size(), static_cast<uint8_t>(fill));
        code = api_.write_bytes_v2(address, after.data(), static_cast<uint32_t>(after.size()));
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        HexUndo entry;
        entry.address = address;
        entry.before = before;
        entry.after = after;
        hex_undo_.push_back(std::move(entry));
        if (hex_undo_.size() > hex_undo_limit) hex_undo_.erase(hex_undo_.begin());
        set_status(L"Filled " + std::to_wstring(length) + L" byte(s) with " + hex_text(fill) + L".");
        return Answer{};
    }
    return Answer{CE_INVALID_ARGUMENT, L"Unknown memory command: " + name, {}};
}

Session::Answer Session::cmd_pointer(const std::wstring& name, const json::Value* args) {
    if (name == L"ptr.cancel") {
        cancel();
        return Answer{};
    }
    if (name == L"ptr.page") {
        if (!pointer_page_valid_) return Answer{CE_INVALID_ARGUMENT, L"No pointer results yet.", {}};
        const int32_t delta = arg_i32(args, L"delta");
        int64_t start = static_cast<int64_t>(pointer_start_);
        if (delta) start += delta;
        else if (member(args, L"start")) start = static_cast<int64_t>(arg_u64(args, L"start"));
        if (start < 0) start = 0;
        if (pointer_total_) {
            const int64_t last_page = static_cast<int64_t>(((pointer_total_ - 1) / page_size) * page_size);
            if (start > last_page) start = last_page;
        } else start = 0;
        pointer_start_ = static_cast<uint64_t>(start);
        refresh_pointer_results();
        Answer answer;
        answer.data = pointer_rows_payload();
        return answer;
    }
    if (name == L"ptr.refresh") {
        poll_pointer_status();
        Answer answer;
        answer.data = pointer_rows_payload();
        return answer;
    }
    if (name == L"ptr.addToList") {
        const uint64_t index = arg_u64(args, L"index");
        if (index >= pointer_page_.size())
            return Answer{CE_INVALID_ARGUMENT, L"Select a pointer scan result first.", {}};
        const auto& result = pointer_page_[static_cast<size_t>(index)];
        CeRecordRequestV2 request{};
        request.size = sizeof(request);
        request.version = CE_V2_VERSION;
        request.type = CE_TYPE_U32;
        request.flags = CE_VALUE_SIGNED;
        request.byte_length = 4;
        request.address.base = result.base;
        request.address.offset_count = result.level_count;
        for (uint32_t i = 0; i < result.level_count && i < CE_V2_MAX_POINTER_LEVELS; ++i)
            request.address.offsets[i] = result.offsets[i];
        const std::wstring description =
            L"pointer scan, " + std::to_wstring(result.level_count) + L" level(s)";
        request.description = description.c_str();
        request.value = nullptr;   // snapshot, never write
        uint64_t id = 0;
        const int code = api_.upsert_record_v2(&request, &id);
        if (code != CE_OK) return Answer{code, diagnostic_of(code), {}};
        records_stale_ = false;
        records_start_ = 0;
        refresh_records();
        set_status(L"Added pointer record " + std::to_wstring(id) + L".");
        Answer answer;
        json::Writer writer;
        writer.object().key(L"id").u64(id).end();
        answer.data = writer.take();
        return answer;
    }
    if (name != L"ptr.scan") return Answer{CE_INVALID_ARGUMENT, L"Unknown pointer command: " + name, {}};
    if (!idle() || job_ != Job::none)
        return Answer{CE_BUSY, L"The session is busy; wait for the running job to finish.", {}};

    const std::wstring target_text = trim(arg_string(args, L"target"));
    uint64_t target = 0;
    // The shared expression evaluator, so a module expression works here too.
    if (target_text.empty() || api_.resolve_address_v2(target_text.c_str(), &target) != CE_OK || !target)
        return Answer{CE_INVALID_ARGUMENT,
                      L"Target must be an address expression, for example the module name plus an "
                      L"offset.",
                      {}};
    uint64_t max_offset = 0;
    if (!parse_unsigned(trim(arg_string(args, L"maxOffset")), 16, max_offset) || !max_offset ||
        max_offset > (1u << 20))
        return Answer{CE_INVALID_ARGUMENT, L"Max offset must be a nonzero hexadecimal value up to 0x100000.", {}};
    CePointerScanRequestV2 request{};
    request.size = sizeof(request);
    request.version = CE_V2_VERSION;
    request.levels = arg_u32(args, L"levels", 1);
    if (!request.levels || request.levels > CE_V2_MAX_POINTER_LEVELS)
        return Answer{CE_INVALID_ARGUMENT, L"Levels must be 1 to 8.", {}};
    request.alignment = arg_u32(args, L"alignment", 8);
    request.max_offset = static_cast<uint32_t>(max_offset);
    // NO_LOOP is always on: a path that revisits its own address is never useful
    // and otherwise multiplies into junk chains up to the level cap.
    request.flags = (arg_bool(args, L"staticOnly") ? CE_PTRSCAN_STATIC_ONLY : 0u) | CE_PTRSCAN_NO_LOOP;
    request.target = target;

    completion_operation_ = L"Pointer scan";
    completion_status_ = CE_OK;
    completion_detail_.clear();
    job_ = Job::pointer_scan;
    cancel_requested_ = false;
    worker_done_.store(false);
    Wake* wake = &wake_;
    Session* self = this;
    try {
        worker_ = std::thread([self, request, wake]() mutable noexcept {
            int status = CE_INTERNAL_ERROR;
            std::wstring diagnostic;
            try {
                if (self->stopping_) status = CE_CANCELLED;
                else {
                    status = self->api_.pointer_scan_v2(&request);
                    diagnostic = self->diagnostic_of(status);
                }
            } catch (...) {
                status = CE_INTERNAL_ERROR;
                diagnostic = L"Unexpected pointer scan worker exception.";
            }
            self->completion_status_ = status;
            self->completion_detail_ = std::move(diagnostic);
            self->worker_done_.store(true);
            if (*wake) (*wake)();
        });
    } catch (...) {
        job_ = Job::none;
        return Answer{CE_INTERNAL_ERROR, L"Cannot create scan worker thread.", {}};
    }
    set_status(L"Pointer scan running asynchronously. Cancel and window toggle remain available. The "
               L"address list keeps working.");
    return Answer{};
}

std::wstring Session::pointer_rows_payload() {
    json::Writer writer;
    writer.object()
        .key(L"start").u64(pointer_start_)
        .key(L"total").u64(pointer_total_)
        .key(L"count").u64(pointer_page_.size())
        .key(L"label").string(L"Results: " + std::to_wstring(pointer_total_) + L" total | " +
                              (pointer_page_.empty()
                                   ? std::wstring(L"0")
                                   : std::to_wstring(pointer_start_ + 1) + L" - " +
                                         std::to_wstring(pointer_start_ + pointer_page_.size())) +
                              L" | 512/page");
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    uint32_t required = 0;
    if (api_.get_modules_v2(nullptr, 0, &required) == CE_OK && required) {
        std::vector<CeModuleInfoV2> loaded(required);
        if (api_.get_modules_v2(loaded.data(), required, &required) == CE_OK)
            for (const auto& entry : loaded)
                ranges.emplace_back(entry.base, entry.base + entry.module_size);
    }
    writer.key(L"rows").array();
    for (const auto& result : pointer_page_) {
        bool is_static = false;
        for (const auto& range : ranges)
            is_static = is_static || (result.base >= range.first && result.base < range.second);
        writer.object()
            .key(L"base").string(address_text(result.base))
            .key(L"offsets").string(pointer_offset_text(result))
            .key(L"static").boolean(is_static)
            .end();
    }
    writer.end();
    writer.end();
    return writer.take();
}
