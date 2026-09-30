// Pointer scan. Everything runs against the test process itself, so the "host" is a
// process whose contents the test controls exactly.
#include <windows.h>
#include "core.hpp"
#include "typed_value.hpp"
#include <cstdint>
#include <cstring>
#include <iostream>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

namespace {
#define CHECK(x) do{if(!(x))throw std::runtime_error(std::string(__FUNCTION__)+":"+std::to_string(__LINE__)+" " #x);}while(false)

// A real static chain: all three live in the module's data, so a static-only scan must
// stop at the outermost one and report a ONE-level path rather than padding to max depth.
int g_value = 0;
int* g_l1 = &g_value;
int** g_l2 = &g_l1;

// The scan arenas live in the module's data rather than on the stack: a stack target moves
// as the test's own frames churn, so a path that verified during the scan could dangle by
// the time it is read back.
alignas(8) unsigned char g_order_arena[0x100];
alignas(8) unsigned char g_negative_arena[0x40];
alignas(8) unsigned char g_loop_arena[0x40];
alignas(8) unsigned char g_truncation_arena[0x100];

CePointerScanRequestV2 request(uint64_t target, uint32_t levels, uint32_t flags = 0,
                               uint32_t max_offset = 0x100, uint32_t alignment = 8) {
    CePointerScanRequestV2 r{};
    r.size = sizeof(r);
    r.version = CE_V2_VERSION;
    r.levels = levels;
    r.alignment = alignment;
    r.max_offset = max_offset;
    r.flags = flags;
    r.target = target;
    return r;
}
std::vector<CePointerScanResultV2> results(ce::Core& c) {
    CePointerScanStatusV2 status{};
    status.size = sizeof(status);
    status.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(status) == CE_OK);
    std::vector<CePointerScanResultV2> out;
    for (uint64_t i = 0; i < status.total; ++i) {
        CePointerScanResultV2 result{};
        result.size = sizeof(result);
        result.version = CE_V2_VERSION;
        CHECK(c.pointer_scan_result_v2(status.generation, i, result) == CE_OK);
        out.push_back(result);
    }
    return out;
}
bool resolves_to(const CePointerScanResultV2& result, uint64_t target) {
    CeAddressV2 chain{};
    chain.base = result.base;
    chain.offset_count = result.level_count;
    for (uint32_t i = 0; i < result.level_count; ++i) chain.offsets[i] = result.offsets[i];
    uint64_t resolved = 0;
    return ce::typed::resolve(chain, resolved) == CE_OK && resolved == target;
}
// Finds the result whose base and offsets match exactly; asserting the whole array is the
// only way to catch a reversed path, which still resolves but lands somewhere else.
const CePointerScanResultV2* find(const std::vector<CePointerScanResultV2>& all, uint64_t base,
                                  std::initializer_list<int64_t> offsets) {
    for (const auto& result : all) {
        if (result.base != base || result.level_count != offsets.size()) continue;
        size_t i = 0;
        bool same = true;
        for (int64_t offset : offsets) same &= result.offsets[i++] == offset;
        if (same) return &result;
    }
    return nullptr;
}

void test_static_chain_stops_at_the_static_base() {
    ce::Core c;
    const auto target = reinterpret_cast<uint64_t>(&g_value);
    CHECK(c.pointer_scan_v2(request(target, 2, CE_PTRSCAN_STATIC_ONLY)) == CE_OK);
    const auto all = results(c);
    CHECK(!all.empty());
    // g_l1 holds &g_value and is itself static, so the chain ends there with one offset.
    const auto* direct = find(all, reinterpret_cast<uint64_t>(&g_l1), {0});
    CHECK(direct && resolves_to(*direct, target));
    for (const auto& result : all) CHECK(resolves_to(result, target));
    std::printf("POINTER_STATIC base=%p offsets=%u\n", reinterpret_cast<void*>(all[0].base), all[0].level_count);
}

