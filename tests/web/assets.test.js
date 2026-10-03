// The three-way asset list, and whether the page can actually reach what it asks for.
//
// `web/webui.rc` embeds the files, `web/resource_ids.h` numbers them and the table
// in `src/ui/webview_host.cpp` serves them; one `path` is both the URL and the
// file location. The failure this guards is quiet and total: a module the page
// imports but the table does not list is a 404, and a 404 inside the module graph
// stops the whole page. Nothing renders, `hello` is never sent, the window's
// reveal hook is never registered, and the window sits on its cover until the
// veil's timeout - a UI that looks alive and answers nothing.
//
// Run with: bun test tests/web
import { expect, test } from 'bun:test';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const root = join(import.meta.dir, '..', '..');
const read = (path) => readFileSync(join(root, path), 'utf8');

/** `NAME RCDATA "file"` lines. The app icon is ICON, not served, and not a page asset. */
const embedded = [...read('web/webui.rc').matchAll(/^\s*(IDR_\w+)\s+RCDATA\s+"([^"]+)"/gm)]
  .map(([, id, path]) => ({ id, path }));
/** `{L"path", IDR_...}` entries of the asset table. */
const served = [...read('src/ui/webview_host.cpp').matchAll(/\{L"([^"]+)",\s*(IDR_\w+)/g)]
  .map(([, path, id]) => ({ path, id }));

test('the embeds and the table name the same files', () => {
  expect(served.map((entry) => entry.path).sort()).toEqual(embedded.map((entry) => entry.path).sort());
  expect(served.map((entry) => entry.id).sort()).toEqual(embedded.map((entry) => entry.id).sort());
});

test('every embedded file is on disk and numbered', () => {
  const ids = read('web/resource_ids.h');
  for (const entry of embedded) {
    expect(existsSync(join(root, 'web', entry.path))).toBe(true);
    expect(ids).toContain(`#define ${entry.id} `);
  }
});

test('every module the page imports is served', () => {
  const paths = new Set(embedded.map((entry) => entry.path));
  const dir = join(root, 'web', 'js');
  for (const file of readdirSync(dir)) {
    if (!file.endsWith('.js')) continue;
    const text = readFileSync(join(dir, file), 'utf8');
    for (const [, target] of text.matchAll(/from\s+'\.\/([\w.-]+\.js)'/g)) {
      expect({ imported: `js/${target}`, by: file, served: paths.has(`js/${target}`) })
        .toEqual({ imported: `js/${target}`, by: file, served: true });
    }
  }
});

test('everything index.html asks the origin for is served', () => {
  const paths = new Set(embedded.map((entry) => entry.path));
  const page = read('web/index.html');
  const asked = [...page.matchAll(/(?:src|href)="([^":]+)"/g)]
    .map(([, url]) => url)
    .filter((url) => !url.startsWith('data:'));
  expect(asked.length).toBeGreaterThan(0);
  for (const url of asked) {
    expect({ asked: url, served: paths.has(url) }).toEqual({ asked: url, served: true });
  }
});
