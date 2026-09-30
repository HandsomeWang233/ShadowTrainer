#pragma once
// Pointer scan: find multi-level paths from a static base to a target address.
//
// Walking backwards from the target, a slot at address `a` is the next outer pointer
// when `read(a) + offset == current`, so the slot's VALUE is `current - offset` and the
// slot's ADDRESS becomes the new current. Offsets are therefore discovered innermost
// first and must be reversed before they are reported: CeAddressV2 wants execution
// order, outermost first. A reversed path still resolves, it just lands somewhere else,
// which is why the tests assert the array contents and not merely that it resolves.
//
// Cost: the number of offsets per node is 2*max_offset/alignment + 1, so testing each
// offset with its own binary search would cost thousands of searches per node. Instead
// the harvested map is sorted by value once, and each node does ONE upper_bound followed
// by a backwards walk over the hits -- the offset falls out of the value difference.
// Without that the caps would bound memory but not time.
#include "ce/api_v2.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

namespace ce::pointerscan {

// A committed, readable region; [begin, end).
struct Range { uint64_t begin = 0, end = 0; };

// Caps. The first is the memory one: the harvested map is 16 bytes per slot, and this
// DLL lives inside the host process it is scanning.
inline constexpr size_t max_slots = 4u << 20;                 // 4M slots = 64 MiB
inline constexpr size_t max_nodes_per_level = 200000;
inline constexpr size_t max_results = 65536;
inline constexpr uint32_t max_offsets_per_node = 16;
inline constexpr uint32_t max_offset_limit = 1u << 20;

struct Progress { uint32_t phase = CE_SCAN_ENUMERATING; uint64_t processed = 0, total = 0; };
using ProgressCallback = std::function<void(const Progress&)>;
using CancelCallback = std::function<bool()>;
// Re-resolves a candidate path and says whether it lands exactly on the target.
using VerifyCallback = std::function<bool(const CePointerScanResultV2&)>;

struct Outcome {
    std::vector<CePointerScanResultV2> results;
    bool truncated = false;
    uint64_t slots = 0;
};

inline bool contains(const std::vector<Range>& ranges, uint64_t address, uint64_t width) {
    if (!width) return false;
    size_t low = 0, high = ranges.size();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (ranges[middle].begin <= address) low = middle + 1;
        else high = middle;
    }
    if (!low) return false;
    const Range& range = ranges[low - 1];
    return address >= range.begin && address <= range.end && width <= range.end - address;
}

// A pointer slot and the address holding it. The address is canonical user-mode
// (< 2^47), so its top bit carries the "static" flag; keeping the flag in the slot is
// what holds the map at 16 bytes per entry, which at four million entries is the
// difference between 64 and 96 MiB of the host's memory.
struct Slot { uint64_t value = 0, address = 0; };
inline constexpr uint64_t slot_static = 1ull << 63;
inline uint64_t slot_address(const Slot& slot) noexcept { return slot.address & ~slot_static; }
inline bool slot_is_static(const Slot& slot) noexcept { return (slot.address & slot_static) != 0; }
// Comparator for std::upper_bound(map, high, ...): comp(value, element).
inline bool by_value(uint64_t left, const Slot& right) noexcept { return left < right.value; }

inline int harvest(const std::vector<Range>& ranges, const std::vector<Range>& modules,
    uint32_t alignment, std::vector<Slot>& map, bool& truncated,
    const CancelCallback& cancelled, const ProgressCallback& progress) {
    map.clear();
    // Reserve the whole budget up front: a growth reallocation would move the map and
    // invalidate the self-exclusion range below.
    map.reserve(max_slots);
    const Range self{reinterpret_cast<uintptr_t>(map.data()),
                     reinterpret_cast<uintptr_t>(map.data()) + map.capacity() * sizeof(Slot)};
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize ? system.dwPageSize : 4096;
    std::vector<uint8_t> buffer(page + sizeof(uint64_t));
    uint64_t processed = 0;
    uint64_t total = 0;
    for (const Range& range : ranges) total += range.end - range.begin;
    for (const Range& range : ranges) {
        uint64_t cursor = range.begin;
        while (cursor < range.end) {
            if (cancelled()) return CE_CANCELLED;
            const size_t wanted = static_cast<size_t>((std::min)(uint64_t(page), range.end - cursor));
            SIZE_T received = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(uintptr_t(cursor)),
                    buffer.data(), wanted, &received))
                received = 0;
            for (size_t offset = 0; offset + sizeof(uint64_t) <= received; offset += alignment) {
                uint64_t value = 0;
                std::memcpy(&value, buffer.data() + offset, sizeof(value));
                if (!value) continue;
                const uint64_t address = cursor + offset;
                if (!contains(ranges, value, sizeof(value))) continue;
                if (address >= self.begin && address < self.end) continue;
                if (map.size() >= max_slots) return CE_UNSUPPORTED;
                map.push_back({value, address | (contains(modules, address, 1) ? slot_static : 0)});
            }
            processed += received;
            progress({CE_SCAN_READING, processed, total});
            cursor += wanted;
        }
    }
    std::sort(map.begin(), map.end(), [](const Slot& left, const Slot& right) { return left.value < right.value; });
    (void)truncated;
    return CE_OK;
}