// The ordering sentinel. The arena holds three slots forming base -> +0x28 -> +0 -> target
// with DISTINCT offsets, so a reversed array is detectable: it still resolves, it just
// resolves to a different address.
void test_offsets_come_out_in_execution_order() {
    unsigned char* arena = g_order_arena;
    std::memset(arena, 0, sizeof(g_order_arena));
    const uint64_t target = reinterpret_cast<uint64_t>(&arena[0x60]);
    // base=arena+0x40 -> arena+0x20 -> arena+0x00 -> target, offsets [0x10, 0, 0x18].
    *reinterpret_cast<uint64_t*>(arena + 0x00) = target - 0x18;
    *reinterpret_cast<uint64_t*>(arena + 0x20) = reinterpret_cast<uint64_t>(arena + 0x00);
    *reinterpret_cast<uint64_t*>(arena + 0x40) = reinterpret_cast<uint64_t>(arena + 0x10);
    ce::Core c;
    CHECK(c.pointer_scan_v2(request(target, 3, 0, 0x80)) == CE_OK);
    const auto all = results(c);
    const uint64_t outermost = reinterpret_cast<uint64_t>(arena + 0x40);
    // Execution order: read(arena+0x40)=arena+0x10, +0x10 -> arena+0x20;
    // read(arena+0x20)=arena, +0 -> arena; read(arena)=target-0x18, +0x18 -> target.
    const CePointerScanResultV2* three = find(all, outermost, {0x10, 0, 0x18});
    if (!three) {
        std::printf("POINTER_ORDER results=%zu\n", all.size());
        for (const auto& result : all)
            std::printf("  base=%llx levels=%u off0=%lld off1=%lld off2=%lld\n",
                static_cast<unsigned long long>(result.base), result.level_count,
                static_cast<long long>(result.offsets[0]), static_cast<long long>(result.offsets[1]),
                static_cast<long long>(result.offsets[2]));
    }
    CHECK(three && resolves_to(*three, target));
    // Reversing the array must NOT resolve to the target; that is the whole point.
    CePointerScanResultV2 reversed = *three;
    for (uint32_t i = 0; i < reversed.level_count; ++i) reversed.offsets[i] = three->offsets[reversed.level_count - 1 - i];
    CHECK(!resolves_to(reversed, target));
}

void test_negative_offset() {
    unsigned char* arena = g_negative_arena;
    std::memset(arena, 0, sizeof(g_negative_arena));
    const uint64_t target = reinterpret_cast<uint64_t>(&arena[0x20]);
    *reinterpret_cast<uint64_t*>(arena + 0x00) = target + 0x10;   // pointer past the target
    ce::Core c;
    CHECK(c.pointer_scan_v2(request(target, 1, 0, 0x20)) == CE_OK);
    const auto all = results(c);
    const auto* one = find(all, reinterpret_cast<uint64_t>(arena + 0x00), {-0x10});
    CHECK(one && resolves_to(*one, target));
}

void test_no_loop_prunes_self_reference() {
    unsigned char* arena = g_loop_arena;
    std::memset(arena, 0, sizeof(g_loop_arena));
    *reinterpret_cast<uint64_t*>(arena + 0x00) = reinterpret_cast<uint64_t>(arena);
    const uint64_t target = reinterpret_cast<uint64_t>(arena);
    ce::Core plain, looped;
    CHECK(plain.pointer_scan_v2(request(target, 3, 0, 0x40)) == CE_OK);
    CHECK(looped.pointer_scan_v2(request(target, 3, CE_PTRSCAN_NO_LOOP, 0x40)) == CE_OK);
    const auto with_loops = results(plain), without = results(looped);
    // The self-referential slot is reachable without the flag and gone with it. Asserting
    // this specific path, rather than re-resolving the whole set, keeps the test off the
    // heap: a scan covers the entire address space, so any path through reused heap memory
    // can stop resolving for reasons that have nothing to do with NO_LOOP.
    const uint64_t self = reinterpret_cast<uint64_t>(arena);
    CHECK(find(with_loops, self, {0, 0, 0}) != nullptr);
    CHECK(find(without, self, {0, 0, 0}) == nullptr);
    std::printf("POINTER_LOOP self_reference_results=%zu pruned=%zu\n", with_loops.size(), without.size());
}

void test_truncation_is_reported_not_fatal() {
    unsigned char* arena = g_truncation_arena;
    std::memset(arena, 0, sizeof(g_truncation_arena));
    const uint64_t target = reinterpret_cast<uint64_t>(&arena[0x60]);
    // Many slots point into the same window, so one node fans out past the per-node cap.
    for (size_t i = 0; i < 32; ++i) *reinterpret_cast<uint64_t*>(arena + i * 8) = target;
    ce::Core c;
    CHECK(c.pointer_scan_v2(request(target, 1, 0, 0x40)) == CE_OK);
    CePointerScanStatusV2 status{};
    status.size = sizeof(status);
    status.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(status) == CE_OK);
    CHECK(status.active == 0 && status.phase == CE_SCAN_COMPLETED);
    CHECK(status.truncated != 0 && status.total > 0);
    // Every stored path was already re-resolved by the scanner itself; re-checking the
    // whole set here would only test whether the heap stayed still, not the scanner.
    std::printf("POINTER_TRUNCATED truncated=%u results=%llu\n", status.truncated,
        static_cast<unsigned long long>(status.total));
}

