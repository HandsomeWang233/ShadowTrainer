// The three process views: Regions, Modules and Threads.
//
// One module serves all three rail pages. Each view is a snapshot the runtime
// keeps on its side and pushes as a slice of already-formatted row arrays, so
// this page only paints `views[kind]`. Entering a page loads its view once;
// paging afterwards is a local page change and never re-enumerates the host.
import * as chrome from './chrome.js';

const PLACEHOLDER_ROWS = 3;
const KINDS = ['regions', 'modules', 'threads'];
const FIRST_PAGE = 3;    // rail index of Regions

export function createViewsPage(ctx) {
  const requested = { regions: false, modules: false, threads: false };

  function table(listId, pageId, columns) {
    const list = ctx.node(listId);
    const body = list.querySelector('tbody');
    let built = -1;          // how many rows the DOM currently holds
    let pageStart = null;    // the page those rows belong to

    function placeholder() {
      if (built === -1 && body.children.length === PLACEHOLDER_ROWS) return;
      body.replaceChildren();
      for (let i = 0; i < PLACEHOLDER_ROWS; i += 1) {
        const tr = document.createElement('tr');
        tr.className = 'is-placeholder';
        for (let c = 0; c < columns; c += 1) tr.append(document.createElement('td'));
        for (const td of tr.children) td.textContent = '--';
        body.append(tr);
      }
      built = -1;
      pageStart = null;
    }

    return function show(view) {
      if (view && view.label) chrome.setText(pageId, view.label);
      const rows = (view && view.rows) || [];
      if (!view || !view.loaded || !rows.length) {
        placeholder();
        return;
      }
      if (pageStart !== view.start || built !== rows.length) {
        body.replaceChildren();
        for (let i = 0; i < rows.length; i += 1) {
          const tr = document.createElement('tr');
          for (let c = 0; c < columns; c += 1) tr.append(document.createElement('td'));
          body.append(tr);
        }
        built = rows.length;
        pageStart = view.start;
      }
      for (let i = 0; i < built; i += 1) {
        const cells = rows[i] || [];
        const tr = body.children[i];
        for (let c = 0; c < columns; c += 1) {
          const td = tr.children[c];
          const text = cells[c] === undefined ? '--' : String(cells[c]);
          if (td.textContent !== text) td.textContent = text;
        }
      }
    };
  }

  const tables = {
    regions: table('regions-list', 'regions-page', 6),
    modules: table('modules-list', 'modules-page', 4),
    threads: table('threads-list', 'threads-page', 5),
  };

  function load(model, kind) {
    if (requested[kind]) return;
    requested[kind] = true;
    const view = model && model.views ? model.views[kind] : null;
    if (view && view.loaded) return;
    ctx.run('view.load', { kind });
  }

  function activate(model, index) {
    if (Number.isInteger(index)) {
      const kind = KINDS[index - FIRST_PAGE];
      if (kind) load(model, kind);
      return;
    }
    // Called without a rail index: every view still gets its one load.
    for (const kind of KINDS) load(model, kind);
  }

  function render(model) {
    const section = model.views || {};
    tables.regions(section.regions);
    tables.modules(section.modules);
    tables.threads(section.threads);
  }

  return { render, activate };
}
