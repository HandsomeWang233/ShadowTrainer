// The startup veil.
//
// The window plays the word sequence itself while WebView2 comes up - see
// paint_boot in src/ui/webui.cpp, which owns that half - and the page's half is the
// reveal. This module answers one question: when may the veil lift?
import { connected } from './bridge.js';

const node = () => document.getElementById('boot');

const reduced = () =>
  typeof window.matchMedia === 'function' &&
  window.matchMedia('(prefers-reduced-motion: reduce)').matches;

/** One of the veil's durations, read back from the stylesheet. */
function duration(name, fallback) {
  const raw = getComputedStyle(document.documentElement).getPropertyValue(name);
  const value = Number.parseFloat(raw);
  return Number.isFinite(value) && value > 0 ? value : fallback;
}

/**
 * Resolves when the veil may lift. Resolves immediately when there is no veil
 * or motion is reduced, so the caller can always await it.
 */
export function prepare() {
  const veil = node();
  if (!veil || reduced()) return Promise.resolve();

  if (!connected) {
    // Nothing is hosting us, so nobody will play the word or send a go-ahead.
    // Show the UI after a beat rather than sitting behind a flat field.
    return new Promise((resolve) => {
      setTimeout(resolve, 300);
    });
  }

  return new Promise((resolve) => {
    let settled = false;
    const finish = () => {
      if (settled) return;
      settled = true;
      resolve();
    };
    // The window calls this once its cover has finished, and again when this
    // page says "hello" - which is what a reload relies on, since the cover
    // plays once per process. The timeout is the last resort.
    window.__shadowtrainerReveal = finish;
    setTimeout(finish, 8000);
  });
}

/**
 * Uncovers the UI: the veil is punched out from the centre of the window and
 * then dropped from the document entirely.
 */
export function lift() {
  const veil = node();
  if (!veil) return;
  if (reduced()) {
    veil.remove();
    return;
  }
  veil.classList.add('is-lifting');
  // The element is removed rather than merely hidden: it covers the whole
  // window, and nothing should be left compositing over the app afterwards.
  setTimeout(() => veil.remove(), duration('--boot-reveal-dur', 780) + 80);
}