void test_generation_guards_results() {
    ce::Core c;
    const auto target = reinterpret_cast<uint64_t>(&g_value);
    CHECK(c.pointer_scan_v2(request(target, 1, CE_PTRSCAN_STATIC_ONLY)) == CE_OK);
    CePointerScanStatusV2 status{};
    status.size = sizeof(status);
    status.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(status) == CE_OK);
    const uint64_t published = status.generation;
    CePointerScanResultV2 result{};
    result.size = sizeof(result);
    result.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_result_v2(published, 0, result) == CE_OK);
    CHECK(c.pointer_scan_result_v2(published + 1, 0, result) == CE_BUSY);
    CHECK(c.pointer_scan_result_v2(published, status.total, result) == CE_INVALID_ARGUMENT);
    // A second scan publishes a new generation and the old one becomes stale.
    CHECK(c.pointer_scan_v2(request(target, 1, CE_PTRSCAN_STATIC_ONLY)) == CE_OK);
    CePointerScanStatusV2 after{};
    after.size = sizeof(after);
    after.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(after) == CE_OK && after.generation != published);
    CHECK(c.pointer_scan_result_v2(published, 0, result) == CE_BUSY);
}

void test_pointer_scan_does_not_disturb_the_value_scan() {
    ce::Core c;
    int32_t marks[4] = {11, 22, 33, 44};
    CeScanRequestV2 scan{};
    scan.size = sizeof(scan);
    scan.version = CE_V2_VERSION;
    scan.type = CE_TYPE_U32;
    scan.comparison = CE_CMP_EXACT;
    scan.alignment = 4;
    scan.byte_length = 4;
    scan.begin = reinterpret_cast<uintptr_t>(marks);
    scan.end = scan.begin + sizeof(marks);
    scan.value = L"33";
    CHECK(c.first_scan_v2(scan) == CE_OK);
    CeScanInfoV2 before{};
    CHECK(c.scan_info_v2(before) == CE_OK && before.count == 1);
    CHECK(c.pointer_scan_v2(request(reinterpret_cast<uint64_t>(&g_value), 1, CE_PTRSCAN_STATIC_ONLY)) == CE_OK);
    CeScanInfoV2 after{};
    CHECK(c.scan_info_v2(after) == CE_OK);
    CHECK(after.generation == before.generation && after.count == before.count);
    // And the results are still readable at the old generation.
    CeResultV2 result{};
    std::vector<uint8_t> bytes;
    CHECK(c.result_v2(before.generation, 0, result, bytes) == CE_OK);
}

void test_admission_is_shared_with_the_value_scan() {
    ce::Core c;
    // Hold the job slot with a pointer scan on a worker, then a value scan must say busy.
    std::atomic<bool> started{false}, release{false};
    CePointerScanStatusV2 before{};
    before.size = sizeof(before);
    before.version = CE_V2_VERSION;
    std::thread worker([&] {
        // A deliberately long scan: every 8-byte slot in the whole address space.
        CePointerScanRequestV2 wide = request(reinterpret_cast<uint64_t>(&g_value), 8, 0, 0x100000, 8);
        started.store(true);
        c.pointer_scan_v2(wide);
        release.store(true);
    });
    while (!started.load()) Sleep(1);
    Sleep(30);
    CeScanRequestV2 scan{};
    scan.size = sizeof(scan);
    scan.version = CE_V2_VERSION;
    scan.type = CE_TYPE_U32;
    scan.comparison = CE_CMP_UNKNOWN;
    scan.alignment = 4;
    scan.byte_length = 4;
    const int status = c.first_scan_v2(scan);
    CHECK(status == CE_BUSY || release.load());
    c.cancel_scan();
    worker.join();
    CHECK(c.pointer_scan_status_v2(before) == CE_OK);
    CHECK(before.active == 0);
}

void test_cancel_reports_cancelled() {
    ce::Core c;
    CePointerScanStatusV2 before{};
    before.size = sizeof(before);
    before.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(before) == CE_OK);
    std::atomic<bool> done{false};
    int status = CE_OK;
    std::thread worker([&] {
        CePointerScanRequestV2 wide = request(reinterpret_cast<uint64_t>(&g_value), 8, 0, 0x100000, 8);
        status = c.pointer_scan_v2(wide);
        done.store(true);
    });
    Sleep(20);
    c.cancel_scan();
    worker.join();
    CHECK(status == CE_CANCELLED);
    CePointerScanStatusV2 after{};
    after.size = sizeof(after);
    after.version = CE_V2_VERSION;
    CHECK(c.pointer_scan_status_v2(after) == CE_OK);
    CHECK(after.active == 0 && after.phase == CE_SCAN_CANCELLED);
    // A cancelled scan leaves the published results and generation alone.
    CHECK(after.generation == before.generation);
}
}

int main() {
    try {
        test_static_chain_stops_at_the_static_base();
        test_offsets_come_out_in_execution_order();
        test_negative_offset();
        test_no_loop_prunes_self_reference();
        test_truncation_is_reported_not_fatal();
        test_generation_guards_results();
        test_pointer_scan_does_not_disturb_the_value_scan();
        test_admission_is_shared_with_the_value_scan();
        test_cancel_reports_cancelled();
        std::puts("POINTER_SCAN_TESTS_PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "POINTER_SCAN_TESTS_FAIL %s\n", error.what());
        std::wcerr << ce::last_error() << L'\n';
        return 1;
    }
}
