# Host-process read-only views (Regions / Modules / Threads)

[English](PROCESS_VIEWS.md) | [中文](PROCESS_VIEWS_CN.md)

Three read-only views of the hosting process — its memory regions, its loaded
modules and its threads — as tabs in the WebView page. They only enumerate and
display; nothing here mutates host state. This file covers the ABI they expose,
how the walks work, how the page drives them, and where they stop.

## Scope

The reference is CE's `formmemoryregionsunit.pas` (468 lines),
`frmEnumerateDLLsUnit.pas` (399) and `frmThreadlistunit.pas` (1194), of which
only "enumerate + display" is taken:

- The only host-mutating action in CE's region window is "set writable", and it
  is gated by the driver.
- CE's DLL window is 100% enumeration / display / navigation — no load, unload
  or inject.
- Roughly 60-65% of CE's thread window is suspend / resume / register edits /
  debug-register clears. None of it is done here.

Deliberately out of scope for this batch: changing memory protection, suspending
or resuming threads, setting thread priority, loading or unloading modules,
cross-process enumeration, symbols / PDB names, undocumented APIs such as
`NtQueryInformationThread`, and the region table's mapped file name column (CE's
`Extra` column, which needs `GetMappedFileName`).

Read-only is not left to discipline: the thread walk opens with
`OpenThread(THREAD_QUERY_LIMITED_INFORMATION)`, so suspending or terminating a
thread is impossible by access mask.

## ABI

```c
#define CE_V2_MAX_VIEW_ITEMS (1u << 20)
CE_API int CE_CALL CE_GetRegionsV2(CeRegionInfoV2* regions, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetModulesV2(CeModuleInfoV2* modules, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetThreadsV2(CeThreadInfoV2* threads, uint32_t capacity, uint32_t* required);
```

Fill-an-array rather than "count, then fetch by index": one call returns one
consistent snapshot, with no generation, cache or refresh handshake.

- `capacity` and `required` count **elements**. `(nullptr, 0)` plus a `required`
  pointer is the size query. A short buffer writes nothing at all and returns
  `CE_INVALID_ARGUMENT` with `required` set. A null buffer with nonzero
  capacity, and a missing `required`, are also `CE_INVALID_ARGUMENT`. Counts
  above `CE_V2_MAX_VIEW_ITEMS` are `CE_UNSUPPORTED`.
- The capacity convention matches `CE_GetResultV2` / `copy_text_v2`; only the
  unit changes, from bytes to elements.
- `size` and `version` are **outputs** — the implementation writes them into
  every filled element — the opposite of an input struct such as
  `CeScanRequestV2`. That is deliberate: a 5000-element array should not require
  the caller to pre-seed a version number per element.
- `CE_V2_VERSION` does not change: `valid_v2` requires an exact size and version
  match, so adding structures leaves the existing ones alone.

Structure sizes are pinned twice: by the ABI `static_assert`s in
`src/bridge/ui_bridge.cpp` (the same block pins `CeScanStatusV2` at 80 bytes) and
by runtime assertions in `runtime_v2_tests`.

| Struct | Size |
|---|---|
| `CeRegionInfoV2` | 48 |
| `CeModuleInfoV2` | 552 |
| `CeThreadInfoV2` | 168 |

`valid_mask` marks per-field validity, bit by bit. A clear bit means the field is
zero and carries no information — no Win32 sentinel values such as
`THREAD_PRIORITY_ERROR_RETURN` leak into the ABI.

| Bit | Field | Set when |
|---|---|---|
| 0 | `description` | `GetThreadDescription` succeeded and the name is non-empty |
| 1 | `priority` | `GetThreadPriority` did not return `THREAD_PRIORITY_ERROR_RETURN` |
| 2 | `created` | `GetThreadTimes` succeeded |

`priority` keeps `GetThreadPriority`'s signed -15..15 value in a `uint32_t`;
`created` is a raw FILETIME (100 ns units since 1601-01-01 UTC), not converted.

## Enumeration

