// Click-to-sort headers for the data tables.
//
// The runtime owns the order: it holds the whole list (a view) or the order that
// stands in for one (the address list, the pointer results), so the page only
// reports which column was clicked and paints the marker that comes back.
// Sorting the rows the browser happens to hold would order the window on screen,
// not the table.
//
// The marker is the `aria-sort` attribute, which is also what a screen reader
// reads; the caret is drawn from it in the stylesheet.

/** Which way a header runs. `sortColumn` of -1 means nothing is sorted. */
export function arrow(column, sortColumn, descending) {
  if (sortColumn === undefined || sortColumn === null) return 'none';
  if (column !== Number(sortColumn)) return 'none';
  return descending ? 'descending' : 'ascending';
}

/** Every header of `table` becomes a button; `onSort(column)` runs on a click. */
export function install(table, onSort) {
  if (!table) return;
  table.querySelectorAll('thead th').forEach((th, column) => {
    th.classList.add('is-sortable');
    th.addEventListener('click', () => onSort(column));
  });
}

/** Paints the runtime's answer onto the headers. */
export function paint(table, sortColumn, descending) {
  if (!table) return;
  table.querySelectorAll('thead th').forEach((th, column) => {
    const state = arrow(column, sortColumn, descending);
    if (state === 'none') th.removeAttribute('aria-sort');
    else th.setAttribute('aria-sort', state);
  });
}
