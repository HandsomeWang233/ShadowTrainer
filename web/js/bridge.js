// Transport between this page and the runtime.
//
// One JSON channel both ways: commands go out as {id, cmd, args} and the runtime
// answers each with exactly one {kind:"reply", id, ...}. Anything the runtime
// says without being asked arrives as {kind:"event", changed:[...]}.
//
// Every 64-bit quantity (address, id, generation, count, size) arrives as a
// DECIMAL STRING. JavaScript never does 64-bit arithmetic here: pages send
// deltas or row indices and let the runtime clamp.

const host = (typeof window !== 'undefined' && window.chrome && window.chrome.webview)
  ? window.chrome.webview
  : null;

/** False when the page is opened outside the runtime, e.g. in a browser. */
export const connected = host !== null;

let nextId = 1;
const pending = new Map();
const listeners = new Set();

if (host) {
  host.addEventListener('message', (event) => {
    const message = event.data;
    if (!message || typeof message !== 'object') return;
    if (message.kind === 'reply') {
      const entry = pending.get(message.id);
      if (entry) {
        pending.delete(message.id);
        entry(message);
      }
      return;
    }
    for (const listener of listeners) {
      try {
        listener(message);
      } catch (error) {
        // A broken listener must not swallow the next event.
        console.error('event listener failed', error);
      }
    }
  });
}

/** Resolves with the whole reply envelope so callers can read status too. */
export function request(cmd, args) {
  if (!host) return Promise.resolve({ kind: 'reply', id: 0, ok: false, error: { code: 2, name: 'Session is not running', detail: 'Not running inside the runtime.' } });
  const id = nextId++;
  return new Promise((resolve) => {
    pending.set(id, resolve);
    host.postMessage({ id, cmd, args: args || {} });
  });
}

/** Convenience for callers that only care about the payload. */
export async function call(cmd, args) {
  const reply = await request(cmd, args);
  if (!reply || !reply.ok) {
    const detail = reply && reply.error ? reply.error.detail : '';
    const name = reply && reply.error ? reply.error.name : 'Unknown error';
    throw Object.assign(new Error(detail || name), { reply });
  }
  return reply.data;
}

export function onEvent(handler) {
  listeners.add(handler);
}

/** The status line always comes from the runtime; the page never composes it. */
export function statusText(reply) {
  return reply && typeof reply.status === 'string' ? reply.status : '';
}
