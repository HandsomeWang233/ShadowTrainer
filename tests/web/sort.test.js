// The header marker: which way a column reads, and what "not sorted" looks like.
//
// The runtime owns the order, so the page's only job is to turn the column it
// was told about into the marker beside that header. Getting it wrong is a caret
// on the wrong column, or a caret pointing the way the data does not run.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import { arrow } from '../../web/js/sort.js';

test('nothing is marked until a column is sorted', () => {
  expect(arrow(0, -1, false)).toBe('none');
  // A section that predates the feature, or a reply that lost the field.
  expect(arrow(2, undefined, false)).toBe('none');
  expect(arrow(2, null, false)).toBe('none');
});

test('only the sorted column is marked, and it points the way it runs', () => {
  expect(arrow(1, 1, false)).toBe('ascending');
  expect(arrow(1, 1, true)).toBe('descending');
  expect(arrow(0, 1, false)).toBe('none');
  expect(arrow(2, 1, true)).toBe('none');
});

test('the column arrives as a number or as the string JSON was parsed into', () => {
  // `records.sortColumn` crosses as an integer, but a payload built by hand in a
  // test or a mock may spell it as a string; both name the same column.
  expect(arrow(3, '3', false)).toBe('ascending');
  expect(arrow(3, '4', false)).toBe('none');
});
