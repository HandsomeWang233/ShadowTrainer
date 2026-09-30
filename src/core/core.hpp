#pragma once
#include "ce/api.h"
#include "ce/api_v2.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ce {
struct AddressRecord {
    uint64_t address = 0;
    int32_t value = 0;
    bool frozen = false;
};

// Core owns its result file and address records. No HWND or UI calls here.
// All public methods must be internally thread-safe. Only one scan may run.
class Core {
public:
    Core();
    ~Core();
    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;
    int first_scan(const CeScanRequest& request);
    int next_scan(int32_t value);
    void cancel_scan() noexcept;
    int result_count(uint64_t& count);
    int result(uint64_t index, CeResult& value);
    int read(uint64_t address, int32_t& value);
    int write(uint64_t address, int32_t value);
    int freeze(uint64_t address, int32_t value, bool enabled);
    void tick_freezes();
    std::vector<AddressRecord> records();
    int save_table(const wchar_t* path);
    int load_table(const wchar_t* path);
    int first_scan_v2(const CeScanRequestV2& request);
    int next_scan_v2(const CeScanRequestV2& request);
    int scan_info_v2(CeScanInfoV2& info);
    int scan_status_v2(CeScanStatusV2& status);
    int result_v2(uint64_t generation, uint64_t index, CeResultV2& info, std::vector<uint8_t>& bytes);
    int new_scan_v2();
    int undo_scan_v2();
    int read_bytes_v2(uint64_t address, void* bytes, uint32_t length);
    int write_bytes_v2(uint64_t address, const void* bytes, uint32_t length);
    int write_value_v2(uint64_t address, uint32_t type, uint32_t flags, uint32_t length, const wchar_t* value);
    int format_value_v2(uint32_t type, uint32_t flags, const void* bytes, uint32_t length, std::wstring& text);
    int resolve_address_v2(const wchar_t* expression, uint64_t& address);
    int resolve_pointer_v2(const CeAddressV2& address, uint64_t& resolved);
    int upsert_record_v2(const CeRecordRequestV2& request, uint64_t& id);
    int record_count_v2(uint64_t& count);
    int record_v2(uint64_t index, CeRecordInfoV2& info, std::wstring& description);
    int remove_record_v2(uint64_t id);
    int write_record_v2(uint64_t id, const wchar_t* value);
    int freeze_record_v2(uint64_t id, bool enabled);
    int save_table_v2(const wchar_t* path);
    int load_table_v2(const wchar_t* path);
    int regions_v2(std::vector<CeRegionInfoV2>& out);
    int modules_v2(std::vector<CeModuleInfoV2>& out);
    int threads_v2(std::vector<CeThreadInfoV2>& out);
    int pointer_scan_v2(const CePointerScanRequestV2& request);
    int pointer_scan_result_v2(uint64_t generation, uint64_t index, CePointerScanResultV2& result);
    int pointer_scan_status_v2(CePointerScanStatusV2& status);
    void request_stop() noexcept;
    void shutdown();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Diagnostic lives in the calling thread. Return CeStatus from operations.
void set_error(const std::wstring& text);
const std::wstring& last_error();
}
