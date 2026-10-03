// The enable/disable rules, ported from the native UI's update_controls().
//
// The runtime re-checks the same predicates when a command arrives, so a stale
// page cannot slip one past it -- but a wrong rule here still means a control
// that looks usable and is not, or one that looks dead and is not.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import { enabled } from '../../web/js/gating.js';
import { CE_TYPE, CE_CMP } from '../../web/js/format.js';

const idle = {
  idle: true, stopping: false, dialog: false, busy: false, scanning: false,
  haveScan: false, canUndo: false, canRefresh: true, recordsStale: false,
  selected: '0', selectedOpaque: false, job: 'none', pointerActive: false,
  cancelRequested: false,
  resultsPrev: false, resultsNext: false, recordsPrev: false, recordsNext: false,
  ptrPrev: false, ptrNext: false, ptrAdd: false,
  memoryValid: false, memoryBack: false, memoryForward: false,
  memoryPrev: false, memoryNext: false, memoryRefresh: false, hexUndo: false, hexFill: false,
  views: {
    regions: { prev: false, next: false },
    modules: { prev: false, next: false },
    threads: { prev: false, next: false },
  },
};

const form = { type: CE_TYPE.U32, comparison: CE_CMP.EXACT, editType: CE_TYPE.U32 };
const flags = (controls, state = form) => enabled({ ...idle, ...controls }, state);

test('a fresh idle session can scan but not undo or cancel', () => {
  expect(flags({})['scan-first']).toBe(true);
  expect(flags({})['scan-next']).toBe(false);       // nothing committed yet
  expect(flags({})['scan-undo']).toBe(false);
  expect(flags({})['scan-cancel']).toBe(false);
  expect(flags({})['results-refresh']).toBe(true);
});

test('a first scan is offered only for the comparisons that seed one', () => {
  for (const comparison of [CE_CMP.EXACT, CE_CMP.GREATER, CE_CMP.BETWEEN]) {
    expect(flags({}, { ...form, comparison })['scan-first']).toBe(true);
  }
  // "Changed" and the rest refine a committed scan, so they can never start one
  // -- not even when a scan exists.
  for (const comparison of [CE_CMP.CHANGED, CE_CMP.UNCHANGED, CE_CMP.INCREASED]) {
    expect(flags({}, { ...form, comparison })['scan-first']).toBe(false);
    expect(flags({ haveScan: true }, { ...form, comparison })['scan-first']).toBe(false);
  }
});

test('the scan format is locked once a scan is committed', () => {
  const before = flags({});
  const after = flags({ haveScan: true });
  for (const id of ['scan-type', 'scan-signed', 'scan-hex', 'scan-compare',
                    'scan-begin', 'scan-end', 'scan-alignment']) {
    expect(before[id]).toBe(true);
    expect(after[id]).toBe(false);
  }
  // The bytes field is additionally gated by the type's width.
  expect(flags({}, { ...form, type: CE_TYPE.AOB })['scan-length']).toBe(true);
  expect(flags({ haveScan: true }, { ...form, type: CE_TYPE.AOB })['scan-length']).toBe(false);
});

test('next scan needs a committed scan and a comparison that refines one', () => {
  expect(flags({ haveScan: true })['scan-next']).toBe(true);
  expect(flags({ haveScan: true }, { ...form, comparison: CE_CMP.UNKNOWN })['scan-next']).toBe(false);
  expect(flags({})['scan-next']).toBe(false);
});

test('rounding is offered only for an exact float or double scan', () => {
  expect(flags({}, { ...form, type: CE_TYPE.FLOAT })['scan-rounding']).toBe(true);
  expect(flags({}, { ...form, type: CE_TYPE.DOUBLE })['scan-rounding']).toBe(true);
  expect(flags({}, { ...form, type: CE_TYPE.U32 })['scan-rounding']).toBe(false);
  expect(flags({}, { ...form, type: CE_TYPE.FLOAT, comparison: CE_CMP.GREATER })['scan-rounding'])
    .toBe(false);
});

test('the bytes field only matters for a variable-width type', () => {
  expect(flags({}, { ...form, type: CE_TYPE.UTF8 })['scan-length']).toBe(true);
  expect(flags({}, { ...form, type: CE_TYPE.AOB })['scan-length']).toBe(true);
  expect(flags({}, { ...form, type: CE_TYPE.U64 })['scan-length']).toBe(false);
});

test('the second value belongs to the range comparison only', () => {
  expect(flags({}, { ...form, comparison: CE_CMP.BETWEEN })['scan-second']).toBe(true);
  expect(flags({}, { ...form, comparison: CE_CMP.EXACT })['scan-second']).toBe(false);
  // ... and only for a numeric type.
  expect(flags({}, { ...form, type: CE_TYPE.UTF8, comparison: CE_CMP.BETWEEN })['scan-second'])
    .toBe(false);
});

test('record actions need a selection that is not an imported opaque row', () => {
  const none = flags({});
  expect(none['address-update']).toBe(false);
  expect(none['record-remove']).toBe(false);

  const selected = flags({ selected: '7' });
  expect(selected['address-update']).toBe(true);
  expect(selected['record-write']).toBe(true);
  expect(selected['record-freeze-on']).toBe(true);
  expect(selected['record-remove']).toBe(true);

  // An imported CT row can be discarded but never edited or frozen.
  const opaque = flags({ selected: '7', selectedOpaque: true });
  expect(opaque['address-update']).toBe(false);
  expect(opaque['record-write']).toBe(false);
  expect(opaque['record-freeze-on']).toBe(false);
  expect(opaque['record-remove']).toBe(true);

  // A listing that changed underneath the selection cannot be acted on at all.
  const stale = flags({ selected: '7', recordsStale: true });
  expect(stale['address-update']).toBe(false);
  expect(stale['record-remove']).toBe(false);
});

test('a busy session closes the door on new work but not on its own cancel', () => {
  const scanning = flags({ idle: false, scanning: true, canRefresh: false });
  expect(scanning['scan-first']).toBe(false);
  expect(scanning['scan-new']).toBe(false);
  expect(scanning['scan-cancel']).toBe(true);
  expect(scanning['results-refresh']).toBe(false);
  expect(scanning['ptr-refresh']).toBe(false);   // its own idle flag gates it
  // The pointer cancel follows its own activity flag instead.
  expect(flags({ idle: false, pointerActive: true, job: 'pointer' })['ptr-cancel']).toBe(true);
});

test('a stopping session disables the frame as well', () => {
  const stopping = flags({ stopping: true, idle: false });
  expect(stopping['stop-session']).toBe(false);
  expect(stopping['scan-first']).toBe(false);
});

test('paging buttons follow the runtime page bounds', () => {
  expect(flags({})['results-prev']).toBe(false);
  expect(flags({ resultsPrev: true, resultsNext: true })['results-next']).toBe(true);
  expect(flags({ views: { ...idle.views, regions: { prev: true, next: false } } })['regions-prev'])
    .toBe(true);
});
