#pragma once
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

enum CeValueTypeV2 {
    CE_TYPE_U8 = 0, CE_TYPE_U16 = 1, CE_TYPE_U32 = 2, CE_TYPE_U64 = 3,
    CE_TYPE_FLOAT = 4, CE_TYPE_DOUBLE = 5,
    CE_TYPE_UTF8 = 6, CE_TYPE_UTF16 = 7, CE_TYPE_AOB = 8
};
enum CeValueFlagsV2 { CE_VALUE_SIGNED = 1, CE_VALUE_HEX = 2 };
enum CeCompareV2 {
    CE_CMP_EXACT = 0, CE_CMP_UNKNOWN = 1, CE_CMP_GREATER = 2,
    CE_CMP_LESS = 3, CE_CMP_BETWEEN = 4, CE_CMP_CHANGED = 5,
    CE_CMP_UNCHANGED = 6, CE_CMP_INCREASED = 7, CE_CMP_DECREASED = 8,
    CE_CMP_INCREASED_BY = 9, CE_CMP_DECREASED_BY = 10
};
#define CE_V2_VERSION 2u
#define CE_V2_MAX_VALUE_BYTES 65536u
#define CE_V2_MAX_OFFSETS 16u
// Hard ceiling for the process-view enumerations below, so a pathological host
// fails cleanly instead of forcing a huge allocation.
#define CE_V2_MAX_VIEW_ITEMS (1u << 20)

// All pointer inputs are borrowed for the duration of the synchronous call only.
// Text input uses Windows UTF-16. UTF8 scan values are converted without a NUL.
// byte_length=0 infers the size on FIRST exact scan or typed write.
// Next scans cannot change type/flags/width/alignment; zero length/alignment inherit.
// First ranges are [begin,end); next refines the committed candidates (range ignored).
// Rounding applies only to CE_CMP_EXACT on Float/Double; a nonzero rounding on any
// other comparison or type is refused with CE_INVALID_ARGUMENT. The values run one
// higher than CE's TRoundingType because 0 has to keep meaning this build's
// existing exact comparison.
enum CeRoundingV2 {
    CE_ROUND_EXACT = 0, CE_ROUND_ROUNDED = 1, CE_ROUND_EXTREME = 2, CE_ROUND_TRUNCATED = 3
};
typedef struct CeScanRequestV2 {
    uint32_t size, version, type, flags;
    uint32_t comparison, alignment, byte_length, rounding;
    uint64_t begin, end;
    const wchar_t* value;
    const wchar_t* value2;
} CeScanRequestV2;
typedef struct CeScanInfoV2 {
    uint32_t size, version, type, flags;
    uint32_t byte_length, alignment, reserved1, reserved2;
    uint64_t generation, count;
} CeScanInfoV2;
typedef struct CeResultV2 {
    uint32_t size, version, type, flags;
    uint32_t byte_length, reserved;
    uint64_t generation, address;
} CeResultV2;
// Resolve base, then for each offset: address = read_host_pointer(address)+offset.
// Offset order is execution order; count=0 means absolute address.
typedef struct CeAddressV2 {
    uint64_t base;
    uint32_t offset_count, reserved;
    int64_t offsets[CE_V2_MAX_OFFSETS];
} CeAddressV2;
typedef struct CeRecordRequestV2 {
    uint32_t size, version, type, flags;
    uint32_t byte_length, reserved;
    uint64_t id; // zero adds a new record; nonzero edits an existing record
    CeAddressV2 address;
    const wchar_t* description;
    const wchar_t* value; // null snapshots current memory; otherwise parse only (no write)
} CeRecordRequestV2;
typedef struct CeRecordInfoV2 {
    uint32_t size, version, type, flags;
    uint32_t byte_length, frozen, last_status, reserved;
    uint64_t id, resolved_address;
    CeAddressV2 address;
} CeRecordInfoV2;

