// The drift guard between the headers and the page.
//
// format.js is a hand-written mirror of include/ce/*.h, and nothing else would
// notice if a value moved: a wrong type code or comparison would just make the
// runtime refuse the command at run time, on someone else's machine. So the
// header is parsed here and the mirror is asserted against it.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import {
  VALUE_TYPES, COMPARISONS, ROUNDINGS, ALIGNMENTS, HEX_MODES, STATUS_NAMES, CE_TYPE, CE_CMP,
} from '../../web/js/format.js';
import { MODES } from '../../web/js/hexview.js';

const root = join(import.meta.dir, '..', '..');

/** Reads `NAME = value` pairs out of one enum block in a header. */
function enumValues(header, name) {
  const text = readFileSync(join(root, 'include', 'ce', header), 'utf8');
  const start = text.indexOf(`enum ${name} {`);
  expect(start).toBeGreaterThanOrEqual(0);
  const block = text.slice(start, text.indexOf('};', start));
  const found = new Map();
  for (const match of block.matchAll(/\b(CE_[A-Z0-9_]+)\s*=\s*(\d+)/g)) {
    found.set(match[1], Number(match[2]));
  }
  return found;
}

test('CeValueTypeV2 matches VALUE_TYPES', () => {
  const header = enumValues('api_v2.h', 'CeValueTypeV2');
  expect(header.size).toBe(VALUE_TYPES.length);
  for (const { value, label } of VALUE_TYPES) {
    expect(label.length).toBeGreaterThan(0);
    const name = Object.keys(Object.fromEntries(header)).find((key) => header.get(key) === value);
    expect(name).toBeDefined();
  }
  expect(CE_TYPE.U8).toBe(header.get('CE_TYPE_U8'));
  expect(CE_TYPE.U16).toBe(header.get('CE_TYPE_U16'));
  expect(CE_TYPE.U32).toBe(header.get('CE_TYPE_U32'));
  expect(CE_TYPE.U64).toBe(header.get('CE_TYPE_U64'));
  expect(CE_TYPE.FLOAT).toBe(header.get('CE_TYPE_FLOAT'));
  expect(CE_TYPE.DOUBLE).toBe(header.get('CE_TYPE_DOUBLE'));
  expect(CE_TYPE.UTF8).toBe(header.get('CE_TYPE_UTF8'));
  expect(CE_TYPE.UTF16).toBe(header.get('CE_TYPE_UTF16'));
  expect(CE_TYPE.AOB).toBe(header.get('CE_TYPE_AOB'));
});

test('CeCompareV2 matches COMPARISONS', () => {
  const header = enumValues('api_v2.h', 'CeCompareV2');
  expect(header.size).toBe(COMPARISONS.length);
  const values = COMPARISONS.map((entry) => entry.value);
  expect(values).toEqual([...header.values()]);
  expect(CE_CMP.EXACT).toBe(header.get('CE_CMP_EXACT'));
  expect(CE_CMP.BETWEEN).toBe(header.get('CE_CMP_BETWEEN'));
  expect(CE_CMP.DECREASED_BY).toBe(header.get('CE_CMP_DECREASED_BY'));
});

test('CeRoundingV2 matches ROUNDINGS', () => {
  const header = enumValues('api_v2.h', 'CeRoundingV2');
  expect(header.size).toBe(ROUNDINGS.length);
  expect(ROUNDINGS.map((entry) => entry.value)).toEqual([...header.values()]);
});

test('CeStatus matches STATUS_NAMES', () => {
  const header = enumValues('api.h', 'CeStatus');
  expect(header.size).toBe(STATUS_NAMES.length);
  STATUS_NAMES.forEach((name, index) => {
    expect(typeof name).toBe('string');
    expect(name.length).toBeGreaterThan(0);
    expect([...header.values()]).toContain(index);
  });
  expect(STATUS_NAMES[header.get('CE_OK')]).toBe('OK');
  expect(STATUS_NAMES[header.get('CE_BUSY')]).toBe('Busy');
});

test('the display-mode combo and the hex layout table agree', () => {
  expect(HEX_MODES.length).toBe(MODES.length);
  HEX_MODES.forEach((entry, index) => {
    expect(entry.value).toBe(index);
    expect(entry.label).toBe(MODES[index].label);
  });
});

test('alignment choices are the four powers of two', () => {
  expect(ALIGNMENTS.map((entry) => entry.value)).toEqual([1, 2, 4, 8]);
});
