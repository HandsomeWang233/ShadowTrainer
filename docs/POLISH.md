# Stage 2 Polish

[English](POLISH.md) | [中文](POLISH_CN.md)

The record of one development batch: reliability and usability work on features
that were already implemented. 

## Core fixes

- **Freeze and write conflicts.** An explicit write of a frozen record
  (`Write selected`) re-resolves the record first, because a pointer may have
  moved since the last freeze tick, and rejects the write with
  `CE_INVALID_ARGUMENT` when the resolved range overlaps another enabled freeze.
  Nothing is written to the target and the saved freeze bytes are unchanged; the
  conflict disables that record's freeze. The freeze tick applies the same
  conflict rule, so a background freeze that starts to overlap drops its own
  freeze instead of writing. Explicit writes of non-frozen records keep their
  historical behavior.
- **V1 CT save preflight.** A V1 save validates the whole collection before
  touching the destination: any record the V1 subset cannot express — a non-V1
  type, a duplicate address — fails the save with `CE_UNSUPPORTED` pointing at
  V2 export. The existing file at the destination is preserved exactly; the
  collection is never merged or trimmed, and a table that could not be loaded
  back is never written. Same-address records are a V2-supported shape.

## Real scan status

`CE_GetScanStatusV2` and its own 80-byte `CeScanStatusV2` are additive: the
existing structures and version values are unchanged. The snapshot carries
operation ID, phase, active and cancel-request flags, `has_scan`/`can_undo`,
generation, and the progress unit with its counts.

- The first scan's enumeration phase has no known total; progress after that is
  counted in readable bytes (unreadable pages are not counted). A Next scan
  counts candidates from the previous generation.
- Reading the source is not committing the file: `writing` and `committing` are
  displayed separately, and only `COMPLETED` means success. A cancellation is not
  disguised as 100%: the count may already equal the total when the cancel lands
  at the final commit, and the terminal phase is still `CANCELLED`.
- The query takes a short try-lock and may return `CE_BUSY` instead of waiting
  for the scan to finish; the caller retries and keeps its last snapshot.
- Undo is enabled only while the core actually holds the previous generation:
  never on the first scan, and not after Undo (the previous generation is
  consumed) or New. A generation with zero results still counts as history. A
  failed or cancelled pass keeps the committed results and the undo state.

## Record refresh

The address list gained a `Current value` column. It polls the current page's
rows in batches under a budget — only while the window is visible and not
minimized, one poll per 500 ms, 16 rows / 64 KiB and 12 ms of work per poll,
values previewed to the first 256 bytes. Long values carry an explicit
truncation notice; an unreadable row reports an error instead of 0.

Hiding the window pauses only these live reads; it never pauses a freeze the
user enabled. A failed write or a failed background freeze turns the row's
`Frozen` off and keeps the error in `Last core status`; later status queries do
not replace that diagnostic.

## Memory page

- `Resolve` re-parses the address/pointer chain from the form.
- `Previous 256` / `Next 256` move over the resolved page address and do not
  modify the resolve inputs.
- A page that would overflow or underflow the host address range is refused; the
  full 256-byte window must fit.
- Unreadable cells are marked (`??`) and are not editable.

## Evidence and boundaries

`polish_core_tests` is the new suite; `core_tests`, `typed_tests`,
`hotkey_tests`, `runtime_tests` and `runtime_v2_tests` keep running as
regression. The retired UI's sources, binding tests and probes are kept cold in
`baseline/pre-webui/` — a pure archive, not a rollback path, that the build does
not reference. The reference implementation's source is unchanged.

This batch hardens features that already existed. The full debugger, Lua and
Auto Assembler (AA) migration is still pending, and the Tutorial / real
external-injection acceptance has not been run. The authoritative result is the
local build and its tests (see [`STATUS.md`](STATUS.md)).
