#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef _WIN32
#define CE_CALL __cdecl
#ifdef SHADOWTRAINER_BUILD
#define CE_API __declspec(dllexport)
#else
#define CE_API __declspec(dllimport)
#endif
#else
#define CE_CALL
#define CE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum CeStatus {
    CE_OK = 0, CE_INVALID_ARGUMENT = 1, CE_NOT_RUNNING = 2,
    CE_BUSY = 3, CE_CANCELLED = 4, CE_IO_ERROR = 5,
    CE_ACCESS_ERROR = 6, CE_UNSUPPORTED = 7, CE_INTERNAL_ERROR = 8
};
enum CeState { CE_STARTING = 0, CE_RUNNING = 1, CE_STOPPING = 2, CE_STOPPED = 3, CE_FAILED = 4 };
// Version 1: only signed, four-byte exact scans. Range is [begin,end).
// begin=end=0 scans committed readable regions in the current process.
// A nonzero range makes deterministic testing and small targeted scans possible.
typedef struct CeScanRequest {
    uint32_t size;
    uint32_t alignment; // 1 or 4
    uint64_t begin;
    uint64_t end;
    int32_t value;
    uint32_t reserved;
} CeScanRequest;
typedef struct CeResult {
    uint64_t address;
    int32_t value;
    uint32_t reserved;
} CeResult;

CE_API uint32_t CE_CALL CE_GetApiVersion(void);
CE_API uint32_t CE_CALL CE_GetState(void);
CE_API uint32_t CE_CALL CE_GetHostPid(void);
CE_API int CE_CALL CE_ShowWindow(void);
CE_API int CE_CALL CE_RequestStop(void);
// Wait for bootstrap/UI worker EXIT, not just a state flag, before FreeLibrary.
// Caller owns its LoadLibrary reference and must retain it through this call.
CE_API int CE_CALL CE_WaitStopped(uint32_t timeout_ms);
CE_API int CE_CALL CE_FirstScan(const CeScanRequest* request);
CE_API int CE_CALL CE_NextScan(int32_t value);
CE_API void CE_CALL CE_CancelScan(void);
CE_API int CE_CALL CE_ResultCount(uint64_t* count);
CE_API int CE_CALL CE_GetResult(uint64_t index, CeResult* result);
CE_API int CE_CALL CE_ReadInt32(uint64_t address, int32_t* value);
CE_API int CE_CALL CE_WriteInt32(uint64_t address, int32_t value);
CE_API int CE_CALL CE_SetFreeze(uint64_t address, int32_t value, int enabled);
CE_API int CE_CALL CE_SaveTable(const wchar_t* path);
CE_API int CE_CALL CE_LoadTable(const wchar_t* path);
// Copies a per-calling-thread diagnostic; returns required wchar_t capacity.
CE_API uint32_t CE_CALL CE_GetLastError(wchar_t* buffer, uint32_t capacity);

#ifdef __cplusplus
}
#endif
