// The three process views: Regions, Modules and Threads.
//
// One module serves all three rail pages. Each view is a snapshot the runtime
// keeps on its side and pushes as a slice of already-formatted row arrays, so
// this page only paints `views[kind]`. Entering a page loads its view once;
// paging and sorting afterwards are the runtime's, so rows cannot shift under
// the reader.
//
// The module table is also the one table with a verb: a selected row can be
// unloaded from the host. The runtime refuses the host executable and this DLL,
// and the page asks before sending the command.
import * as chrome from './chrome.js';
import { ask } from './modal.js';
import { install as installSort, paint as paintSort } from './sort.js';

const PLACEHOLDER_ROWS = 3;
const KINDS = ['regions', 'modules', 'threads'];
const FIRST_PAGE = 3;    // rail index of Regions

export function createViewsPage(ctx) {
  const requested = { regions: false, modules: false, threads: false };
  let idle = true;        // the session state the unload button follows
  let moduleIds = [];     // the base each module row names, parallel to its rows
  let moduleRows = [];    // the cells of those rows, for the confirmation text
  let moduleBase = null;  // the selected module's base, or null

  function modulesBody() {
    const list = ctx.node('modules-list');
    return list ? list.querySelector('tbody') : null;
  }

  function updateUnload() {
    const button = ctx.node('modules-unload');
    if (button) button.disabled = !(idle && moduleBase !== null);
  }

  function clearModuleSelection() {
    moduleBase = null;
    const body = modulesBody();
    if (body) for (const tr of body.children) tr.classList.remove('is-selected');
    updateUnload();
  }

  function selectModule(index) {
    const id = moduleIds[index];
    moduleBase = id === undefined ? null : String(id);
    const body = modulesBody();
    if (body) {
      for (let i = 0; i < body.children.length; i += 1) {
        body.children[i].classList.toggle('is-selected', String(moduleIds[i]) === moduleBase);
      }
    }
    updateUnload();
  }

  function table(listId, pageId, columns, kind, hooks = {}) {
    const list = ctx.node(listId);
    const body = list.querySelector('tbody');
    let built = -1;          // how many rows the DOM currently holds
    let pageStart = null;    // the page those rows belong to

    installSort(list, (column) => ctx.run('view.sort', { kind, column }, (data) => {
      if (data) show(data);
    }));

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

    function show(view) {
      if (view && view.label) chrome.setText(pageId, view.label);
      if (view) paintSort(list, view.sortColumn, view.sortDescending);
      if (hooks.ids) hooks.ids(view);
      const rows = (view && view.rows) || [];
      if (!view || !view.loaded || !rows.length) {
        placeholder();
        return;
      }
      if (pageStart !== view.start || built !== rows.length) {
        body.replaceChildren();
        // New rows are different rows, so whatever was picked among the old ones
        // is gone: a selection is a row on screen, not a position in a list.
        if (hooks.rebuild) hooks.rebuild();
        for (let i = 0; i < rows.length; i += 1) {
          const tr = document.createElement('tr');
          for (let c = 0; c < columns; c += 1) tr.append(document.createElement('td'));
          if (hooks.row) hooks.row(tr, i);
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
    }

    return show;
  }

  const tables = {
    regions: table('regions-list', 'regions-page', 6, 'regions'),
    threads: table('threads-list', 'threads-page', 5, 'threads'),
    modules: table('modules-list', 'modules-page', 4, 'modules', {
      ids: (view) => {
        moduleIds = (view && view.ids) || [];
        moduleRows = (view && view.rows) || [];
        // A view that is gone or reloaded takes the selection with it.
        if (!view || !view.loaded) clearModuleSelection();
      },
      rebuild: () => {
        moduleBase = null;
        updateUnload();
      },
      row: (tr, index) => tr.addEventListener('click', () => selectModule(index)),
    }),
  };

  const unload = ctx.node('modules-unload');
  if (unload) {
    unload.addEventListener('click', async () => {
      if (moduleBase === null) return;
      const cells = moduleRows[moduleIds.indexOf(moduleBase)] || [];
      const name = cells[2] === undefined ? 'the selected module' : String(cells[2]);
      const path = cells[3] === undefined ? '' : String(cells[3]);
      // Deleting the reference a live process is running on is not undoable, so
      // the click alone is not the decision - and the path is what makes the
      // question unambiguous, since two modules can share a file name.
      const agreed = await ask({
        title: `Unload ${name}?`,
        body: `${path}\n\nFreeLibrary runs inside the host process, and a module ` +
              'it is still using can take the whole process down. There is no undo.',
        confirmLabel: 'Unload',
      });
      if (!agreed || moduleBase === null) return;
      ctx.run('view.unload', { kind: 'modules', base: moduleBase }, (data) => {
        clearModuleSelection();
        if (data) tables.modules(data);
      });
    });
  }

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
    const controls = (model && model.controls) || {};
    idle = !!controls.idle;
    const section = (model && model.views) || {};
    tables.regions(section.regions);
    tables.modules(section.modules);
    tables.threads(section.threads);
    updateUnload();
  }

  return { render, activate };
}
