// The Memory preview page: a 256-byte snapshot you can edit in place.
//
// The grid is static markup and every byte carries its own data-offset, so the
// page paints from the payload's byte string and never re-creates nodes. What a
// cell SHOWS depends on the display mode: a hex cell is a sequence of bytes in
// address order, and typing replaces one nibble at a time, high nibble first,
// committing only when the last digit lands. Decimal and float cells instead
// stage a literal that Enter commits and Escape throws away -- the same split
// the native control had, because a numeric literal cannot be typed
// nibble-by-nibble without every intermediate keystroke being a value.
import * as chrome from './chrome.js';
import { cellText, layout, placeDigit, digitValue } from './hexview.js';

const BYTES = 256;
const PAGE = 256;

export function createMemoryPage(ctx) {
  const hexCells = new Array(BYTES);
  const asciiCells = new Array(BYTES);
  const grid = ctx.node('memory-hex');
  if (grid) {
    for (const cell of grid.querySelectorAll('.hex-cell')) {
      hexCells[Number(cell.dataset.offset)] = cell;
    }
    for (const cell of grid.querySelectorAll('.ascii-cell')) {
      asciiCells[Number(cell.dataset.offset)] = cell;
    }
    grid.tabIndex = 0;
  }

  let page = null;          // { base: BigInt, bytes: Uint8Array, readable: string }
  let caret = -1;           // byte offset of the selected cell
  let staged = null;        // { offset, bytes: Uint8Array, digits, text }
  let selection = null;     // { from, to } byte offsets, inclusive
  let dragging = false;

  const modeIndex = () => Number(ctx.value('memory-type')) || 0;
  const shape = () => layout(modeIndex());

  // ---- rendering ----------------------------------------------------------------

  function cellBytes(offset, count) {
    const out = new Uint8Array(count);
    for (let i = 0; i < count; i += 1) out[i] = page ? page.bytes[offset + i] : 0;
    return out;
  }

  function unreadable(offset) {
    return !page || page.readable.charAt(offset) !== '1';
  }

  /** Every byte of a cell has to be readable before the cell can be edited. */
  function cellEditable(offset, count) {
    for (let i = 0; i < count; i += 1) if (unreadable(offset + i)) return false;
    return true;
  }

  function inSelection(offset) {
    return !!selection && offset >= selection.from && offset <= selection.to;
  }

  function paintCell(offset, count) {
    const cell = hexCells[offset];
    if (!cell) return;
    const stagedHere = !!staged && staged.offset === offset;
    const area = shape().digitArea;
    let text;
    // An unreadable cell carries no value, so it shows what the page's own hint
    // promises rather than a zero that was never there.
    const blank = page && !cellEditable(offset, count);
    if (blank) {
      text = '?'.repeat(Math.max(2, shape().cellWidth - 1));
    } else if (stagedHere && area && staged.digits > 0) {
      text = cellText({ ...shape().mode, hexadecimal: true }, staged.bytes);
    } else if (stagedHere && !area && staged.text) {
      text = staged.text.toUpperCase();
    } else {
      text = page ? cellText(shape().mode, cellBytes(offset, count)) : '--';
    }
    if (cell.textContent !== text) cell.textContent = text;
    const bad = !page || !cellEditable(offset, count);
    cell.classList.toggle('unreadable', bad);
    cell.classList.toggle('is-unreadable', bad);
    cell.classList.toggle('staged', stagedHere);
    cell.classList.toggle('is-staged', stagedHere);
    cell.classList.toggle('selected', caret === offset);
    cell.classList.toggle('is-selected-range', inSelection(offset));
  }

  function paintAscii(offset) {
    const cell = asciiCells[offset];
    if (!cell) return;
    const byte = page ? page.bytes[offset] : 0;
    const text = !page ? '.' : (unreadable(offset) ? '?' : (byte < 32 || byte > 126 ? '.' : String.fromCharCode(byte)));
    if (cell.textContent !== text) cell.textContent = text;
    const bad = unreadable(offset);
    cell.classList.toggle('unreadable', bad);
    cell.classList.toggle('is-unreadable', bad);
    cell.classList.toggle('is-selected-range', inSelection(offset));
  }

  function paintAddresses() {
    if (!grid || !page) return;
    grid.querySelectorAll('.hex-addr').forEach((element, row) => {
      const text = `0x${(page.base + BigInt(row * 16)).toString(16).toUpperCase()
        .padStart(16, '0')}`;
      if (element.textContent !== text) element.textContent = text;
    });
  }

  /**
   * One pass over the grid with the current mode. A cell that covers several
   * bytes spans that many columns and its siblings are hidden, so the fixed
   * 16-column hex area still fills exactly one row per 16 bytes.
   */
  function paintAll() {
    const { bytesPerCell: per, cellsPerRow } = shape();
    for (let group = 0; group < cellsPerRow; group += 1) {
      const offset = group * per;
      for (let i = 0; i < per; i += 1) {
        const cell = hexCells[offset + i];
        if (!cell) continue;
        cell.style.gridColumn = i === 0 ? `span ${per}` : '';
        cell.style.display = i === 0 ? '' : 'none';
      }
      paintCell(offset, per);
    }
    for (let i = 0; i < BYTES; i += 1) paintAscii(i);
    paintAddresses();
  }

  function show(data) {
    if (!data) return;
    const bytes = typeof data.bytes === 'string' ? data.bytes : '';
    const readable = typeof data.readable === 'string' ? data.readable : '';
    const buffer = new Uint8Array(BYTES);
    for (let i = 0; i < BYTES; i += 1) {
      buffer[i] = parseInt(bytes.slice(i * 2, i * 2 + 2), 16) || 0;
    }
    page = { base: BigInt(data.page || 0), bytes: buffer, readable };
    caret = -1;
    staged = null;
    selection = null;
    chrome.setText('memory-page', data.label || '');
    const missing = typeof data.unreadable === 'number'
      ? data.unreadable
      : (readable.match(/0/g) || []).length;
    chrome.setText('memory-hex-foot',
      `256 bytes requested; ${missing} unreadable. Snapshot only; no disassembly.`);
    paintAll();
  }

  // ---- editing -------------------------------------------------------------------

  const cellAt = (offset) => offset - (offset % shape().bytesPerCell);

  function selectCell(offset) {
    const target = cellAt(offset);
    if (target < 0 || target >= BYTES || !page) return;
    if (caret !== target) {
      caret = target;
      // A new cell drops whatever the previous one had staged: those digits
      // belong to the cell they were typed into.
      staged = null;
    }
    if (grid) grid.focus();
  }

  function reload() {
    if (page) ctx.run('memory.page', { address: String(page.base) }, show);
  }

  function commitHex() {
    if (!staged || !page) return;
    const address = page.base + BigInt(staged.offset);
    const offset = staged.offset;
    const written = staged.bytes.slice();
    let hex = '';
    for (const byte of written) hex += byte.toString(16).padStart(2, '0');
    ctx.run('memory.writeBytes', { address: String(address), hex }, () => {
      // Adopt what was just written: the reply carries no page, and re-reading
      // would only report what we already know.
      page.bytes.set(written, offset);
      staged = null;
      paintAll();
    });
  }

  function typeHexDigit(character) {
    const digit = digitValue(character);
    if (digit < 0 || !page || caret < 0) return false;
    const { bytesPerCell: per, digitArea } = shape();
    if (!digitArea || !cellEditable(caret, per)) return false;
    if (!staged || staged.offset !== caret) {
      staged = { offset: caret, bytes: cellBytes(caret, per), digits: 0, text: '' };
    }
    placeDigit(staged.bytes, staged.digits, digit);
    staged.digits += 1;
    if (staged.digits >= digitArea) {
      // The cell is written once its last digit lands, so a half-typed value
      // never reaches memory.
      paintAll();
      commitHex();
      caret = cellAt(caret + per);
      return true;
    }
    paintAll();
    return true;
  }

  const TEXT_CHARS = /^[0-9+\-.eEaAfFiInN]$/;

  function typeTextChar(character) {
    if (!page || caret < 0) return false;
    const { bytesPerCell: per, digitArea } = shape();
    if (digitArea || !cellEditable(caret, per)) return false;
    if (!staged || staged.offset !== caret) {
      staged = { offset: caret, bytes: cellBytes(caret, per), digits: 0, text: '' };
    }
    if (staged.text.length >= 40) return true;
    if (!TEXT_CHARS.test(character)) return true;   // swallowed, not a value
    staged.text += character;
    paintAll();
    return true;
  }

  function commitText() {
    if (!staged || !page || shape().digitArea || !staged.text) return;
    const { mode: display, bytesPerCell: per } = shape();
    const request = {
      address: String(page.base + BigInt(staged.offset)),
      type: display.type,
      flags: 1,                      // CE_VALUE_SIGNED: the decimal modes are signed
      byteLength: String(per),
      text: staged.text,
    };
    staged = null;
    // The literal may encode to any bit pattern, so ask for the page again
    // rather than guessing what it wrote.
    ctx.run('memory.writeValue', request, reload);
  }

  function moveCaret(delta) {
    if (caret < 0) {
      selectCell(0);
      return;
    }
    selectCell(caret + delta);
  }

  function onKeyDown(event) {
    if (!page) return;
    if (event.ctrlKey && (event.key === 'z' || event.key === 'Z')) {
      event.preventDefault();
      ctx.run('memory.undo', undefined, reload);
      return;
    }
    if (event.key === 'Escape') {
      if (staged) {
        staged = null;
        paintAll();
      }
      event.preventDefault();
      return;
    }
    if (event.key === 'Enter') {
      commitText();
      event.preventDefault();
      return;
    }
    const per = shape().bytesPerCell;
    if (event.key === 'ArrowLeft') { moveCaret(-per); paintAll(); event.preventDefault(); return; }
    if (event.key === 'ArrowRight') { moveCaret(per); paintAll(); event.preventDefault(); return; }
    if (event.key === 'ArrowUp') { moveCaret(-16); paintAll(); event.preventDefault(); return; }
    if (event.key === 'ArrowDown') { moveCaret(16); paintAll(); event.preventDefault(); return; }
    if (event.key.length !== 1) return;
    if (shape().digitArea ? typeHexDigit(event.key) : typeTextChar(event.key)) {
      event.preventDefault();
    }
  }

  // ---- pointing ------------------------------------------------------------------

  function byteFromEvent(event) {
    const element = event.target.closest('.hex-cell, .ascii-cell');
    if (!element) return -1;
    const offset = Number(element.dataset.offset);
    return Number.isFinite(offset) ? offset : -1;
  }

  function onPointerDown(event) {
    if (event.button !== 0 || !page) return;
    const offset = byteFromEvent(event);
    if (offset < 0) return;
    event.preventDefault();
    if (event.shiftKey && caret >= 0) {
      selection = { from: Math.min(caret, offset), to: Math.max(caret, offset) };
    } else {
      selection = null;
      dragging = true;
      selectCell(offset);
    }
    paintAll();
  }

  function onPointerMove(event) {
    if (!dragging || !page) return;
    const offset = byteFromEvent(event);
    if (offset < 0 || offset === caret) return;
    selection = { from: Math.min(caret, offset), to: Math.max(caret, offset) };
    paintAll();
  }

  function onPointerUp() {
    dragging = false;
  }

  function fillSelection() {
    if (!page || !selection) {
      ctx.run('memory.fill', {});   // no selection: let the runtime say why
      return;
    }
    const from = selection.from;
    const to = selection.to;
    const fill = parseInt(ctx.value('memory-fill-byte'), 16) || 0;
    ctx.run('memory.fill', {
      address: String(page.base + BigInt(from)),
      length: String(to - from + 1),
      byte: ctx.value('memory-fill-byte'),
    }, () => {
      for (let i = from; i <= to; i += 1) page.bytes[i] = fill;
      paintAll();
    });
  }

  if (grid) {
    grid.addEventListener('mousedown', onPointerDown);
    grid.addEventListener('mousemove', onPointerMove);
    window.addEventListener('mouseup', onPointerUp);
    grid.addEventListener('keydown', onKeyDown);
    grid.addEventListener('contextmenu', (event) => event.preventDefault());
  }

  function own(id, handler) {
    const target = ctx.node(id);
    if (target) target.addEventListener('click', handler);
  }

  function resolve() {
    // Values always come fresh off the form; the runtime keeps no copy of it.
    ctx.run('memory.resolve', {
      base: ctx.value('memory-base'),
      offsets: ctx.value('memory-offsets'),
    }, show);
  }

  // These commands answer with the page they just resolved, so the buttons
  // belong here rather than in app.js's shared wiring.
  own('memory-resolve', resolve);
  own('memory-refresh', resolve);
  own('memory-prev', () => ctx.run('memory.move', { delta: -PAGE }, show));
  own('memory-next', () => ctx.run('memory.move', { delta: PAGE }, show));
  own('memory-back', () => ctx.run('memory.back', undefined, show));
  own('memory-forward', () => ctx.run('memory.forward', undefined, show));
  own('memory-undo', () => ctx.run('memory.undo', undefined, reload));
  own('memory-fill', fillSelection);

  const typeCombo = ctx.node('memory-type');
  if (typeCombo) {
    typeCombo.addEventListener('change', () => {
      // A different cell width invalidates the caret and anything staged.
      caret = -1;
      staged = null;
      selection = null;
      if (page) paintAll();
    });
  }

  function render(model) {
    // Nothing has been resolved yet: mirror only the runtime's own label and
    // leave the grid at its static markup.
    if (!page && model.memory && model.memory.label) {
      chrome.setText('memory-page', model.memory.label);
    }
  }

  return { render, activate() {} };
}
