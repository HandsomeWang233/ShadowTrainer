// The page's own confirmation dialog.
//
// window.confirm would be less code, and WebView2 does show it - but it is Edge's
// dialog: light chrome, the system typeface, and the app behind it left alone.
// This one is the page's, so it wears the theme (web/css/app.css, section 14),
// sits centred over a blurred copy of the app, and resolves a promise either way.
//
// It is deliberately one dialog at a time: a second question while the first is
// open would leave the first promise hanging, so the open one is answered "no"
// before the new one takes over.

let resolveOpen = null;   // the promise the visible dialog is waiting on

function node(id) {
  return document.getElementById(id);
}

/** Closes the dialog, if one is open, and answers it. */
function settle(answer) {
  const dialog = node('dialog');
  if (dialog && !dialog.hidden) {
    dialog.hidden = true;
    document.removeEventListener('keydown', onKeyDown, true);
  }
  const resolve = resolveOpen;
  resolveOpen = null;
  if (resolve) resolve(answer);
}

function onKeyDown(event) {
  if (event.key === 'Escape') {
    event.preventDefault();
    settle(false);
  }
}

/**
 * Asks a question and resolves true when the reader agrees. `danger` paints the
 * confirm button as destructive; `confirmLabel` is what it says.
 */
export function ask({ title, body, confirmLabel = 'Confirm', cancelLabel = 'Cancel', danger = true }) {
  const dialog = node('dialog');
  if (!dialog) return Promise.resolve(false);
  // Whatever was open is answered first, so no caller is left waiting.
  settle(false);
  node('dialog-title').textContent = title || '';
  node('dialog-body').textContent = body || '';
  const confirm = node('dialog-confirm');
  const cancel = node('dialog-cancel');
  confirm.textContent = confirmLabel;
  cancel.textContent = cancelLabel;
  confirm.classList.toggle('btn-danger', !!danger);
  confirm.classList.toggle('btn-primary', !danger);
  dialog.hidden = false;
  // Enter answers the question the dialog is asking, and Escape - or the scrim,
  // which is "outside" - is the way out of it.
  confirm.focus();
  document.addEventListener('keydown', onKeyDown, true);
  return new Promise((resolve) => {
    resolveOpen = resolve;
    confirm.onclick = () => settle(true);
    cancel.onclick = () => settle(false);
    node('dialog-scrim').onclick = () => settle(false);
  });
}
