# Feature parity matrix (phase 2)

[English](FEATURE_PARITY.md) | [中文](FEATURE_PARITY_CN.md)

Implementation status is read separately from passing tests; the final evidence is this machine's build and test run (see the evidence policy in [`STATUS.md`](STATUS.md)). The original CE source is never modified. The retired native UI's source, tests and probes are cold-stored under `baseline/pre-webui/` (archive only).

Status uses five values: **Implemented**, **Partial**, **Not implemented**, **Blocked**, **Dropped**. A Partial row is a deliberately bounded subset; the acceptance boundary states the bound.

## Host and session

Bootstrap, window lifecycle and the one global hotkey.

| Capability | Status | Acceptance boundary |
|---|---|---|
| Automatic DLL UI, default host, stop and unload | Implemented | Self-built host loaded with a normal LoadLibrary; carried over from the previous phase and re-verified here. It does not stand in for a real injector or the CE Tutorial. |
| Backtick menu toggle | Implemented | Foreground PID, release-armed, 16 ms sampling, still toggles after the window is hidden; the FSM and the shared window-toggle path are tested. |

## Scanning

Value scans, their results, and the pointer scanner.

| Capability | Status | Acceptance boundary |
|---|---|---|
| Integer 1/2/4/8, signed/unsigned, hex | Implemented | Integers do not pass through double; boundary and overflow tests. |
| Float/Double | Implemented | NaN/Infinity and raw-byte change rules; `CeScanRequestV2.rounding` provides Rounded/Extreme/Truncated and still defaults to this build's original Exact; applies only to exact Float/Double. |
| UTF-8 / UTF-16 / AOB | Implemented | 65536-byte ceiling, full- and half-byte masks, cross-page overlap tests. |
| Unknown, between, greater, less, changed, increased, decreased, increased/decreased by | Implemented | Applies to numeric types; text and AOB accept exact, changed and unchanged only. |
| File-backed results, paging, cancel, single-level undo | Implemented | Metadata and history commit as one unit; a mismatched generation is refused so pages cannot mix generations; not the original CE result-file format. |
| Pointer scan (single reverse lookup) | Partial | Up to 8 levels, alignment 1/2/4/8, max offset at most 0x100000, optional static-module filter that emits a node inside a loaded module and stops descending. Four caps (4M collected slots, 16 distinct offsets per node, 200k nodes per level, 65536 results) set `truncated` and still return the results found; every candidate is re-resolved before it is listed. No result rescan or table reuse, no heap-block filter, no pointer graph; the UI always enables NO_LOOP. |

## Records and memory

The address list, pointer-chain resolution, and the Memory preview page.

| Capability | Status | Acceptance boundary |
|---|---|---|
| Typed read/write, freeze, address-list CRUD | Implemented | Stable IDs; 80 ms freeze tick; overlapping enabled freezes are refused; a failed freeze write disables the freeze. |
| Pointer chains, module + offset | Partial | Basic resolution: up to 16 levels, host pointer width, re-resolved on every freeze. This is record resolution, not the pointer scan listed above. |
| Address expressions | Partial | CE's numeric/module subset: multi-token `+ - *`, `--` treated as `+`, quoted module names, overflow and address-space checks. No symbols, `[]` dereference, `(TYPE)` casts, parentheses or division. |
| Hex/ASCII preview | Implemented | 256-byte page; unreadable cells are marked and cannot be edited; not a disassembler. |
| Byte editing in the hex view | Implemented | Clicking the hex or ASCII column selects a whole cell (not an individual nibble); the keyboard can move and type without a click first. Nibbles are entered one at a time with the staged digits visible, and the cell is written only once full; an unreadable cell cannot be edited. |
| Display types | Implemented | Ten modes (Byte / 2 / 4 / 8 bytes in hex and decimal, plus Float and Double); cell width and the ASCII column follow the mode. A hex cell is a byte sequence in address order (`CE_WriteBytesV2`); decimal and float cells are literals (`CE_WriteValueV2`, Enter commits, Esc cancels). |
| Hex-view undo | Implemented | Ring of at most 256 entries holding the bytes before and after each write; button or Ctrl+Z. Bytes the host changed after the write are still restored, and the status line says so. |
| Hex-view fill | Implemented | Drag a byte range on the grid (or Shift-click); bytes are interpreted per byte; an empty selection is refused; the whole range must be readable; one write records one undo entry. |
| Back/Forward page history | Implemented | Stores the resolved base plus the page; entries identical to the current page are dropped; the address and offset fields are not rewritten; this is page history, not expression history. |

