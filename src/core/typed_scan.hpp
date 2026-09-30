#pragma once
#include "typed_value.hpp"
#include <functional>
#include <memory>
namespace ce::typed {
struct ScanFile {
    struct Impl;
    std::unique_ptr<Impl> impl;
    CeScanInfoV2 info{};
    ScanFile();
    ~ScanFile();
    ScanFile(const ScanFile&)=delete;
    ScanFile& operator=(const ScanFile&)=delete;
    int stamp(uint64_t generation);
    int result(uint64_t index,uint64_t& address,std::vector<uint8_t>& bytes);
};
struct ScanProgress {
    uint32_t phase = CE_SCAN_ENUMERATING;
    uint32_t unit = CE_SCAN_UNIT_BYTES;
    bool total_known = false;
    uint64_t processed = 0, total = 0;
};
using ProgressCallback = std::function<void(const ScanProgress&)>;
// Progress is reported per page/candidate batch, never once per match.
int scan(const CeScanRequestV2& request, const ScanFile* previous,
    const std::function<bool()>& cancelled, std::unique_ptr<ScanFile>& output,
    const ProgressCallback& progress = {});
}
