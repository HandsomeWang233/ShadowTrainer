// Which controls are usable, given what the runtime reports and what is typed.
//
// The runtime pushes everything that follows from core state (idle, busy,
// haveScan, canUndo, recordsStale, page bounds, and so on). The rules that also
// depend on the FORM -- which comparison is selected, which value type, whether
// an imported record is selected -- live here, because re-sending the whole
// gating map on every keystroke would be silly. The runtime re-checks the same
// predicates when a command arrives, so a stale page cannot slip one past it.
import { CE_TYPE, CE_CMP, FIXED_WIDTH } from './format.js';

function needsValue(comparison) {
  return comparison === CE_CMP.EXACT
    || comparison === CE_CMP.GREATER
    || comparison === CE_CMP.LESS
    || comparison === CE_CMP.BETWEEN
    || comparison === CE_CMP.INCREASED_BY
    || comparison === CE_CMP.DECREASED_BY;
}

function roundingApplies(type, comparison) {
  return (type === CE_TYPE.FLOAT || type === CE_TYPE.DOUBLE) && comparison === CE_CMP.EXACT;
}

/**
 * @param {object} controls the `controls` section from the runtime
 * @param {object} form the current scan/address form values
 * @returns {Record<string, boolean>} control id to enabled
 */
export function enabled(controls, form) {
  const c = controls || {};
  const idle = !!c.idle;
  const editing = !c.stopping && !c.dialog;
  const integer = form.type <= CE_TYPE.U64;
  const numeric = form.type <= CE_TYPE.DOUBLE;
  const haveScan = !!c.haveScan;
  const selected = c.selected && c.selected !== '0';
  const opaque = !!c.selectedOpaque;
  const recordActions = idle && !c.recordsStale && selected && !opaque;

  return {
    // Frame
    'stop-session': !c.stopping,
    'win-minimize': true,
    'win-maximize': true,
    'win-close': true,

    // Scan page
    'scan-first': idle && form.comparison <= CE_CMP.BETWEEN,
    'scan-next': idle && haveScan && form.comparison !== CE_CMP.UNKNOWN,
    'scan-new': idle,
    'scan-undo': idle && !!c.canUndo,
    'scan-cancel': (c.scanning || !!c.cancelRequested) && !c.stopping && !c.cancelRequested,
    'scan-type': editing && !haveScan,
    'scan-signed': editing && !haveScan && integer,
    'scan-hex': editing && !haveScan && integer,
    'scan-compare': editing && !haveScan,
    'scan-begin': editing && !haveScan,
    'scan-end': editing && !haveScan,
    'scan-alignment': editing && !haveScan,
    'scan-length': editing && !haveScan && !FIXED_WIDTH[form.type],
    'scan-rounding': editing && roundingApplies(form.type, form.comparison),
    'scan-value': editing && needsValue(form.comparison),
    'scan-second': editing && numeric && form.comparison === CE_CMP.BETWEEN,
    'results-prev': !!c.resultsPrev,
    'results-next': !!c.resultsNext,
    'results-refresh': !!c.canRefresh,

    // Address list
    'address-read': editing,
    'address-write': editing,
    'address-type': editing,
    'address-signed': editing && form.editType <= CE_TYPE.U64,
    'address-hex': editing && form.editType <= CE_TYPE.U64,
    'address-length': editing && !FIXED_WIDTH[form.editType],
    'address-add': idle,
    'address-update': recordActions,
    'record-write': recordActions,
    'record-freeze-on': recordActions,
    'record-freeze-off': recordActions,
    'record-remove': idle && !c.recordsStale && selected,
    'record-save': idle,
    'record-open': idle,
    'records-prev': !!c.recordsPrev,
    'records-next': !!c.recordsNext,
    'records-refresh': !!c.canRefresh,

    // Memory preview
    'memory-resolve': editing,
    'memory-prev': !!c.memoryPrev,
    'memory-next': !!c.memoryNext,
    'memory-refresh': !!c.memoryRefresh,
    'memory-back': !!c.memoryBack,
    'memory-forward': !!c.memoryForward,
    'memory-undo': !!c.hexUndo,
    'memory-fill': !!c.hexFill,
    'memory-offsets': editing,
    'memory-fill-byte': editing,

    // Process views
    'regions-prev': !!(c.views && c.views.regions.prev),
    'regions-next': !!(c.views && c.views.regions.next),
    'regions-refresh': !!c.canRefresh,
    'modules-prev': !!(c.views && c.views.modules.prev),
    'modules-next': !!(c.views && c.views.modules.next),
    'modules-refresh': !!c.canRefresh,
    'threads-prev': !!(c.views && c.views.threads.prev),
    'threads-next': !!(c.views && c.views.threads.next),
    'threads-refresh': !!c.canRefresh,

    // Pointer scan
    'ptr-scan': !c.stopping && !c.dialog && c.job === 'none',
    'ptr-cancel': !!c.pointerActive && !c.stopping && !c.cancelRequested,
    'ptr-add': editing && !!c.ptrAdd,
    'ptr-prev': !!c.ptrPrev,
    'ptr-next': !!c.ptrNext,
    'ptr-refresh': idle && c.job !== 'pointer',
  };
}

/** Applies the map: `disabled` is the attribute that also greys text out. */
export function apply(map) {
  for (const [id, on] of Object.entries(map)) {
    const node = document.getElementById(id);
    if (node) node.disabled = !on;
  }
}
