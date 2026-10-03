// The page's own state and event loop.
//
// The model here is a copy of what the runtime last said, kept only so a
// re-render does not need to ask again; the runtime owns every fact. Commands
// always send the CURRENT form values, read straight off the DOM, because the
// runtime deliberately keeps no copy of the form: a reload then cannot leave the
// two sides disagreeing about what is in a field.
import { call, connected, onEvent, request } from './bridge.js';
import * as boot from './boot.js';
import { enabled, apply as applyGating } from './gating.js';
import { PAGE_SIZE } from './format.js';
import * as chrome from './chrome.js';
import { createScanPage } from './scan.js';
import { createAddressPage } from './address.js';
import { createMemoryPage } from './memory.js';
import { createViewsPage } from './views.js';
import { createPointerPage } from './pointer.js';

const node = (id) => document.getElementById(id);
const value = (id) => (node(id) ? node(id).value : '');
const num = (id) => Number(value(id)) || 0;
const checked = (id) => !!(node(id) && node(id).checked);

/** Everything the runtime has told us, keyed by section. */
const model = {
  host: null,
  status: '',
  scan: null,
  pointer: null,
  results: null,
  resultRows: [],
  records: null,
  recordRows: [],
  views: null,
  controls: null,
  memory: null,
};

// One module can serve several pages: the three process views share theirs, so
// the rail's index is mapped to a module rather than indexed into a list.
let modules = [];
let pageModule = [];

// ---- form ------------------------------------------------------------------------

function scanArgs() {
  return {
    type: num('scan-type'),
    signed: checked('scan-signed'),
    hex: checked('scan-hex'),
    comparison: num('scan-compare'),
    alignment: num('scan-alignment'),
    rounding: num('scan-rounding'),
    byteLength: value('scan-length'),
    begin: value('scan-begin'),
    end: value('scan-end'),
    value: value('scan-value'),
    second: value('scan-second'),
  };
}

function addressArgs() {
  return {
    base: value('address-base'),
    offsets: value('address-offsets'),
    type: num('address-type'),
    signed: checked('address-signed'),
    hex: checked('address-hex'),
    byteLength: value('address-length'),
    description: value('address-description'),
    value: value('address-value'),
    snapshot: checked('address-snapshot'),
  };
}

/** The form object the gating rules look at. */
function formState() {
  return {
    type: num('scan-type'),
    comparison: num('scan-compare'),
    editType: num('address-type'),
  };
}

// ---- commands ---------------------------------------------------------------------

async function run(command, args, after) {
  const reply = await request(command, args);
  if (reply && typeof reply.status === 'string' && reply.status) chrome.setStatus(reply.status);
  if (reply && reply.ok && after) after(reply.data);
  updateGating();
  return reply;
}

/** Applies the editor payload that "activate a result" and "select a record" share. */
function fillEditor(data) {
  if (!data) return;
  const set = (id, text) => { const n = node(id); if (n !== null && text !== undefined) n.value = text; };
  set('address-base', data.base);
  set('address-offsets', data.offsets);
  set('address-description', data.description);
  set('address-type', String(data.type));
  set('address-length', String(data.byteLength));
  set('address-value', data.value);
  node('address-signed').checked = !!data.signed;
  node('address-hex').checked = !!data.hex;
  chrome.setText('address-current', data.current || 'Current value: unavailable.');
  chrome.setText('address-selected', data.selected && data.selected !== '0'
    ? `Selected record ID: ${data.selected}`
    : 'Selected record ID: none');
  // The memory page follows the same address, as it did in the native UI.
  set('memory-base', data.base);
  set('memory-offsets', data.offsets);
}

