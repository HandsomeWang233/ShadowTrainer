// Geometry and value formatting for the memory page's hex view.
//
// Ported from src/ui_hexedit.hpp (the layout tables and the hit test) and from
// the old UI's hex_cell_text() (the value formatting), so the numbers on screen
// are the same numbers the native build produced.
//
// A hex cell is a SEQUENCE OF BYTES IN ADDRESS ORDER, not a number: typing
// 12345678 into a 4-byte cell leaves 12 34 56 78 in memory, which is the
// opposite of parsing it as the integer 0x12345678 and storing it
// little-endian. Everything here follows that rule -- bytes are read and
// written big-endian, and only the decimal/float modes reinterpret them.
import { CE_TYPE } from './format.js';

export const BYTES_PER_ROW = 16;
export const ADDRESS_WIDTH = 18;          // "0x" plus sixteen digits
export const HEX_COLUMN = ADDRESS_WIDTH + 2;

// Mirrors the display-mode combo and the table in ui_hexedit.hpp, in order.
export const MODES = Object.freeze([
  Object.freeze({ label: 'Byte', type: CE_TYPE.U8, hexadecimal: true }),
  Object.freeze({ label: 'Byte (decimal)', type: CE_TYPE.U8, hexadecimal: false }),
  Object.freeze({ label: '2 Bytes', type: CE_TYPE.U16, hexadecimal: true }),
  Object.freeze({ label: '2 Bytes (decimal)', type: CE_TYPE.U16, hexadecimal: false }),
  Object.freeze({ label: '4 Bytes', type: CE_TYPE.U32, hexadecimal: true }),
  Object.freeze({ label: '4 Bytes (decimal)', type: CE_TYPE.U32, hexadecimal: false }),
  Object.freeze({ label: '8 Bytes', type: CE_TYPE.U64, hexadecimal: true }),
  Object.freeze({ label: '8 Bytes (decimal)', type: CE_TYPE.U64, hexadecimal: false }),
  // Float and Double are values, not digits: `hexadecimal` is false so the cell
  // has no digit area and stages a literal instead. The native table passed true
  // here, which gave the cell a digit area and made its own "%.9g" branch
  // unreachable -- the cell widths were already sized for that branch.
  Object.freeze({ label: 'Float', type: CE_TYPE.FLOAT, hexadecimal: false }),
  Object.freeze({ label: 'Double', type: CE_TYPE.DOUBLE, hexadecimal: false }),
]);

export function bytesPerCell(mode) {
  switch (mode.type) {
    case CE_TYPE.U16: return 2;
    case CE_TYPE.U32: return 4;
    case CE_TYPE.U64: return 8;
    case CE_TYPE.FLOAT: return 4;
    case CE_TYPE.DOUBLE: return 8;
    default: return 1;
  }
}

function cellWidth(mode) {
  switch (mode.type) {
    case CE_TYPE.U16: return mode.hexadecimal ? 5 : 7;
    case CE_TYPE.U32: return mode.hexadecimal ? 9 : 12;
    case CE_TYPE.U64: return mode.hexadecimal ? 17 : 21;
    case CE_TYPE.FLOAT: return 16;    // "%.9g" worst case
    case CE_TYPE.DOUBLE: return 25;   // "%.17g" worst case
    default: return mode.hexadecimal ? 3 : 5;
  }
}

export function layout(index) {
  const mode = MODES[index] || MODES[0];
  const per = bytesPerCell(mode);
  const cells = BYTES_PER_ROW / per;
  const width = cellWidth(mode);
  return {
    mode,
    bytesPerCell: per,
    cellsPerRow: cells,
    cellWidth: width,
    digitArea: mode.hexadecimal ? per * 2 : 0,
    hexColumn: HEX_COLUMN,
    asciiColumn: HEX_COLUMN + cells * width + 1,
    rowWidth: HEX_COLUMN + cells * width + 1 + BYTES_PER_ROW,
  };
}

/**
 * Big-endian integer from `count` bytes. `signed` sign-extends: for widths below
 * eight bytes from the byte's own top bit, and for eight bytes from the top bit
 * of the whole value, which is what a C `long long` cast does.
 */
function integerOf(bytes, count, signed) {
  let value = 0n;
  for (let i = 0; i < count; i += 1) value = (value << 8n) | BigInt(bytes[i] & 0xff);
  if (!signed) return value;
  if (count < 8) return (bytes[0] & 0x80) ? value - (1n << BigInt(count * 8)) : value;
  return value >= (1n << 63n) ? value - (1n << 64n) : value;
}

/**
 * C's "%.*g": significant digits, trailing zeros stripped, and exponential form
 * only when the exponent falls outside [-4, precision).
 */
export function formatG(value, precision) {
  if (!Number.isFinite(value)) return String(value);
  if (value === 0) return '0';
  const exponent = Math.floor(Math.log10(Math.abs(value)));
  if (exponent < -4 || exponent >= precision) {
    return value.toExponential(precision - 1).replace(/\.?0+e/, 'e');
  }
  const decimals = Math.max(0, precision - 1 - exponent);
  return value.toFixed(decimals).replace(/\.?0+$/, '');
}

/** The text one cell shows. `bytes` is the cell in address order. */
export function cellText(mode, bytes) {
  const width = bytes.length;
  if (mode.hexadecimal) {
    const value = integerOf(bytes, width, false);
    const digits = value.toString(16).toUpperCase().padStart(width * 2, '0');
    return digits.slice(-(width * 2));
  }
  if (mode.type === CE_TYPE.FLOAT || mode.type === CE_TYPE.DOUBLE) {
    const view = new DataView(new ArrayBuffer(width));
    for (let i = 0; i < width; i += 1) view.setUint8(i, bytes[i] & 0xff);
    const value = mode.type === CE_TYPE.FLOAT ? view.getFloat32(0, false) : view.getFloat64(0, false);
    return formatG(value, mode.type === CE_TYPE.FLOAT ? 9 : 17);
  }
  return integerOf(bytes, width, true).toString(10);
}

export function digitValue(character) {
  if (character >= '0' && character <= '9') return character.charCodeAt(0) - 48;
  if (character >= 'a' && character <= 'f') return character.charCodeAt(0) - 87;
  if (character >= 'A' && character <= 'F') return character.charCodeAt(0) - 55;
  return -1;
}

/**
 * Places one hex digit at `index` of an N-byte cell: digit 0 and 1 are the high
 * and low nibble of the FIRST byte, so the bytes stay in address order.
 */
export function placeDigit(bytes, index, digit) {
  const cell = index >> 1;
  if (cell >= bytes.length || digit < 0 || digit > 15) return;
  bytes[cell] = index % 2 === 0
    ? ((digit << 4) | (bytes[cell] & 0x0f)) & 0xff
    : ((bytes[cell] & 0xf0) | digit) & 0xff;
}

