// Frozen enum mirrors for the CE web UI.
//
// Every table below mirrors include/ce/api_v2.h; STATUS_NAMES mirrors the
// CeStatus enum in include/ce/api.h. tests/web/enums.test.js parses those
// headers and asserts these tables stay in sync, so both sides move together.
//
// Pure data plus one pure formatter: importing this module has no side effects
// and touches no DOM. Nothing here is allowed to depend on the host page.

// CeValueTypeV2: CE_TYPE_U8 .. CE_TYPE_AOB (0..8).
export const VALUE_TYPES = Object.freeze([
  Object.freeze({ value: 0, label: 'Byte' }),
  Object.freeze({ value: 1, label: '2 Bytes' }),
  Object.freeze({ value: 2, label: '4 Bytes' }),
  Object.freeze({ value: 3, label: '8 Bytes' }),
  Object.freeze({ value: 4, label: 'Float' }),
  Object.freeze({ value: 5, label: 'Double' }),
  Object.freeze({ value: 6, label: 'UTF-8' }),
  Object.freeze({ value: 7, label: 'UTF-16' }),
  Object.freeze({ value: 8, label: 'Array of bytes' }),
]);

// The same values as bare constants: rule code reads better as CE_CMP.BETWEEN
// than as a magic 4.
export const CE_TYPE = Object.freeze({
  U8: 0, U16: 1, U32: 2, U64: 3, FLOAT: 4, DOUBLE: 5, UTF8: 6, UTF16: 7, AOB: 8,
});

// The core's own width table: a fixed-width type ignores the "Bytes" field, so
// the field is only usable for the variable-width ones.
export const FIXED_WIDTH = Object.freeze([1, 2, 4, 8, 4, 8, 0, 0, 0]);

// CeCompareV2: CE_CMP_EXACT .. CE_CMP_DECREASED_BY (0..10).
export const COMPARISONS = Object.freeze([
  Object.freeze({ value: 0, label: 'Exact' }),
  Object.freeze({ value: 1, label: 'Unknown initial' }),
  Object.freeze({ value: 2, label: 'Greater than' }),
  Object.freeze({ value: 3, label: 'Less than' }),
  Object.freeze({ value: 4, label: 'Between (inclusive)' }),
  Object.freeze({ value: 5, label: 'Changed' }),
  Object.freeze({ value: 6, label: 'Unchanged' }),
  Object.freeze({ value: 7, label: 'Increased' }),
  Object.freeze({ value: 8, label: 'Decreased' }),
  Object.freeze({ value: 9, label: 'Increased by' }),
  Object.freeze({ value: 10, label: 'Decreased by' }),
]);

export const CE_CMP = Object.freeze({
  EXACT: 0, UNKNOWN: 1, GREATER: 2, LESS: 3, BETWEEN: 4, CHANGED: 5,
  UNCHANGED: 6, INCREASED: 7, DECREASED: 8, INCREASED_BY: 9, DECREASED_BY: 10,
});

// CeRoundingV2: CE_ROUND_EXACT .. CE_ROUND_TRUNCATED (0..3).
export const ROUNDINGS = Object.freeze([
  Object.freeze({ value: 0, label: 'Exact' }),
  Object.freeze({ value: 1, label: 'Rounded' }),
  Object.freeze({ value: 2, label: 'Rounded (extreme)' }),
  Object.freeze({ value: 3, label: 'Truncated' }),
]);

// Alignment steps accepted by CeScanRequestV2.alignment and by the pointer scan.
export const ALIGNMENTS = Object.freeze([
  Object.freeze({ value: 1, label: '1 byte(s)' }),
  Object.freeze({ value: 2, label: '2 byte(s)' }),
  Object.freeze({ value: 4, label: '4 byte(s)' }),
  Object.freeze({ value: 8, label: '8 byte(s)' }),
]);

// Memory preview display modes, the Ctrl+1..0 order: value 0 is a Byte shown as
// hex, value 1 a Byte shown as signed decimal, and so on. Ten modes, values 0..9.
export const HEX_MODES = Object.freeze([
  Object.freeze({ value: 0, label: 'Byte' }),
  Object.freeze({ value: 1, label: 'Byte (decimal)' }),
  Object.freeze({ value: 2, label: '2 Bytes' }),
  Object.freeze({ value: 3, label: '2 Bytes (decimal)' }),
  Object.freeze({ value: 4, label: '4 Bytes' }),
  Object.freeze({ value: 5, label: '4 Bytes (decimal)' }),
  Object.freeze({ value: 6, label: '8 Bytes' }),
  Object.freeze({ value: 7, label: '8 Bytes (decimal)' }),
  Object.freeze({ value: 8, label: 'Float' }),
  Object.freeze({ value: 9, label: 'Double' }),
]);

// Result rows per page for every paging row in the shell.
export const PAGE_SIZE = 512;

// CE_FormatValueV2 text budget the address list renders per record value.
export const CURRENT_VALUE_LIMIT = 256;

// CE_V2_MAX_POINTER_LEVELS / CE_V2_MAX_OFFSETS.
export const MAX_POINTER_LEVELS = 8;
export const MAX_OFFSETS = 16;

// Left rail order. index i drives #page-* order, nav order and the banner text.
export const PAGES = Object.freeze([
  Object.freeze({ nav: 'Scan', banner: 'Memory scan' }),
  Object.freeze({ nav: 'Address list', banner: 'Address library' }),
  Object.freeze({ nav: 'Memory preview', banner: 'Memory preview' }),
  Object.freeze({ nav: 'Regions', banner: 'Process regions' }),
  Object.freeze({ nav: 'Modules', banner: 'Loaded modules' }),
  Object.freeze({ nav: 'Threads', banner: 'Host threads' }),
  Object.freeze({ nav: 'Pointer scan', banner: 'Pointer scan' }),
]);

// CeStatus, index == code.
export const STATUS_NAMES = Object.freeze([
  'OK',                      // CE_OK = 0
  'Invalid argument',        // CE_INVALID_ARGUMENT = 1
  'Session is not running',  // CE_NOT_RUNNING = 2
  'Busy',                    // CE_BUSY = 3
  'Cancelled',               // CE_CANCELLED = 4
  'File I/O error',          // CE_IO_ERROR = 5
  'Memory access error',     // CE_ACCESS_ERROR = 6
  'Unsupported',             // CE_UNSUPPORTED = 7
  'Internal error',          // CE_INTERNAL_ERROR = 8
]);

// "<operation>: <name> (<code>)", plus " - <detail>" only for a real failure
// that carries a detail string. Success never grows a detail suffix.
export function statusText(operation, code, detail) {
  const name = STATUS_NAMES[code] === undefined ? 'Unknown status' : STATUS_NAMES[code];
  let text = operation + ': ' + name + ' (' + code + ')';
  if (code !== 0 && typeof detail === 'string' && detail.length > 0) {
    text += ' - ' + detail;
  }
  return text;
}
