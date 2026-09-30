// Port of tests/hex_geometry_tests.cpp, plus the parts of the editor that used
// to live in C++: the column tables, the address-order nibble rule, and the
// value formatting of every display mode.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import {
  MODES, layout, cellText, digitValue, placeDigit, formatG, bytesPerCell,
} from '../../web/js/hexview.js';
import { CE_TYPE } from '../../web/js/format.js';

test('byte hex keeps the shipped columns', () => {
  const box = layout(0);
  expect(box.bytesPerCell).toBe(1);
  expect(box.cellWidth).toBe(3);
  expect(box.digitArea).toBe(2);
  expect(box.cellsPerRow).toBe(16);
  expect(box.hexColumn).toBe(20);
  expect(box.asciiColumn).toBe(69);
  expect(box.rowWidth).toBe(85);
});

test('every mode tiles its row', () => {
  for (const mode of MODES) {
    const box = layout(MODES.indexOf(mode));
    expect(box.cellsPerRow * box.bytesPerCell).toBe(16);
    expect(box.hexColumn).toBe(20);
    expect(box.asciiColumn).toBe(box.hexColumn + box.cellsPerRow * box.cellWidth + 1);
    expect(box.rowWidth).toBe(box.asciiColumn + 16);
    expect(box.digitArea).toBe(mode.hexadecimal ? box.bytesPerCell * 2 : 0);
  }
});

test('the published cell widths are unchanged', () => {
  // The table from src/ui_hexedit.hpp: mode order is the combo's order.
  const widths = [3, 5, 5, 7, 9, 12, 17, 21, 16, 25];
  MODES.forEach((mode, index) => {
    expect(layout(index).cellWidth).toBe(widths[index]);
  });
});

test('an eight-digit cell is a byte sequence in address order', () => {
  // Typing 12345678 into a four-byte cell must leave 12 34 56 78 in memory, not
  // 78 56 34 12 as a little-endian integer would.
  const bytes = new Uint8Array(4);
  const typed = '12345678';
  for (let i = 0; i < 8; i += 1) placeDigit(bytes, i, digitValue(typed[i]));
  expect([...bytes]).toEqual([0x12, 0x34, 0x56, 0x78]);

  // One digit replaces exactly one nibble.
  placeDigit(bytes, 3, 0xf);
  expect([...bytes]).toEqual([0x12, 0x3f, 0x56, 0x78]);

  // Out-of-range indices and values are ignored rather than corrupting a byte.
  const before = [...bytes];
  placeDigit(bytes, 8, 5);
  placeDigit(bytes, 0, 99);
  expect([...bytes]).toEqual(before);
});

test('hex cells are formatted big-endian and padded to the cell', () => {
  expect(cellText({ type: CE_TYPE.U8, hexadecimal: true }, [0x0a])).toBe('0A');
  expect(cellText({ type: CE_TYPE.U16, hexadecimal: true }, [0x12, 0x34])).toBe('1234');
  expect(cellText({ type: CE_TYPE.U32, hexadecimal: true }, [1, 2, 3, 4])).toBe('01020304');
  expect(cellText({ type: CE_TYPE.U16, hexadecimal: true }, [0, 7])).toBe('0007');
});

test('decimal cells are signed and follow the byte order', () => {
  expect(cellText({ type: CE_TYPE.U8, hexadecimal: false }, [0xff])).toBe('-1');
  expect(cellText({ type: CE_TYPE.U8, hexadecimal: false }, [0x7f])).toBe('127');
  expect(cellText({ type: CE_TYPE.U16, hexadecimal: false }, [0xff, 0xfe])).toBe('-2');
  expect(cellText({ type: CE_TYPE.U16, hexadecimal: false }, [0x12, 0x34])).toBe('4660');
  expect(cellText({ type: CE_TYPE.U64, hexadecimal: false },
    [0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff])).toBe('-1');
});

test('float and double show a value, not a bit pattern', () => {
  const single = MODES.find((mode) => mode.label === 'Float');
  const double = MODES.find((mode) => mode.label === 'Double');
  expect(single.hexadecimal).toBe(false);
  expect(double.hexadecimal).toBe(false);
  // Big-endian 1.0f and 1.0.
  expect(cellText(single, [0x3f, 0x80, 0x00, 0x00])).toBe('1');
  expect(cellText(double, [0x3f, 0xf0, 0, 0, 0, 0, 0, 0])).toBe('1');
  // The float maximum, whose %.9g form is what sized the 16-column cell.
  expect(cellText(single, [0x7f, 0x7f, 0xff, 0xff])).toBe('3.40282347e+38');
  // A negative float, which the decimal integer modes could not express.
  expect(cellText(single, [0xbf, 0x80, 0x00, 0x00])).toBe('-1');
});

test('formatG matches C %g', () => {
  expect(formatG(1, 9)).toBe('1');
  expect(formatG(0, 9)).toBe('0');
  expect(formatG(100, 9)).toBe('100');
  expect(formatG(1.5, 9)).toBe('1.5');
  expect(formatG(0.0001, 9)).toBe('0.0001');
  expect(formatG(0.00001, 9)).toBe('1e-5');
  expect(formatG(1234567890, 9)).toBe('1.23456789e+9');
  expect(formatG(-3.40282347e38, 9)).toBe('-3.40282347e+38');
  expect(formatG(NaN, 9)).toBe('NaN');
  expect(formatG(Infinity, 9)).toBe('Infinity');
});

test('bytesPerCell follows the display mode', () => {
  expect(MODES.map(bytesPerCell)).toEqual([1, 1, 2, 2, 4, 4, 8, 8, 4, 8]);
});
