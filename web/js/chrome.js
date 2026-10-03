// The window frame and everything that is always on screen: the title bar, the
// navigation rail, the banner, the host line, the status line and the session
// dot. None of it talks to the core directly; it renders what app.js hands it.
import { PAGES } from './format.js';
import { request } from './bridge.js';

const win = (id) => document.getElementById(id);

/** Title-bar buttons and navigation. Callbacks are injected by app.js. */
export function install({ onPage }) {
  win('win-minimize').addEventListener('click', () => request('window.minimize'));
  win('win-maximize').addEventListener('click', () => request('window.maximize'));
  win('win-close').addEventListener('click', () => request('window.close'));

  // The page owns the frame, so dragging and resizing are handed back to
  // Windows: the runtime starts its own move/size loop from these.
  const titlebar = document.querySelector('.titlebar');
  if (titlebar) {
    titlebar.addEventListener('mousedown', (event) => {
      if (event.button !== 0 || event.target.closest('button')) return;
      request('window.drag', { x: event.screenX, y: event.screenY });
    });
  }
  for (const edge of ['left', 'right', 'top', 'bottom',
                      'topLeft', 'topRight', 'bottomLeft', 'bottomRight']) {
    const grip = document.querySelector(`[data-resize="${edge}"]`);
    if (!grip) continue;
    grip.addEventListener('mousedown', (event) => {
      if (event.button !== 0) return;
      request('window.beginResize', { edge, x: event.screenX, y: event.screenY });
    });
  }

  document.querySelectorAll('.nav-item').forEach((item) => {
    item.addEventListener('click', () => onPage(Number(item.dataset.pageIndex)));
  });
}

/** Switches the visible page: banner, rail highlight, and the scroller's top. */
export function showPage(index) {
  document.querySelectorAll('.page').forEach((page) => {
    const on = Number(page.dataset.pageIndex) === index;
    page.hidden = !on;
    page.classList.toggle('is-hidden', !on);
  });
  document.querySelectorAll('.nav-item').forEach((item) => {
    const on = Number(item.dataset.pageIndex) === index;
    item.classList.toggle('is-active', on);
    if (on) item.setAttribute('aria-current', 'page');
    else item.removeAttribute('aria-current');
  });
  const banner = win('banner');
  if (banner && PAGES[index]) banner.textContent = PAGES[index].banner;
  const body = win('pagebody');
  if (body) body.scrollTop = 0;
}

export function setHost(pid) {
  const node = win('host-pid');
  if (node) node.textContent = String(pid);
}

export function setStatus(text) {
  const node = win('status-line');
  if (node && node.textContent !== text) node.textContent = text;
}

export function setSession(word, state) {
  const label = win('session-word');
  const dot = win('session-dot');
  if (label) label.textContent = word;
  if (dot) dot.dataset.state = state;
}

/** The session word mirrors the runtime's own idea of what it is doing. */
export function sessionFrom(controls) {
  if (!controls) return ['Session ready', 'ready'];
  if (controls.stopping) return ['Stopping', 'stopping'];
  if (controls.scanning || controls.pointerActive) return ['Scanning', 'scanning'];
  return ['Session ready', 'ready'];
}

/**
 * What the `progress` section means on screen: the class to carry, the fill
 * width, and whether the bar belongs on screen at all.
 *
 * `idle` is the only mode that draws nothing: it covers a session that has never
 * scanned, the telemetry New just reset, and the cancelled and failed passes the
 * runtime folds into it. A running scan (`marquee`/`determinate`) draws the
 * track, a completed one keeps it at full width, and the label beside it carries
 * the state in words throughout -- the bar is never the only signal.
 */
export function progressView(progress) {
  // A missing section, or one with no mode, is idle: the runtime always names
  // one, so anything else is a page opened outside it.
  const mode = (progress && progress.mode) || 'idle';
  // A determinate bar takes a 0..1000 value from the runtime.
  const fill = mode === 'determinate' || mode === 'done'
    ? Math.max(0, Math.min(100, (progress.value || 0) / 10))
    : 0;
  return { mode, visible: mode !== 'idle', fill };
}

/**
 * Marquee and determinate are classes, so the animation stops when nothing is
 * happening; visibility follows the same rule in one place for both pages.
 */
export function setProgress(id, progress) {
  const bar = win(id);
  if (!bar) return;
  const view = progressView(progress);
  bar.hidden = !view.visible;
  bar.classList.toggle('is-indeterminate', view.mode === 'marquee');
  bar.classList.toggle('is-busy', view.mode === 'marquee' || view.mode === 'determinate');
  bar.classList.toggle('is-failed', view.mode === 'failed');
  bar.style.setProperty('--fill', `${view.fill}%`);
}

export function setText(id, text) {
  const node = win(id);
  if (node && node.textContent !== text) node.textContent = text;
}
