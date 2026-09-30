// The Scan page: live telemetry, the result table and its paging.
//
// Rows arrive in two pieces -- the page's metadata in the `results` section and
// the rows themselves, in batches, in `resultRows` -- so a passive reload during
// a scan repaints only what appeared instead of the whole page every 100 ms.
import * as chrome from './chrome.js';

const CELL = ['number', 'address', 'value'];

export function createScanPage(ctx) {
  const table = document.getElementById('results-list');
  const body = table.querySelector('tbody');
  let built = -1;          // how many rows the DOM currently holds
  let pageStart = null;    // the page those rows belong to

  function placeholder() {
    body.replaceChildren();
    for (let i = 0; i < 3; i += 1) {
      const tr = document.createElement('tr');
      tr.className = 'is-placeholder';
      tr.append(document.createElement('td'), document.createElement('td'), document.createElement('td'));
      for (const td of tr.children) td.textContent = '--';
      body.append(tr);
    }
    built = -1;
    pageStart = null;
  }

  function activate(index) {
    ctx.run('results.activate', { index }, (data) => {
      ctx.fillEditor(data);
      ctx.goto(1);
    });
  }

  function build(count) {
    body.replaceChildren();
    for (let i = 0; i < count; i += 1) {
      const tr = document.createElement('tr');
      tr.dataset.index = String(i);
      for (let c = 0; c < CELL.length; c += 1) tr.append(document.createElement('td'));
      tr.addEventListener('dblclick', () => activate(i));
      tr.tabIndex = 0;
      tr.addEventListener('keydown', (event) => {
        if (event.key === 'Enter') activate(i);
      });
      body.append(tr);
    }
    built = count;
  }

  function render(model) {
    const results = model.results || {};
    chrome.setText('results-page', results.label || 'Results: 0 total | 512/page');
    chrome.setText('scan-progress-label',
      (model.scan && model.scan.label) || 'Scan: idle | No committed scan');

    const rows = model.resultRows || [];
    const count = rows.filter(Boolean).length;
    if (!count) {
      placeholder();
      return;
    }
    if (pageStart !== results.start || built !== count || built === -1) {
      build(Number(results.count) || count);
      pageStart = results.start;
    }
    for (let i = 0; i < built; i += 1) {
      const row = rows[i];
      const tr = body.children[i];
      if (!row || !tr) continue;
      const values = [row.number, row.address, row.value];
      for (let c = 0; c < CELL.length; c += 1) {
        const td = tr.children[c];
        const text = values[c] === undefined ? '' : String(values[c]);
        if (td.textContent !== text) td.textContent = text;
      }
    }
  }

  return { render };
}