// One node of the backward walk. The path is carried with the node rather than stored as
// a graph: paths are at most CE_V2_MAX_POINTER_LEVELS long, and a DAG would cost far more.
struct Node {
    uint64_t address = 0;
    uint32_t count = 0;
    uint64_t visited[CE_V2_MAX_POINTER_LEVELS]{};   // addresses along the path, for NO_LOOP
    int64_t offsets[CE_V2_MAX_POINTER_LEVELS]{};    // discovery order: innermost first
};

inline int scan(const CePointerScanRequestV2& request, const std::vector<Range>& ranges,
    const std::vector<Range>& modules, const CancelCallback& cancelled,
    const ProgressCallback& progress, const VerifyCallback& verify, Outcome& outcome) {
    std::vector<Slot> map;
    if (const int status = harvest(ranges, modules, request.alignment, map, outcome.truncated, cancelled, progress))
        return status;
    outcome.slots = map.size();
    if (cancelled()) return CE_CANCELLED;

    const uint64_t reach = request.max_offset;
    const bool no_loop = (request.flags & CE_PTRSCAN_NO_LOOP) != 0;
    const bool static_only = (request.flags & CE_PTRSCAN_STATIC_ONLY) != 0;
    std::vector<CePointerScanResultV2> found;
    const auto emit = [&](const Node& node) {
        if (found.size() >= max_results) { outcome.truncated = true; return false; }
        CePointerScanResultV2 result{};
        result.size = sizeof(result);
        result.version = CE_V2_VERSION;
        result.base = node.address;
        result.level_count = node.count;
        // Discovery order is innermost first; execution order is outermost first.
        for (uint32_t i = 0; i < node.count; ++i) result.offsets[i] = node.offsets[node.count - 1 - i];
        found.push_back(result);
        return true;
    };

    std::vector<Node> frontier(1);
    frontier[0].address = request.target;
    for (uint32_t level = 1; level <= request.levels && !frontier.empty(); ++level) {
        std::vector<Node> next;
        for (const Node& node : frontier) {
            if (cancelled()) return CE_CANCELLED;
            const uint64_t low = node.address > reach ? node.address - reach : 0;
            const uint64_t high = node.address > UINT64_MAX - reach ? UINT64_MAX : node.address + reach;
            // One upper_bound, then walk the hits downwards. No per-offset search.
            auto it = std::upper_bound(map.begin(), map.end(), high, by_value);
            uint32_t hits = 0;
            while (it != map.begin()) {
                --it;
                if (it->value < low) break;
                const uint64_t slot = slot_address(*it);
                bool seen = false;
                for (uint32_t i = 0; i < node.count && !seen; ++i) seen = node.visited[i] == slot;
                if (no_loop && (seen || slot == node.address)) continue;
                if (++hits > max_offsets_per_node) { outcome.truncated = true; break; }
                Node child = node;
                child.address = slot;
                child.visited[child.count] = slot;
                child.offsets[child.count] = static_cast<int64_t>(node.address) - static_cast<int64_t>(it->value);
                ++child.count;
                // A static base ends the chain: report it and stop, so a deep scan still
                // returns the short paths it finds on the way. Intermediate hops need not
                // be static, so this only triggers on the OUTERMOST address.
                const bool is_static = slot_is_static(*it);
                if (static_only && is_static) { if (!emit(child)) break; }
                else if (level == request.levels) { if (!static_only && !emit(child)) break; }
                else next.push_back(child);
            }
        }
        if (next.size() > max_nodes_per_level) { next.resize(max_nodes_per_level); outcome.truncated = true; }
        frontier = std::move(next);
        progress({CE_SCAN_WRITING, level, request.levels});
    }

    // The harvest is not an atomic snapshot, so a path can be dangling by the time it is
    // reported. Re-resolving every candidate is milliseconds and turns "best effort" into
    // a postcondition: only paths that still land exactly on the target survive.
    outcome.results.clear();
    outcome.results.reserve(found.size());
    for (const CePointerScanResultV2& result : found) {
        if (cancelled()) return CE_CANCELLED;
        if (verify(result)) outcome.results.push_back(result);
    }
    return CE_OK;
}

} // namespace ce::pointerscan
