# Compatibility and Runtime Conventions

[English](COMPATIBILITY.md) | [中文](COMPATIBILITY_CN.md)

What an external caller can rely on, and what each entry point refuses. The ABI
is `include/ce/api.h` (version 1) and `include/ce/api_v2.h` (version 2); the
rules below are the ones the code and the tests hold to.

## DLL, loading and the hotkey

One x64 `shadowtrainer.dll`. It imports only Windows system libraries — the WebView2 loader is
linked statically, because a side-by-side `WebView2Loader.dll` cannot be relied
on inside someone else's process.

- Load it by any normal mechanism — `LoadLibraryW`, a debugger, an injector.
- The caller owns its load reference and must keep it until `CE_RequestStop` +
  `CE_WaitStopped` have succeeded. Stopping unloads the module, so a later
  session means loading it again.
- Hiding the window never stops a scan or a freeze.

**The hotkey.** Backtick (`VK_OEM_3`), sampled every 16 ms from the key's high
bit, triggering on a press edge. It only takes effect when the foreground
window belongs to the host process and no modal dialog is up; after startup or
after losing eligibility, an eligible release must be observed before a press
triggers. Sampling continues while the window is hidden.

There is no keyboard hook: host input is not intercepted or exclusively
grabbed, and the sampler does not take the foreground on its own. Two
consequences follow from sampling: a very short tap can fall between samples,
and the key's character depends on the keyboard layout.

## API versions

- V1 (`include/ce/api.h`, `CE_GetApiVersion == 1`) keeps its structures, its
  exports and its version number.
- V2 (`include/ce/api_v2.h`, `CE_GetApiVersionV2 == 2`) is additive. It adds
  explicit struct versions, caller-provided buffers and the extended types.
- A V1 result or next-scan call accepts only a compatible signed-32-bit
  session; a session holding a different type is refused with `CE_UNSUPPORTED`
  rather than truncated.
- V2 requests validate `size`/`version` before anything else. Output goes into
  a caller-provided buffer, with `required` reporting the capacity needed.
- A V2 result/pointer-result read carries a generation, and a mismatch returns
  `CE_BUSY`, so a paged read can never straddle a commit.