// Read-only views of the hosting process. size/version are OUTPUT ONLY: the
// implementation writes them into every filled element, so callers never pre-seed
// an array. capacity and required count ELEMENTS, not bytes. A null pointer with
// zero capacity is a legal size query; a short buffer writes nothing at all and
// returns CE_INVALID_ARGUMENT with required set. Counts above
// CE_V2_MAX_VIEW_ITEMS are CE_UNSUPPORTED.
// state/type/protects are the raw MEM_* values; MEM_FREE regions are included.
typedef struct CeRegionInfoV2 {
    uint32_t size, version, state, type;
    uint32_t allocation_protect, protect, reserved1, reserved2;
    uint64_t base, region_size;
} CeRegionInfoV2;
// path is an inline, possibly truncated copy of the module's file name.
typedef struct CeModuleInfoV2 {
    uint32_t size, version, reserved1, reserved2;
    uint64_t base, module_size;
    wchar_t path[260];
} CeModuleInfoV2;
// valid_mask bits: 0 description holds a name, 1 priority, 2 created. A clear bit
// means the field is zero and carries no information; no Win32 sentinel values are
// exposed. priority keeps GetThreadPriority's signed -15..15 value in a uint32_t.
// created is a FILETIME (100ns units since 1601-01-01 UTC), not converted.
typedef struct CeThreadInfoV2 {
    uint32_t size, version, current, priority, valid_mask, reserved1;
    uint64_t thread_id, created;
    wchar_t description[64];
} CeThreadInfoV2;
CE_API int CE_CALL CE_GetRegionsV2(CeRegionInfoV2* regions, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetModulesV2(CeModuleInfoV2* modules, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetThreadsV2(CeThreadInfoV2* threads, uint32_t capacity, uint32_t* required);
// Unloads one module of the host process, by the base address a module
// enumeration reported. The host executable and this DLL are refused; anything
// else is handed to the loader, which may still refuse it, and may unmap code
// another thread is running. Additive to V2.
CE_API int CE_CALL CE_UnloadModuleV2(uint64_t base);

// Pointer scan: find multi-level paths from a static base to a target address. The
// offsets in a result are in EXECUTION order (outermost first), exactly what
// CeAddressV2 expects, so a result can be stored as a record or resolved directly.
#define CE_V2_MAX_POINTER_LEVELS 8u
enum CePointerScanFlagsV2 {
    // Stop at any node whose address lies inside a loaded module and emit it as a
    // result instead of descending further; intermediate hops need not be static.
    CE_PTRSCAN_STATIC_ONLY = 1,
    CE_PTRSCAN_NO_LOOP = 2
};
typedef struct CePointerScanRequestV2 {
    uint32_t size, version, levels, alignment;   // levels 1..8; alignment 1, 2, 4 or 8
    uint32_t max_offset, flags, reserved1, reserved2;
    uint64_t target;
} CePointerScanRequestV2;
typedef struct CePointerScanResultV2 {
    uint32_t size, version, level_count, reserved;
    uint64_t base;
    int64_t offsets[CE_V2_MAX_POINTER_LEVELS];
} CePointerScanResultV2;
// Its own telemetry rather than reusing CeScanStatusV2: a pointer scan must not look
// like a value scan to the UI, which would freeze the address list for its duration
// and render its phases on the Scan page.
// truncated means the caps stopped the walk early; the results are still valid.
typedef struct CePointerScanStatusV2 {
    uint32_t size, version, phase, active;
    uint32_t cancel_requested, truncated, reserved1, reserved2;
    uint64_t operation_id, generation, processed, total;
} CePointerScanStatusV2;
// Synchronous; the caller owns the worker thread. Shares the single-job admission and
// the cancellation epoch with the value scan, so only one of the two can run at a time.
CE_API int CE_CALL CE_PointerScanV2(const CePointerScanRequestV2* request);
CE_API int CE_CALL CE_CancelPointerScanV2(void);
CE_API int CE_CALL CE_GetPointerScanStatusV2(CePointerScanStatusV2* status);
// generation must match the published result set; mismatch returns CE_BUSY.
CE_API int CE_CALL CE_GetPointerScanResultV2(uint64_t generation, uint64_t index, CePointerScanResultV2* result);

enum CeScanPhaseV2 {
    CE_SCAN_IDLE = 0, CE_SCAN_ENUMERATING = 1, CE_SCAN_READING = 2,
    CE_SCAN_WRITING = 3, CE_SCAN_COMMITTING = 4, CE_SCAN_COMPLETED = 5,
    CE_SCAN_CANCELLED = 6, CE_SCAN_FAILED = 7
};
enum CeScanUnitV2 { CE_SCAN_UNIT_NONE = 0, CE_SCAN_UNIT_BYTES = 1, CE_SCAN_UNIT_CANDIDATES = 2 };
// Separate additive telemetry: existing CeScanInfoV2 layout is unchanged.
// status may return CE_BUSY instead of blocking behind commit/I/O; retry later.
// processed/total share unit; totals unknown while enumerating. Complete means
// data and metadata committed, not merely reading 100% of the source.
typedef struct CeScanStatusV2 {
    uint32_t size, version, phase, active;
    uint32_t cancel_requested, has_scan, can_undo, unit;
    uint32_t total_known, last_status, reserved1, reserved2;
    uint64_t operation_id, generation, processed, total;
} CeScanStatusV2;
CE_API int CE_CALL CE_GetScanStatusV2(CeScanStatusV2* status);

// V1 remains version 1. Query this additive API separately.
CE_API uint32_t CE_CALL CE_GetApiVersionV2(void);
CE_API int CE_CALL CE_FirstScanV2(const CeScanRequestV2* request);
CE_API int CE_CALL CE_NextScanV2(const CeScanRequestV2* request);
CE_API int CE_CALL CE_GetScanInfoV2(CeScanInfoV2* info);
// generation must match the published scan; mismatch returns CE_BUSY.
// capacity/required are bytes; a null zero-capacity buffer queries size.
CE_API int CE_CALL CE_GetResultV2(uint64_t generation, uint64_t index, CeResultV2* info,
                                 void* bytes, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_NewScanV2(void);
CE_API int CE_CALL CE_UndoScanV2(void);
CE_API int CE_CALL CE_ReadBytesV2(uint64_t address, void* bytes, uint32_t length);
// Raw byte write, symmetric with CE_ReadBytesV2: no parsing, so any bit pattern is
// stored exactly as given. Requires currently writable host memory.
CE_API int CE_CALL CE_WriteBytesV2(uint64_t address, const void* bytes, uint32_t length);
CE_API int CE_CALL CE_WriteValueV2(uint64_t address, uint32_t type, uint32_t flags,
                                  uint32_t byte_length, const wchar_t* value);
// Formatting required/capacity count wchar_t INCLUDING the terminator.
CE_API int CE_CALL CE_FormatValueV2(uint32_t type, uint32_t flags, const void* bytes,
                                   uint32_t length, wchar_t* text, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_ResolveAddressV2(const wchar_t* expression, uint64_t* address);
CE_API int CE_CALL CE_ResolvePointerV2(const CeAddressV2* address, uint64_t* resolved);
CE_API int CE_CALL CE_UpsertRecordV2(const CeRecordRequestV2* record, uint64_t* id);
CE_API int CE_CALL CE_RecordCountV2(uint64_t* count);
CE_API int CE_CALL CE_GetRecordV2(uint64_t index, CeRecordInfoV2* info,
                                 wchar_t* description, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_RemoveRecordV2(uint64_t id);
CE_API int CE_CALL CE_WriteRecordV2(uint64_t id, const wchar_t* value);
CE_API int CE_CALL CE_FreezeRecordV2(uint64_t id, int enabled);
CE_API int CE_CALL CE_SaveTableV2(const wchar_t* path);
CE_API int CE_CALL CE_LoadTableV2(const wchar_t* path);
// Post a private UI toggle; CE_ShowWindow retains its V1 show-only semantics.
CE_API int CE_CALL CE_ToggleWindowV2(void);

#ifdef __cplusplus
}
#endif