`src/core/process_view.inc`, textually included by `src/core/core.cpp` after
`typed_core.inc` / `typed_table.inc`. An `.inc` rather than a new `.cpp`, so the
source lists in `build.cmd` and `src/shadowtrainer.vcxproj` stay untouched.

| View | Walk | Notes |
|---|---|---|
| Regions | `VirtualQuery` from `lpMinimumApplicationAddress` to `lpMaximumApplicationAddress` | loop shape follows `typed_scan.cpp`'s range walk, with a `next <= cursor` guard; `MEM_FREE` and `MEM_RESERVE` are included, as CE's State column includes them |
| Modules | `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ...)` | `TH32CS_SNAPMODULE32` is deliberately **not** added: it exists to show WOW64 modules to a 64-bit process, and the combination produces duplicate rows. The snapshot is retaken on the documented `ERROR_BAD_LENGTH` / `ERROR_PARTIAL_COPY` path, up to four attempts, because the host may be inside `LoadLibrary`. |
| Threads | `TH32CS_SNAPTHREAD`, filtered by `th32OwnerProcessID` | the current thread uses the `GetCurrentThread()` pseudo-handle, skipping Open/Close; a failing `GetThreadPriority` / `GetThreadTimes` / `GetThreadDescription` clears only its own bit instead of failing the whole call |

None of the three takes `state_mutex`: they read no Core state, and
`tick_freezes` needs the mutex every 80 ms — holding it across a whole region
walk would stall the freeze thread. The precedent is `format_value_v2`, which
only reads `stopped`.

## UI

The three tabs are navigation indices 3/4/5 (seven pages in all) and are rendered
by `web/js/views.js` from the `views` section the bridge pushes. A page is a hint
line, a table, a count label, Previous / Next page / Refresh. The label reads
`<Kind>: <n> total | <first> - <last> | 512/page`, or `<Kind>: not loaded`
before the first load.

**The bridge holds the snapshot**: the row arrays inside `Session` are that
snapshot, and paging only repaints the current slice. Re-enumerating on every
page turn would let rows move under the reader and multiply the walk cost by the
page count.

Refresh policy: `view.load` enumerates once, and only if that page has never been
loaded; after that, only an explicit `view.refresh` re-walks. A region walk is
one system call per region — 300-500 in a small process, tens of thousands in a
heavy host — so it must not run on every tab click. A Refresh issued while a scan
is running records a pending flag, and the existing 100 ms poll timer (bare id 1,
`src/ui/webui.cpp`) performs it once the bridge is idle: **no new timer**.

## Tests

- `runtime_v2_tests`: the three `sizeof`s; the count queries; regions tile the
  address space from `lpMinimumApplicationAddress` and cover the caller's stack
  address, with both `MEM_COMMIT` and `MEM_FREE` present; the module table
  contains the main exe; exactly one thread is flagged current; a short buffer
  writes nothing. A snapshot races a live host — regions are added between the
  size query and the fill — so `fill_view` retries (four attempts, re-reading the
  count each time) and the assertions rely only on invariants that hold under a
  live host (`tests/runtime_v2_tests.cpp`).
- `typed_tests`: the same checks through `ce::Core` directly, plus
  `CE_NOT_RUNNING` from all three after `shutdown()`.
- `bridge_tests`: `view.load` for all three kinds, row content (region columns,
  module path, thread current marker) and labels, in mode A (fake `CeApi`) and
  mode B (the real DLL) alike.

## Known boundaries

- Module paths are inline and fixed at 260 (`MAX_PATH`), truncated if longer;
  thread descriptions are fixed at 64.
- The region table includes Free and Reserve, so it has more rows than
  "committed regions"; a `state == MEM_FREE` row has `type` and
  `allocation_protect` set to 0, and `protect` set to `PAGE_NOACCESS`.
- Nothing polls, so the data is the state at the moment Refresh was pressed;
  threads and regions keep changing in a live host.
- `CreateToolhelp32Snapshot`'s call-site restriction is in
  `docs/COMPATIBILITY.md` — do not call it from `DllMain`.