## Process views

Regions, Modules and Threads for the hosting process. All three are read-only.

| Capability | Status | Acceptance boundary |
|---|---|---|
| Memory region list | Implemented | Read-only. Tiles the whole application address range contiguously, including MEM_FREE; base/size/state/protect/allocation-protect/type; protections are never changed. |
| Module/DLL list | Implemented | Toolhelp32 snapshot; base/size/name/path (the path is a fixed 260-character copy and truncates). No load or inject; one selected row can be unloaded (`CE_UnloadModuleV2`), which refuses the host executable and this DLL and hands everything else to the loader. |
| Thread list | Implemented | Read-only. TID/priority/creation time/name/current flag; threads are opened with `THREAD_QUERY_LIMITED_INFORMATION` only, so they **cannot** be suspended or resumed. |

## Cheat tables and ABI

CT import/export and the public C ABI.

| Capability | Status | Acceptance boundary |
|---|---|---|
| Extended CT numeric/text/AOB/offsets | Partial | Strict subset: CodePage=1 and ZeroTerminate=1 are refused; import never auto-activates records. |
| External CT tolerant import | Implemented | Unknown entry children and top-level siblings are preserved and written back verbatim; an unknown VariableType imports as a read-only opaque record that refuses write/freeze/edit but can be deleted; preserved subtrees are capped at 4 MiB; re-serialization is semantically equivalent, not byte-identical. |
| V1 CT path | Implemented | Unchanged: still a strict signed32 subset; a script, offsets or an unknown field rejects the whole table. |
| V1 interface | Implemented | Retained: the 17 original exports at version 1; a session it cannot represent is refused, never implicitly truncated. |
| V2 interface | Implemented | Added: explicit struct versions and caller buffers; shares the session lifecycle. |

## Build and verification

What this machine builds and what evidence exists.

| Capability | Status | Acceptance boundary |
|---|---|---|
| x64 | Implemented | x64-only build and test; x86 was retired on 2026-09-30 and only `dist/x64/shadowtrainer.dll` is produced. |
| CE Tutorial / reference-runtime differential | Blocked | No configured Lazarus/FPC toolchain or Tutorial EXE is available, so the differential run has not started. |

## Not implemented or explicitly out of scope

Kept visible so the matrix stays honest; none of these is stubbed.

| Capability | Status | Acceptance boundary |
|---|---|---|
| find/replace, per-byte coloring | Not implemented | Kept as a future target. |
| Grouped scan, script-defined custom scan types | Not implemented | Kept as a future target; CE's float rounding modes are now covered. |
| Original result import/export, multiple scan tabs, full pointer scan, pointer graph | Not implemented | These local capabilities have not been approved for removal. |
| Symbols, structures, disassembly, assembly | Not implemented | The original full-feature goal is retained. |
| User-mode debugging, access/write breakpoints, tracing | Not implemented | This build does not pretend its own debugging is supported. |
| Auto Assembler, Lua CE API and object system | Not implemented | Needs its own semantic migration and acceptance work. |
| Full CT history, LCL, trainer, all hotkeys | Not implemented | Menu shortcuts are not the full hotkey system. |
| Plugin SDK 6, Mono/CLR/Java, speedhack/D3D | Not implemented | The original user-mode goal is retained. |
| Local external-process selection, cross-bitness | Not implemented | Distinct from the excluded remote-service item; the three process views act only on the host process itself. |
| Modifying host state (memory protection, thread suspend/resume, module load) | Not implemented | Explicitly excluded from this batch; the thread list stays read-only by access mask rather than by discipline. Module **unload** is the one exception and lives on the module list. |
| Driver / DBVM / remote-service exclusive capabilities | Dropped | Explicitly dropped by the user; not part of the release. |

Completing this phase does not mean the refactor is complete; no unimplemented capability is simulated with empty functions or always-succeeding stubs.