- The process read-only views use a different, fill-an-array convention; see
  [Process views](#process-views-read-only).

## Types and comparisons

### Value types

| `CeValueTypeV2` | Width | Notes |
|---|---|---|
| `CE_TYPE_U8` / `U16` / `U32` / `U64` | 1 / 2 / 4 / 8 | Little-endian, signed or unsigned via `CE_VALUE_SIGNED`, decimal or fixed-width hex via `CE_VALUE_HEX` |
| `CE_TYPE_FLOAT` / `CE_TYPE_DOUBLE` | 4 / 8 | Parsed and rounded as described below |
| `CE_TYPE_UTF8` | explicit | Raw UTF-8 bytes; typed spaces are kept |
| `CE_TYPE_UTF16` | explicit, even | UTF-16LE code units |
| `CE_TYPE_AOB` | explicit | Hex byte pairs, with `??`/`A?` wildcards in scan patterns |

`uint64` never passes through `double`. Float/Double text is parsed with a
fixed C numeric locale; the host's locale is not modified.

### Comparison semantics

- Numeric exact/greater/less/between compare numerically, and between includes
  both bounds.
- NaN matches no numeric comparison; positive and negative zero are exact-equal.
- changed/unchanged compare raw bytes, so an unchanged NaN bit pattern stays
  unchanged while a ±0 flip counts as a change.
- Infinity is a valid exact operand. A delta must be finite, and integer deltas
  are overflow-checked.
- Grouped scans and Lua/Auto Assembler custom scan types are still not
  implemented.

### Text and AOB

- UTF-8/UTF-16LE search values carry no implicit NUL, and typed spaces are
  preserved.
- AOB patterns accept full- and half-byte wildcards; writes and freezes do not.
- Text and AOB support exact and, on next scan, changed/unchanged only, and the
  width must stay the same.
- The largest single value is 65536 bytes; anything larger is an explicit
  error.

### Ranges, alignment and the first/next schema

- First-scan ranges are `[begin, end)`; `begin == end == 0` scans the committed
  readable regions of the host. A nonzero range makes deterministic testing and
  small targeted scans possible.
- Alignment is a power of two from 1 to 65536 (the UI offers 1/2/4/8) and is
  absolute: a variable that is not aligned to it never matches. A `uint64` on a
  32-bit stack is not necessarily 8-byte aligned.
- On next scan, `alignment == 0` and `byte_length == 0` inherit the session;
  any other change to type, flags, width or alignment is refused — that needs a
  new first scan.
- A copy of a first-scan request can be passed to next scan to keep its
  `begin`/`end`, but those fields do not redefine the committed candidate set.
- The first scan with unknown value streams addresses and historical raw values
  to a file.

## Floating-point rounding

`CeScanRequestV2.rounding` offers the reference implementation's three exact
float roundings plus this build's own Exact:

| Value | Name | CE equivalent | Meaning |
|---|---|---|---|
| 0 | `CE_ROUND_EXACT` | none | Default: bit-for-bit numeric equality. Existing behaviour, and old requests that pass 0, stay on this. |
| 1 | `CE_ROUND_ROUNDED` | `rtRounded` (0) | `RoundTo(mem, -N) == entered`, i.e. round-half-to-even at N decimals |
| 2 | `CE_ROUND_EXTREME` | `rtExtremerounded` (1) | The interval `entered ± 10^-N`. **CE is inconsistent here**: the single form uses a closed interval, the double form an open one, and this build copies that. |
| 3 | `CE_ROUND_TRUNCATED` | `rtTruncated` (2) | `entered <= mem < entered + 10^-N` |

The enum values are deliberately one above CE's `TRoundingType`, because 0 has
to keep meaning this build's existing exact behaviour. The struct size and
offsets are unchanged, and so is the `size`/`version` validation.

### Where N comes from

N is the number of digits the caller typed after the decimal separator. A
literal containing `E`/`e` always gives 0 (CE's floataccuracy rule), and inf/nan
do too. N is capped at 19; more is an error.

### Precision floors

These are limits of floating point, not implementation choices:

- In the interval modes, when `entered ± 10^-N` is no longer distinguishable in
  `double` (a large magnitude with high precision), the interval degenerates
  into an exact comparison.
- `Rounded` cannot match when `|value| × 10^N >= 2^63`.

### Scope

Rounding applies to `CE_CMP_EXACT` on Float/Double only. Integers, text, AOB,
between, changed/unchanged, increased/decreased and the delta comparisons
ignore it completely, and passing a nonzero rounding with any other type or
comparison is rejected. Rounding is not part of the next-scan immutable set, so
it can be switched between scans.

The implementation never touches the FPU rounding mode (the host can change
it); half-to-even is explicit.

## Scan lifecycle and results

### Commit and cancel

Every successful scan commits data, type, width, history and generation
together. A cancelled or failed scan leaves the previous commit in place, and
cancellation never overwrites the previous successful result.

### Undo and New scan

One level of undo restores the previous data and history. New scan clears the
scan but does not clear the address list.

### Broad-scan exclusions

A broad scan (`begin == end == 0`) excludes this round's known working buffers,
the pattern and the scanning thread's stack. It is not an atomic snapshot of the
whole host and does not isolate all DLL memory. Concurrent modification and
freed or reused pages therefore affect results, and counts are not directly
comparable with an out-of-process CE run.

### Result storage and the result list

Results live in a file with no arbitrary count cap; the real limits are disk
space and 64-bit file offsets. The result list shows 512 rows per page; a value
longer than 256 bytes gets a limited preview and is read in full only when it is
selected or activated.

## Records, pointers and writes

### Records and freezes

- Records carry stable IDs and can hold the same address with different types.
- Conflicting overlapping freezes are refused.
- A freeze attempts a write every 80 ms; a failure disables it.
- Periodic freezing is not a synchronisation primitive, and unfreezing does not
  restore a historical value.

### Pointer chains

A pointer chain executes `read_pointer(address) + signed_offset` per step, in
the order the API/UI supplies, up to 16 levels; the host's pointer width decides
the dereference width. A freeze re-resolves the chain every time, so it follows
a moved base.

### Address expressions

The evaluator is the numeric/module subset of CE's
`symbolhandler.getAddressFromName`:

- `+ - *` over multiple tokens; `*` binds first, while `+`/`-` run strictly
  left to right; `--` is treated as `+`.
- Every literal is hexadecimal (optionally with a `0x` or `$` prefix). A name
  is tried as hex first, then as a loaded module name; a name containing `+`,
  `-` or a space must be quoted, as in `"name.exe"+10`.
- Every add, subtract and multiply is overflow-checked, and the result must land
  in the host's user-mode address space.
- Symbols, `[...]` dereference, `(TYPE)` casts, parentheses and division are not
  supported, so it is still not a symbol engine. Pointer paths come from the
  separate pointer scanner instead.
- The old "split at the last `+`/`-`" behaviour is gone: multi-token
  expressions such as `"mod"+10-8` now parse.

### Writing to host memory

Writes touch normal writable memory only; protection is never bypassed, and
multi-byte writes are not guaranteed to be atomic.

## Memory preview (hex view)

### Display modes and geometry

The Memory page's Display type combo has ten modes: byte, 2, 4 and 8 bytes in
hexadecimal and decimal, plus Float and Double. Cell width and the ASCII column
position change with the mode.

| Mode | Bytes per cell | Cell width | Cells per row | ASCII column | Row width |
|---|---|---|---|---|---|
| Byte (hex) | 1 | 3 | 16 | 69 | 85 |
| Byte (dec) | 1 | 5 | 16 | 101 | 117 |
| 2 Bytes (hex/dec) | 2 | 5 / 7 | 8 | 61 / 77 | 77 / 93 |
| 4 Bytes (hex/dec) | 4 | 9 / 12 | 4 | 57 / 69 | 73 / 85 |
| 8 Bytes (hex/dec) | 8 | 17 / 21 | 2 | 55 / 63 | 71 / 79 |
| Float | 4 | 16 | 4 | 85 | 101 |
| Double | 8 | 25 | 2 | 71 | 87 |

The byte-hexadecimal mode keeps its original format of row width 85 and ASCII
column 69.

The page is 256 bytes. Unreadable cells are marked and cannot be edited, and
the preview is not a disassembler.

### Hex cells are bytes in address order

A hexadecimal cell is a sequence of bytes in address order, not a number:
typing `12345678` into a 4-byte cell leaves `12 34 56 78` in memory, whereas
parsing it as an integer and storing it little-endian would leave
`78 56 34 12`. Hex cells therefore write raw bytes through `CE_WriteBytesV2`;
only decimal and float cells parse a literal, through `CE_WriteValueV2`.

The geometry mapping and value formatting live in `web/js/hexview.js`; grid
rendering and input live in `web/js/memory.js`.

### Staging and commit

- A hex cell's digits go into a staging buffer first and are written once 2N
  digits have accumulated. **The staged content is drawn in the cell.** This is
  a correction: a half-typed digit used to be invisible, and because every
  keystroke re-read memory, the cell's first digit was effectively dropped.
- Decimal and float cells have variable width and instead commit on Enter and
  cancel on Escape; an uncommitted literal is neither written nor shown as a
  formatted value.
- An unreadable cell — any byte of it failing to read — cannot be edited,
  byte-for-byte in byte modes. This corrects the earlier behaviour where one
  `??` made all 16 bytes of a row uneditable.
- A failed write does not change the page text.

### Cursor

The cursor can be placed with a click, or moved with the keyboard and typed
into without clicking first. Paging, changing the display type and re-Resolving
all clear it, so an edit never continues at the same offset on a new page.

### Undo

- Every successful write (hex cell, fill) records
  `{address, width, before bytes, after bytes}`.
- At most 256 entries are kept, oldest dropped first; the button or Ctrl+Z
  restores the newest one.
- The host may have changed those bytes in the meantime. This build **compares,
  then restores anyway**, and says in the status line that the host changed
  those bytes, because refusing the undo would make it look broken.
- The restore goes through `CE_WriteBytesV2` (raw bytes, no parsing).
- Navigation does not clear the undo stack.

### Fill

Dragging a byte range on the grid (or Shift-clicking to extend it) interprets
the fill byte per byte and writes it with one `CE_WriteBytesV2`, recorded as
**one** undo entry. An empty selection is refused rather than silently ignored,
and the whole range must be readable: a fill that cannot be undone is worse
than a refusal.

### Page history

Back / Forward move between visited pages. A history entry stores
`{resolved base, page address}` — the label shows "Resolved base", and storing
only the page would show a new expression's base when jumping back. Entries
identical to the current page are deduplicated. Neither button rewrites the
address and offset inputs (the same contract as 256-byte paging). This is
**page** history, not the expression history of a CE address-box dropdown.

### Not provided

No find/replace and no per-byte colouring. The mouse can drag out a byte range
for filling, but a click does not land on a specific nibble (the web build uses
a whole-cell cursor instead).

## Cheat tables (CT)

### V2 (typed) tables

V2 saves and loads the extended types. It supports the implemented CE v45
record fields, keeps the ID and description, and covers numeric values and byte
arrays. Strings require an explicit `Length`/`Unicode`/`CodePage`; `CodePage=1`
is refused, `ZeroTerminate=1` is refused, and export writes `ZeroTerminate=0`.

### Field rules

The original implementation is the reference for the encoding:

- `MemoryRecordUnit.pas` gives String `Length` a capacity of
  `Unicode ? Length*2 : Length`; UTF-8 goes through Unicode 0 / CodePage 0 as
  raw bytes, and UTF-16 Length counts code units; ByteArray `ByteLength` is
  bytes.
- CE's XML stores Offsets leaf-to-root and executes them in reverse, so this
  project's order is the reverse of the API's execution order.
- A missing `ShowAsSigned` inherits a global setting in CE; this build defaults
  to signed and preserves the omission until an explicit edit, while an
  explicit 0/1 is honoured.

Import never writes or freezes. The record list is swapped only after every
field validates, and a record-revision conflict is detected.

### Preserving foreign content

Content this build does not recognise no longer rejects the whole table: it is
kept and written back verbatim on export.

- Unrecognised child elements inside `<CheatEntry>` (`AssemblerScript`,
  `GroupHeader`, `Hotkeys`, `Options`, `LastState`, ...) are kept in document
  order; top-level siblings of `<CheatEntries>` (`UserdefinedSymbols`,
  `Structures`, `Forms`, `LuaScript`, ...) are kept as well.
- When `<VariableType>` is missing or outside what this build can represent
  (`Auto Assembler Script`, `Pointer`, `Grouped`, `Binary`, custom types), the
  whole record imports as an **opaque record**: the address list shows
  `CT opaque (not representable)`, the value column shows an opaque hint, and
  every write/freeze/edit returns `CE_UNSUPPORTED` — but it can be deleted, and
  export writes it back verbatim. The marker is the combination
  `(last_status == CE_UNSUPPORTED && byte_length == 0)`; a normal record cannot
  have width 0, so the pair is unambiguous.
- Preserved content is capped at 4 MiB per node within the same 8 MiB table
  limit. Exceeding a cap is an explicit failure, never truncation.

### Re-serialization

Export is **semantically equivalent, not byte-identical**: attribute order is
sorted, empty elements are written `<X></X>` or `<X />`, the structure
indentation is re-laid-out, and comments/CDATA in the source are not preserved
(a CT containing comments is still rejected wholesale). Text inside a preserved
subtree is written verbatim (including newlines and `&`/`<`/`>` escaping), so a
script body is never re-indented or re-wrapped.

### V1 tables

The V1 path (`CE_SaveTable`/`CE_LoadTable`) gets none of the above and remains
the strict signed-32-bit subset: a script, offsets, a structure or any unknown
field rejects the whole table.

### Limits

8 MiB per file, 10000 records, 4096 description characters. These are table
limits, not scan-count limits. The exact acceptable fields are defined by the
source and the tests; arbitrary old CT files are not claimed to be universally
compatible.

## Process views

`CE_GetRegionsV2` / `CE_GetModulesV2` / `CE_GetThreadsV2` each return one
consistent snapshot per call — no generation, cache or refresh handshake. The
snapshots are read-only; the one command that changes host state is
`CE_UnloadModuleV2`, described at the end of this section.

| Rule | Detail |
|---|---|
| Counts | `capacity`/`required` count **elements**, not bytes; `(nullptr, 0)` is a legal size query returning `CE_OK`. |
| Bad arguments | `(!items && capacity)` or a null `required` returns `CE_INVALID_ARGUMENT`. |
| Short buffer | Writes **nothing at all**, returns `CE_INVALID_ARGUMENT`, and still sets `required` to the real count. |
| Hard ceiling | More than `CE_V2_MAX_VIEW_ITEMS` (1048576) items returns `CE_UNSUPPORTED`. |
| `size`/`version` | **Output only**: the implementation writes them into every filled element, so callers never pre-seed — the opposite of input structs such as `CeScanRequestV2`. |

`src/bridge/ui_bridge.cpp` pins the 48/552/168-byte layouts with
`static_assert`, and `runtime_v2_tests` asserts them at run time.

**Unload**

`CE_UnloadModuleV2(uint64_t base)` releases one module of the host process,
named by the `base` a module enumeration reported. It is synchronous and has no
generation: the caller re-enumerates afterwards.

| Rule | Detail |
|---|---|
| Identity | The base must be the start of a loaded module. `GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, ...)` answers that before the loader is involved; a `0` or an address inside a module's body returns `CE_INVALID_ARGUMENT`. |
| Refusals | The host executable and this DLL return `CE_UNSUPPORTED`. Everything else reaches `FreeLibrary`, which may refuse it in turn. |
| Risk | The module may be in use. `FreeLibrary` can unmap code another thread is running, and the host process can die. There is no undo, and no protection beyond the two refusals above. |
| Effect | Only the reference count moves. A module loaded twice needs two releases, and a module the system or another component holds stays mapped. |

**Regions**

- Sorted by address and **tiled** across the whole user-mode range from
  `lpMinimumApplicationAddress` to `lpMaximumApplicationAddress`, **including
  MEM_FREE / MEM_RESERVE** (CE's State column does the same). A `MEM_FREE` row
  therefore has `type` and `allocation_protect` equal to 0, with `protect` left
  at the `PAGE_NOACCESS` that `VirtualQuery` reports for free pages.
- A typical small process has 300-500 rows; a heavy host can reach tens of
  thousands. The walk is one `VirtualQuery` per region, so refreshing is
  user-triggered and never polled.

**Modules and threads**

- A module's `path` is an inline fixed 260-wchar copy, truncated past
  `MAX_PATH`; a thread's `description` is fixed 64 and truncates the same way.
- `CeThreadInfoV2.valid_mask`: bit0 = `description` holds a name, bit1 =
  `priority` is valid, bit2 = `created` is valid. **A clear bit means the field
  is zero and carries no information** — no Win32 sentinel such as
  `THREAD_PRIORITY_ERROR_RETURN`. `priority` keeps `GetThreadPriority`'s signed
  -15..15 value in a `uint32_t`, and `created` is a raw FILETIME (100 ns units
  since 1601-01-01 UTC) with no implicit time-zone conversion.
- **Read-only is enforced by the access mask**: thread enumeration opens
  threads with `THREAD_QUERY_LIMITED_INFORMATION` only, so this code cannot
  suspend, resume or terminate a thread even by accident. Memory protection,
  thread priorities and the module list are never modified.

**Call-site restriction.** Module and thread enumeration use
`CreateToolhelp32Snapshot`. The documented limitation of that family is about
snapshotting a process other than the current one, and here the host *is* the
current process, so the only real exposure is calling them while the calling
thread holds the loader lock (from `DllMain` or a heap callback). API callers
must not call them from there. The DLL's own `DllMain` creates one thread and
touches none of these interfaces.

## Pointer scan

Finds multi-level pointer paths backwards from a target address; a result can be
added to the address list as a pointer record. It follows CE's
`pointerscancontroller.pas` + `pointerscanworker.pas`, but does a single search
and no result rescan.

### Offset order

`CeAddressV2` resolves level by level as `v = read(v) + offset`. Searching
backwards, a slot's value is `current address - offset`, and the slot's address
becomes the next current address. Offsets are therefore discovered **innermost
first** and reversed into execution order before they are reported. The
reversed array still resolves — it just lands somewhere else, which is why the
tests assert the contents and order of the offset array rather than "it
resolves".

### Cost model and caps

- The number of offsets per node is `2*max_offset/alignment + 1`; testing each
  with its own binary search would cost 1025 lookups per node at the UI
  defaults (max offset `0x1000`, alignment 8) and 2049 at alignment 4 — 2-4 ms,
  independent of the number of hits.
- Instead the harvested table is sorted by value once, and each node does
  **one** `upper_bound` followed by a backward walk over the hits; the offset
  falls out of `parent value - slot value`. The caps bound memory; this is what
  bounds time.

Four brakes, all reported honestly:

| Cap | Value | Behaviour |
|---|---|---|
| Harvested slots | 4M | `CE_UNSUPPORTED`; the table reserves the whole 4M-slot budget up front, so it cannot grow past it |
| Hits per node | 16 | Sets `truncated`, still returns the results found |
| Nodes per level | 200000 | Sets `truncated`, still returns the results found |
| Results | 65536 | Sets `truncated`, still returns the results found |

The last three set the `truncated` bit and **still return the results already
found** — failing wholesale at the moment it becomes useful is the wrong trade.
The UI always enables `CE_PTRSCAN_NO_LOOP` (a self-referential path only
produces junk chains).

### Static filter

The static filter means "emit and stop when you hit one", not "keep only level
N". A node whose address falls inside a loaded module is emitted immediately
and not descended into, so a `levels=4` scan returns the 2- and 3-level chains
it finds on the way. Intermediate hops need not be static; only the outermost
address is tested.

### Verification

Results are re-resolved one by one before they are listed: the harvest is not an
atomic snapshot, so a path can already be dangling when it is reported. Every
candidate is resolved once more with `typed::resolve` and kept only if it lands
exactly on the target. 65536 candidates × at most 8 dereferences is
milliseconds, and turns "best effort" into a hard postcondition. Freezes keep
running during the scan, so frozen values move — expected, and covered by the
same re-resolution.

### Relationship to the value scan

Only one job runs at a time (shared admission and cancellation epoch; one
Cancel reaches whichever is running). Everything else is separate: its own
state structures, generation and result set. The pointer scan has its own
`pointer` section and a lowercase phase vocabulary
(`enumerating` / `reading` / `walking` / ...), so its phases never render on the
Scan page as value-scan phases.

### Memory

Each harvested slot is 16 bytes, with the `static` flag carried in the address's
high bit (user-mode addresses stay below 2^47); four million slots is 64 MiB.
The table is reserved at the cap, so a growth reallocation can never invalidate
the self-exclusion range.

### Still missing

Result rescan / table reuse, heap-block filtering (CE's `useheapdata`), and
spilling the harvest table to a temporary file to break the 64 MiB cap.

## Not implemented

The debugger, disassembly/assembly, Lua, Auto Assembler, the full LCL/trainer
UI, plugins and the managed runtime are still not migrated. Opaque CT records
are **preserved, not executed**: script types round-trip but never run, and
there is no editor for them. The driver, DBVM, and remote service capabilities have been dropped.
implementation differential has still never run; current evidence comes from
the self-built test host.
