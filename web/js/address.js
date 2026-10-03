// The Address list page: the record table and row selection.
//
// Records arrive in two pieces -- the page metadata in `records` and the rows
// themselves in `recordRows` -- so a live value refresh repaints only what
// changed instead of the whole table every 100 ms. The editor above the table
// belongs to app.js; a click here only asks the runtime for the selection
// payload and hands it straight to fillEditor.
import * as chrome from './chrome.js';
import { install as installSort, paint as paintSort } from './sort.js';

const CELLS = 7;
const PLACEHOLDER_ROWS = 3;

export function createAddressPage(ctx) {
  const table = ctx.node('records-list');
  const body = table.querySelector('tbody');
  let built = -1;          // how many rows the DOM currently holds
  let pageStart = null;    // the page those rows belong to

  // A record sort reads every record, not just the page on screen, so the reply
  // carries the list's metadata and the rows follow on the next tick.
  installSort(table, (column) => ctx.run('record.sort', { column }));

  function placeholder() {
    if (built === -1 && body.children.length === PLACEHOLDER_ROWS) return;
    body.replaceChildren();
    for (let i = 0; i < PLACEHOLDER_ROWS; i += 1) {
      const tr = document.createElement('tr');
      tr.className = 'is-placeholder';
      for (let c = 0; c < CELLS; c += 1) tr.append(document.createElement('td'));
      for (const td of tr.children) td.textContent = '--';
      body.append(tr);
    }
    built = -1;
    pageStart = null;
  }

  function select(index) {
    // An imported record answers with `opaque: true` and a busy session refuses
    // outright; ctx.run already surfaces both on the status line, so only the
    // success path here has to feed the editor.
    ctx.run('record.select', { index }, (data) => ctx.fillEditor(data));
  }

  function build(count) {
    body.replaceChildren();
    for (let i = 0; i < count; i += 1) {
      const tr = document.createElement('tr');
      for (let c = 0; c < CELLS; c += 1) tr.append(document.createElement('td'));
      tr.addEventListener('click', () => select(i));
      body.append(tr);
    }
    built = count;
  }

  function render(model) {
    const records = model.records || {};
    chrome.setText('records-page', records.label || 'Records: 0');
    paintSort(table, records.sortColumn, records.sortDescending);

    // fillEditor owns these two readouts; clearing them when the runtime no
    // longer has a selection is the only write this page makes to them.
    const selectedId = records.selected === undefined ? '' : String(records.selected);
    if (!selectedId || selectedId === '0') {
      chrome.setText('address-selected', '');
      chrome.setText('address-current', '');
    }

    const rows = model.recordRows || [];
    const loaded = rows.filter(Boolean).length;
    if (!loaded) {
      placeholder();
      return;
    }
    const count = Number(records.count) || loaded;
    if (pageStart !== records.start || built !== count) {
      build(count);
      pageStart = records.start;
    }
    const selectedIndex = Number(records.selectedIndex);
    for (let i = 0; i < built; i += 1) {
      const row = rows[i];
      const tr = body.children[i];
      if (!tr) continue;
      if (!row) {
        for (const td of tr.children) {
          if (td.textContent !== '--') td.textContent = '--';
        }
        tr.classList.remove('is-selected');
        continue;
      }
      const values = [
        row.id,
        row.description,
        row.resolved,
        row.typeName,
        row.frozen ? 'yes' : 'no',
        row.statusText,
        row.hasValue ? row.value : '--',
      ];
      for (let c = 0; c < CELLS; c += 1) {
        const td = tr.children[c];
        const text = values[c] === undefined || values[c] === null || values[c] === ''
          ? '--' : String(values[c]);
        if (td.textContent !== text) td.textContent = text;
      }
      tr.classList.toggle('is-selected', row.i === selectedIndex);
    }
  }

  return { render };
}