function wire() {
  const onClick = (id, command, args) => {
    const target = node(id);
    if (target) target.addEventListener('click', () => run(command, typeof args === 'function' ? args() : args));
  };

  onClick('stop-session', 'session.stop');
  onClick('scan-first', 'scan.first', scanArgs);
  onClick('scan-next', 'scan.next', scanArgs);
  onClick('scan-new', 'scan.new');
  onClick('scan-undo', 'scan.undo');
  onClick('scan-cancel', 'scan.cancel');
  onClick('results-prev', 'results.page', () => ({ delta: -PAGE_SIZE }));
  onClick('results-next', 'results.page', () => ({ delta: PAGE_SIZE }));
  onClick('results-refresh', 'results.refresh');

  onClick('address-read', 'record.read', () => {
    const a = addressArgs();
    return { base: a.base, offsets: a.offsets, type: a.type, signed: a.signed, hex: a.hex, byteLength: a.byteLength };
  });
  onClick('address-write', 'record.writeAddress', addressArgs);
  onClick('address-add', 'record.add', addressArgs);
  onClick('address-update', 'record.update', addressArgs);
  onClick('record-write', 'record.write', () => ({ value: value('address-value') }));
  onClick('record-freeze-on', 'record.freeze', () => ({ enabled: true }));
  onClick('record-freeze-off', 'record.freeze', () => ({ enabled: false }));
  onClick('record-remove', 'record.remove');
  onClick('record-save', 'record.saveCT');
  onClick('record-open', 'record.openCT');
  onClick('records-prev', 'record.page', () => ({ delta: -PAGE_SIZE }));
  onClick('records-next', 'record.page', () => ({ delta: PAGE_SIZE }));
  onClick('records-refresh', 'record.refresh');

  // memory.js and pointer.js own all of their own buttons: those commands answer
  // with the payload that paints the page, so the reply has to reach the module
  // that knows what to do with it.

  for (const [kind, id] of [['regions', 'regions'], ['modules', 'modules'], ['threads', 'threads']]) {
    onClick(`${id}-prev`, 'view.page', () => ({ kind, delta: -PAGE_SIZE }));
    onClick(`${id}-next`, 'view.page', () => ({ kind, delta: PAGE_SIZE }));
    onClick(`${id}-refresh`, 'view.refresh', () => ({ kind }));
  }

  onClick('ptr-scan', 'ptr.scan', () => ({
    target: value('ptr-target'),
    levels: num('ptr-levels'),
    maxOffset: value('ptr-offset'),
    alignment: num('ptr-align'),
    staticOnly: checked('ptr-static'),
  }));
  onClick('ptr-cancel', 'ptr.cancel');

  // Any form change can change which controls are usable.
  document.querySelectorAll('input, select').forEach((element) => {
    element.addEventListener('change', updateGating);
    element.addEventListener('input', updateGating);
  });
}

// ---- rendering --------------------------------------------------------------------

function updateGating() {
  applyGating(enabled(model.controls, formState()));
}

function renderAll() {
  modules.forEach((page) => page.render(model));
  updateGating();
}

function onPage(index) {
  chrome.showPage(index);
  const page = pageModule[index];
  if (page && page.activate) page.activate(model, index);
}

// ---- events -----------------------------------------------------------------------

function mergeEvent(event) {
  const changed = new Set(event.changed || []);
  if (changed.has('status')) model.status = event.status || model.status;
  for (const section of ['scan', 'pointer', 'results', 'records', 'views', 'controls']) {
    if (changed.has(section)) model[section] = event[section];
  }
  if (changed.has('resultRows')) {
    const rows = event.resultRows || { rows: [] };
    if (rows.reset) model.resultRows = [];
    for (const row of rows.rows || []) model.resultRows[row.i] = row;
  }
  if (changed.has('recordRows')) {
    const rows = event.recordRows || { rows: [] };
    if (rows.reset) model.recordRows = [];
    for (const row of rows.rows || []) model.recordRows[row.i] = row;
  }
  if (model.status) chrome.setStatus(model.status);
  const [word, state] = chrome.sessionFrom(model.controls);
  chrome.setSession(word, state);
  // Both progress bars are painted by the page that owns them, in renderAll()
  // below, so every path that repaints the model also repaints them.
  renderAll();
}

// ---- start ------------------------------------------------------------------------

async function start() {
  chrome.install({ onPage });
  const context = { run, fillEditor, node, value, num, checked, goto: onPage };
  const scan = createScanPage(context);
  const address = createAddressPage(context);
  const memory = createMemoryPage(context);
  const views = createViewsPage(context);
  const pointer = createPointerPage(context);
  modules = [scan, address, memory, views, pointer];
  // Rail order: Scan, Address list, Memory preview, Regions, Modules, Threads,
  // Pointer scan.
  pageModule = [scan, address, memory, views, views, views, pointer];
  wire();

  // The window plays the word itself while WebView2 comes up and tells us when
  // it is done; this resolves then. Everything below paints behind the veil, so
  // the app is never uncovered half-built.
  const veil = boot.prepare();

  if (!connected) {
    chrome.setStatus('Preview mode: this page is not running inside the runtime, so nothing is connected.');
    chrome.showPage(0);
    renderAll();
    await veil;
    boot.lift();
    return;
  }

  onEvent(mergeEvent);
  try {
    const data = await call('hello');
    model.status = data.status || '';
    model.scan = data.scan;
    model.pointer = data.pointer;
    model.results = data.results;
    model.records = data.records;
    model.views = data.views;
    model.controls = data.controls;
    model.memory = data.memory;
    model.resultRows = (data.resultRows && data.resultRows.rows) || [];
    model.recordRows = (data.recordRows && data.recordRows.rows) || [];
    chrome.setHost(data.host.pid);
    chrome.setStatus(model.status);
    chrome.showPage(0);
    renderAll();
  } catch (error) {
    chrome.setStatus(`The runtime did not answer: ${error.message}`);
  }

  await veil;
  boot.lift();
}

start();
