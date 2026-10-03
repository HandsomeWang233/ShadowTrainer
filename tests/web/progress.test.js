// When the two progress bars belong on screen.
//
// The bug this pins: an idle session used to draw the bar as an empty inset
// track next to the label, on both the Scan and the Pointer scan page, which
// reads as work in flight when nothing is running. The rule is now "a running
// scan, or the completion that just happened" -- New puts it straight back.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { progressView } from '../../web/js/chrome.js';

const root = join(import.meta.dir, '..', '..');
const source = (name) => readFileSync(join(root, 'web', 'js', name), 'utf8');

const bar = (mode, value) => progressView({ mode, value });

test('a session that never scanned draws no bar', () => {
  // The runtime's idle section names its own state in the label; the bar would
  // only be an empty track.
  expect(bar('idle', 0).visible).toBe(false);
  expect(progressView(null).visible).toBe(false);
  expect(progressView(undefined).visible).toBe(false);
});

test('New hides the bar again', () => {
  // Ce_NewScanV2 resets the telemetry to CE_SCAN_IDLE, so the section that
  // follows a New is idle -- the same shape as a fresh session.
  expect(bar('idle', 0)).toEqual({ mode: 'idle', visible: false, fill: 0 });
});

test('a scan in flight keeps the bar up', () => {
  expect(bar('marquee', 0).visible).toBe(true);
  const reading = bar('determinate', 425);
  expect(reading.visible).toBe(true);
  expect(reading.fill).toBe(42.5);
});

test('a completed scan keeps the bar, frozen full', () => {
  expect(bar('done', 1000)).toEqual({ mode: 'done', visible: true, fill: 100 });
});

test('a cancelled or failed pass arrives as idle, and so is invisible', () => {
  // The runtime's only modes are idle, marquee, determinate and done:
  // scan_progress folds every terminal phase but COMPLETED into idle, so a
  // cancelled pass never claims 100% and never draws a bar.
  expect(progressView({ mode: undefined }).visible).toBe(false);
  expect(bar('idle', 1000).visible).toBe(false);   // a stale value draws nothing either
});

test('the fill is clamped to the 0..1000 the runtime promises', () => {
  expect(bar('determinate', 2000).fill).toBe(100);
  expect(bar('determinate', -50).fill).toBe(0);
  expect(bar('determinate', undefined).fill).toBe(0);
});

test('each page paints its own bar, so the first render is enough', () => {
  // Both bars ship visible in the markup and are hidden by the idle payload.
  // A page that only painted its bar from an event therefore showed the empty
  // track until something changed -- which is exactly what an injected DLL did
  // on the Scan page, where nothing arrives until a scan is started. Each page
  // owns its bar, the way each owns its label.
  expect(source('scan.js')).toContain("chrome.setProgress('scan-progress'");
  expect(source('pointer.js')).toContain("chrome.setProgress('ptr-progress'");
  // And the event handler does not paint them a second time from the middle.
  expect(source('app.js')).not.toContain("chrome.setProgress('");
});