// The Pointer scan page: the job's progress line and its result table.
//
// Result rows are not part of the model -- they only come back in the reply to
// ptr.refresh / ptr.page -- so this page owns those buttons and paints from the
// payload it receives. A scan is followed through `pointer`: when it stops, the
// finished page is fetched once, and entering the page fetches the current one.
import * as chrome from './chrome.js';
import { PAGE_SIZE } from './format.js';

const CELLS = 3;
const PLACEHOLDER_ROWS = 3;

export function createPointerPage(ctx) {
  const table = ctx.node('ptr-list');
  const body = table.querySelector('tbody');
  let built = -1;          // how many rows the DOM currently holds
  let pageStart = null;    // the payload page those rows belong to
  let rows = [];           // the rows of the last payload
  let selected = -1;       // index into those rows
  let fetching = false;    // a ptr.refresh is already in flight
  let queued = false;      // a fetch arrived while one was in flight
  let lastActive = false;
  let lastValid = false;
  let lastGeneration = null;
  let observed = false;    // the first render only records the initial state

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
    selected = index;
    for (let i = 0; i < body.children.length; i += 1) {
      body.children[i].classList.toggle('is-selected', i === selected);
    }
  }

  function apply(data) {
    if (!data) return;
    rows = data.rows || [];
    selected = -1;
    if (data.label !== undefined) chrome.setText('ptr-page', data.label);
    if (!rows.length) {
      placeholder();
      return;
    }
    if (pageStart !== data.start || built !== rows.length) {
      body.replaceChildren();
      for (let i = 0; i < rows.length; i += 1) {
        const tr = document.createElement('tr');
        tr.dataset.index = String(i);
        for (let c = 0; c < CELLS; c += 1) tr.append(document.createElement('td'));
        tr.addEventListener('click', () => select(i));
        body.append(tr);
      }
      built = rows.length;
      pageStart = data.start;
    }
    for (let i = 0; i < built; i += 1) {
      const row = rows[i];
      const tr = body.children[i];
      if (!row || !tr) continue;
      const values = [row.base, row.offsets, row.static ? 'yes' : 'no'];
      for (let c = 0; c < CELLS; c += 1) {
        const td = tr.children[c];
        const text = values[c] === undefined || values[c] === null ? '--' : String(values[c]);
        if (td.textContent !== text) td.textContent = text;
      }
    }
  }

  function fetchRows() {
    if (fetching) {
      queued = true;
      return;
    }
    fetching = true;
    const done = () => {
      fetching = false;
      if (queued) {
        queued = false;
        fetchRows();
      }
    };
    Promise.resolve(ctx.run('ptr.refresh', undefined, (data) => apply(data))).then(done, done);
  }

  function activate() {
    fetchRows();
  }

  function render(model) {
    const pointer = model.pointer || {};
    chrome.setText('ptr-progress-label', pointer.label || 'Pointer scan: idle | No results');
    chrome.setProgress('ptr-progress', model.pointer ? pointer.progress : null);

    // A scan that stops while this page is open publishes its rows only through
    // a fresh reply, so watch the section for the active -> idle edge.
    const active = !!pointer.active;
    const valid = !!pointer.valid;
    const generation = pointer.generation === undefined ? '' : String(pointer.generation);
    if (!observed) observed = true;
    else if (valid && lastValid && !active && (lastActive || generation !== lastGeneration)) fetchRows();
    lastActive = active;
    lastValid = valid;
    lastGeneration = generation;
  }

  // The rows of a pointer page exist only in the command's reply, so paging,
  // refresh and add belong here rather than in app.js's shared wiring.
  function own(id, handler) {
    const target = ctx.node(id);
    if (target) target.addEventListener('click', handler);
  }

  own('ptr-prev', () => ctx.run('ptr.page', { delta: -PAGE_SIZE }, apply));
  own('ptr-next', () => ctx.run('ptr.page', { delta: PAGE_SIZE }, apply));
  own('ptr-refresh', () => ctx.run('ptr.refresh', undefined, apply));
  own('ptr-add', () => {
    if (selected < 0) return;   // nothing picked yet
    ctx.run('ptr.addToList', { index: selected });
  });

  return {
    render,
    activate,
    // The index ptr-add would send, exposed for callers that need it.
    selectedIndex: () => selected,
  };
}
