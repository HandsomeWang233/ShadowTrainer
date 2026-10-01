# Web UI

[English](WEBUI.md) | [中文](WEBUI_CN.md)

The window is a WebView2 control showing a page built from `web/`. The UI that
used to be here — `ui.cpp`, `ui_theme.cpp`, `ui_dropdown.cpp` and the owner-drawn
controls they drove — is gone; a copy is archived under `baseline/pre-webui/`
for reference only, and nothing in the build refers to it.

Layout, labels, column widths, paging, status strings and semantics are the ones
the native UI had. Only the drawing and the input handling are new.

## The layers

| Layer | Files | Knows about |
|---|---|---|
| Host | `src/ui/webui.cpp`, `src/ui/webui.hpp` | The window, the timers, the hotkey, the file dialogs, the fallback card |
| Cover | `src/ui/cover.cpp`, `src/ui/cover.hpp` | The startup word sequence, drawn with GDI before there is a page to show |
| Plumbing | `src/ui/webview_host.cpp`, `src/ui/webview_host.hpp` | WebView2, the embedded asset server, the message channel |
| Semantics | `src/bridge/ui_bridge.cpp`, `src/bridge/ui_bridge_commands.inc`, `src/bridge/ui_bridge_sections.inc`, `src/bridge/json_min.inc` | Every command, every status string, the worker thread, the caches |
| Page | `web/**` | Rendering and input. It owns no facts |

The first three layers live together in `src/ui/`; the fourth is its own module
in `src/bridge/` because it is the one with a headless test. The scanning engine
and the ABI sit in `src/core/`, and `src/runtime.cpp` is the only translation
unit left at the top. Each `src/<module>/` is its own include root (`build.cmd`
and `src/shadowtrainer.vcxproj` both add them), so the sources keep saying
`#include "core.hpp"` rather than naming a directory.

The split matters because the middle layer is the only one that cannot be tested
without a browser. `ui_bridge` talks to the core through a function-pointer
table (`CeApi`) instead of the exported symbols, so `tests/bridge_tests.cpp` runs
every button action headlessly with fakes, and again against the real DLL.

`runtime.cpp` calls `ce::run_ui` exactly as before; the window contract external
callers depend on is unchanged: a visible unowned top-level window carrying the
`SHADOWTRAINER_WINDOW` property, `WM_APP+1` shows, `WM_APP+2` stops, `WM_APP+3`
toggles, `WM_CLOSE` hides only, and stopping unloads the module.

## Assets

The page is compiled into the DLL as `RCDATA` and served from a virtual origin,
`https://shadowtrainer.local/`. Nothing is written to disk, so an injected DLL
never has to find a writable directory next to an arbitrary host executable.

- Three lists name the same fifteen entries: `web/webui.rc` embeds the files,
  `web/resource_ids.h` numbers them, and the table in `src/ui/webview_host.cpp`
  states each MIME type. An entry carries one `path` that is both the URL and
  the location under `web/`, so it has to match the `.rc`. The two ways to get
  it wrong fail differently: a path in the table that the `.rc` does not embed
  is a 404 at runtime, and a path the `.rc` names that is not on disk stops the
  build. `brand.png` and `app.ico` are the exception: `brand.png` is served like
  any other asset, while `app.ico` is embedded for the window class alone
  (`LoadImageW`/`WM_SETICON`, never served), so only the icon stays out of the
  table.
- The URLs mirror `web/`: the page asks for `/js/app.js` and gets `web/js/app.js`.
  One string does both jobs — the table entry, the `.rc`, and the page's own
  `<link>`/`<script>`/`<img>` all spell it the same way — which is also why
  `web/index.html` opens straight off disk with its stylesheet, scripts and logo
  intact.
- `find_asset` matches the request against the table by **exact equality**, and
  that is the whole guard on the request path: a hand-written URL can only ever
  name one of the fifteen listed entries, and no request-derived string is ever
  handed to `CreateFileW`. Entries carrying separators does not change that —
  what would weaken it is turning the exact match into a prefix or pattern
  match. `webui.rc` and `resource_ids.h` stay at the root of `web/`, which is
  what lets `build.cmd` keep passing a plain `/Iweb`.
- `build.cmd` runs `rc.exe` with `/Iweb`. (Verified: `rc.exe` resolves `RCDATA`
  file names through the include path, not the working directory, so the command
  behaves the same here and under an MSBuild build of `shadowtrainer.vcxproj`.)
- Every text asset is pure ASCII, so `rc.exe` cannot re-encode it. The two
  binary assets ride through untouched: `RCDATA` copies bytes verbatim and
  `ICON` is parsed by `rc.exe` itself.
- Requests are answered by `WebResourceRequested`. The filter is a URI
  **pattern**: `https://shadowtrainer.local/*`. Without the trailing `*` only the
  exact URL matches and every real request escapes to the network.

`build\x64\webui.res` grows with the page, and `dumpbin /dependents` stays free
of `WebView2Loader.dll` because the loader is linked statically — a side-by-side
DLL is not an option for an injected module.

### Development loop

```bat
build\x64\dev_host.exe --dev
```

`SHADOWTRAINER_WEBUI_DIR` makes the host read `web/` from disk instead of the
embedded copies, and `SHADOWTRAINER_WEBUI_DEV=1` turns DevTools back on, so a
stylesheet edit is F5 rather than a rebuild. Reloading is safe: the bridge lives
in C++, so a reload only re-sends `hello` and repaints.

The disk read opens `web/<path>` for a request, and that is the same string the
`.rc` embeds — there is no second mapping to keep in step. The fallback is the
one that was always there: if the read fails, `load_asset` serves the embedded
`RCDATA` copy instead, **silently**. Nothing tests that branch, so if an edit
stops appearing, suspect the fallback before suspecting the browser.

## The startup sequence

`Shadow` flies in from the right and stops dead centre, the letters of `Trainer`
grow in behind it, and the UI then spreads outwards from the exact centre of the
window. **The window plays the word; the page plays the reveal.** That split is
the whole design, and it exists because WebView2 takes a second or two to come up
while the window is already on screen.

### The word, drawn by the window

`src/ui/cover.cpp` draws it with GDI while the page is still starting. It is meant
to be the page's own rendering rather than a likeness of it, so it matches what
the stylesheet would have produced:

- the same face (`Segoe UI Variable Text` when the machine has it, otherwise
  `Segoe UI`), the same em size (GDI's `lfHeight` takes it directly), and the
  same antialiasing: `ANTIALIASED_QUALITY`, not ClearType, because the page sets
  `-webkit-font-smoothing: antialiased` and so gets greyscale. ClearType here
  puts red and blue fringes on every stem - very visible under big white type on
  a dark field - and leaves the two copies of the word looking different;
- the same `letter-spacing: -0.02em`, applied through `SetTextCharacterExtra`.
  GDI applies that extra to `GetTextExtent` as well as to `TextOut`, so the
  measured widths and the drawn ones stay in step with the page's. (Measured:
  without it the word comes out 12% wide, because the page's spacing is negative
  and the cover was not applying any.)
- the same timing curves, solved rather than approximated: `bezier` runs the
  actual `cubic-bezier(0.16, 1, 0.3, 1)` and `cubic-bezier(0.2, 0.9, 0.25, 1)`
  the stylesheet names;
- the same split the page makes between the box a letter grows into and the
  letter itself. The box widens on the bezier; the letter fades, lifts and scales
  **linearly** (from `translateY(0.45em) scale(0.7)`), through its own world
  transform with the origin at its own box centre. Scaling about the left edge
  instead walks each letter sideways and drags the row off centre by up to 20px -
  which is exactly what the first version did;
- a clip on the row box, standing in for `overflow: hidden` on a `line-height: 1`
  element, so a letter rising into place is revealed by the line rather than
  drawn outside it;
- the row re-centred on every frame, so whatever is on screen stays in the middle
  of the window;
- the page's two colours (`#f6faff`, `#b9e6ff`), its radial-gradient glow behind
  the word, and the `INITIALISING` line beneath it.

The word then holds, complete, for two seconds before fading out: long enough to
be read rather than a flash before the UI.

None of that can be checked by running a test - there is no way to look at a
window during one - so `cover.cpp` is a separate translation unit with no
dependence on the window. A throwaway probe can then render frames of it into a
bitmap and put them beside the browser's, which is how the transform bug above
was found, and how the word's baseline was confirmed to land on the same pixel
the page puts it on.

### Composed off-screen

The window paints nothing directly while the cover is up. `paint_boot` composes
the frame into a `CreateCompatibleBitmap` the size of the client area and blits
it in one go. Painting straight onto the window instead shows every intermediate
state - the field filled, then the glow blended over it, then the word - and at
sixty frames a second those steps read as a flicker; a still frame makes it
worse, not better, which is what it was reported as. `WM_ERASEBKGND` is answered
with "already erased" for the same reason: erasing there would put the bare field
on screen a moment before the frame landed on top of it.

It is not a performance fix. A composed frame plus the blit measures 0.67ms at
1360x900 against a 16.7ms frame budget, so there was never a speed problem - only
one of letting a half-finished frame be seen.

### Driven by the compositor

Two things had to change before the motion matched the page's, and neither was
about drawing speed.

**The clock.** `now_ms()` is `QueryPerformanceCounter`, not `GetTickCount64`.
The latter only moves when the system clock ticks - about every 15.6ms unless
something in the process has raised the timer resolution - so sampling it once
per frame hands the animation a time that jumps in steps and sits still in
between. Frames then draw either the same instant or one several milliseconds on,
which reads as uneven motion however fast the frames arrive.

**The beat.** Frames are posted by a second thread calling `DwmFlush`, which
returns at the next composition - the same clock the browser drives the page's
copy from. `SetTimer` was the obvious source and the wrong one: it is only
accurate to the system clock tick, and `WM_TIMER` is a low-priority message, so
the interval wandered by several milliseconds from frame to frame. The ticker
starts with the first painted frame and stops at the hand-over, so a settled
window is not repainting sixty times a second for nothing.

`DwmFlush` is resolved out of `dwmapi.dll` by hand, like the corner call above;
if it is missing, or composition is off - a remote session, say - the flush fails
and the loop falls back to a 16ms sleep.

### The reveal, played by the page

`Host::start(..., hold_visible)` is what makes the hand-over possible. The
controller is created but left hidden, so the page cannot appear behind the
cover, and `Host::reveal()` shows it once the word is done. That is also why the
cover can outlast the load — the page waits behind it, fully rendered, until it
is wanted.

`boot.js` waits for the window to call `window.__shadowtrainerReveal`, then lifts
the veil through a hole that grows from the exact centre of the window. The mask
keeps everything outside that hole, so the app is uncovered centre-first and
looks like it spreads outwards from the middle.

The window makes that call when the cover ends and again whenever the page says
`hello`. The second one is what a reload relies on: the cover plays once per
process, so a reload misses the first call and would otherwise wait out its
timeout. `boot.js` has an 8-second fallback and `index.html` a 12-second one, the
latter being the guard for a module graph that never loads at all.

### Opened on its own

With no bridge there is nothing to play the word and nobody to send the
go-ahead, so `boot.prepare()` waits a beat and lifts. The veil is still there, so
a browser preview shows the reveal and nothing else.

### Keeping it cheap

Nothing in it uses `filter`: a blur over live text re-rasterises the element
every frame. The cover draws with plain GDI, and the page's half is a mask over a
flat field, which is the cheapest thing it could be.

## The bridge protocol

One JSON channel both ways.

```jsonc
// page -> runtime, one reply each
{"id": 17, "cmd": "scan.first", "args": {"type": 2, "comparison": 0, ...}}

// runtime -> page
{"kind": "reply", "id": 17, "ok": true, "data": {...}}
{"kind": "reply", "id": 17, "ok": false,
 "error": {"code": 3, "name": "Busy", "detail": ""}, "status": "First scan: Busy (3)"}

// unsolicited, at most one per tick, only the sections that changed
{"kind": "event", "seq": 412, "changed": ["status", "scan", "controls"],
 "status": "...", "scan": {...}, "controls": {...}}
```

- **Every 64-bit quantity crosses as a decimal string** — addresses, ids,
  generations, counts, sizes. A JSON number cannot carry a `uint64` exactly and
  JavaScript's `Number` cannot hold one. `json::Writer::u64` emits the string;
  `Value::as_u64` accepts either form.
- Status strings are composed only in C++ (`status_name()` and the old
  `"<operation>: <name> (<code>)" + " - <detail>"` format). The page renders them
  verbatim and never builds one.
- The first command is `hello`, answered with the whole model (caches bypassed);
  events start after that. A page reload is therefore stateless.
- Sections: `status`, `scan`, `pointer`, `results`, `resultRows`, `records`,
  `recordRows`, `views`, `controls`. Rows are pushed **incrementally** (`{reset,
  from, rows}`) so a passive reload during a scan repaints only what appeared.
- Commands that change a list (`results.page`, `record.refresh`, `view.load`,
  `ptr.refresh`, ...) answer with the section they changed, so the page paints
  from the reply rather than waiting for the next tick.
- `window.*` and the CT file dialogs are forwarded to the host through
  `Session::Host`; the bridge cannot open a native file picker and does not
  pretend to.

## The tick

`webui.cpp` keeps the native UI's two timers: 100 ms for the poll and 16 ms for
the backtick sampler. Each poll calls `Session::pump(visible, minimized)`, which
is the old `WM_TIMER` body: read scan status (a `CE_BUSY` snapshot is a *miss*,
not a state — the last good telemetry is kept and no data API is touched that
tick), re-issue `CE_CancelScan` while a cancellation is pending, poll the pointer
job, join a finished worker, consume the deferred refresh flags, then load
passive result rows (16 rows / 64 KiB / 12 ms) and sample live record values
(500 ms apart, round-robin). Hiding the window suppresses only the last two.

A worker posts `WM_APP+5` when it finishes so the completion is seen in
milliseconds instead of at the next tick; `pump` also notices it, so the timer is
the safety net rather than the mechanism.

## Gating

Split deliberately, because re-sending a whole enable/disable map on every
keystroke would be silly:

- The runtime pushes what follows from core state: `idle`, `busy`, `haveScan`,
  `canUndo`, `recordsStale`, the selection, page bounds, the view bounds, the
  memory flags and the pointer job.
- `web/js/gating.js` derives the field-level rules — which comparisons can start a
  scan, that the format locks after the first committed scan, that rounding only
  applies to an exact Float/Double scan, that an imported opaque record can be
  removed but never written or frozen.
- The runtime **re-checks the same predicates** when a command arrives and
  answers `CE_BUSY` instead of silently ignoring it, which is a deliberate change
  from the native UI's silent refusal.

`tests/web/gating.test.js` pins the rule table.

## What stayed native

The window itself, `WM_NCHITTEST`'s six-pixel resize border, the custom
non-client handling, minimise/maximise/close, the rounded corners (DWM on
Windows 11, a window region otherwise), the backtick sampler, the two timers,
`WM_APP+1/2/3`, the file dialogs, and the card that stands in for the page.

Dragging and resizing are handed back to Windows: the page posts
`window.drag` / `window.beginResize` with the screen coordinates, and the host
runs `ReleaseCapture()` followed by a posted `WM_NCLBUTTONDOWN`, which is what
starts Windows' own move/size loop.

## WebView2 lifecycle

- `CoInitializeEx(APARTMENTTHREADED)` is done once on the UI thread and released
  when `run_ui` returns. Nothing COM-related happens in `DllMain`.
- The window is created, shown and published, and `CE_RUNNING` is set, **before**
  the WebView is created. A slow or missing runtime never delays the session.
- The user data folder is `%LOCALAPPDATA%\ShadowTrainer\WebView2\<pid>` — per
  process, because WebView2 refuses to share a profile, and never next to the
  host executable, which may not be writable. Folders left by dead processes are
  swept at the next startup; the live one is **not** deleted on shutdown, because
  the browser process is still using it. Deleting it kills that process (which
  then reports `ProcessFailed` and shows the card) and the next load in the same
  process inherits the wreckage.
- Shutdown order: remove the event handlers, `controller->Close()`, then release.
  Releasing the web view first makes WebView2 start its own teardown, which pumps
  messages and dispatches them into half-released objects.
- The teardown runs on the message loop's own stack, not from inside a window
  procedure, and the loop ends the moment the session does: anything still queued
  for one of WebView2's own windows would be dispatched into torn-down objects.

### The one that cost the most

A completion handler's arguments are **borrowed references** — the caller owns
them and releases when `Invoke` returns. Storing one without `AddRef` leaves it
dangling immediately. The symptom was thoroughly misleading: every call returned
`S_OK`, the controller callback fired, `put_Bounds`, `put_IsVisible` and
`Navigate` all succeeded — and yet the WebView never created its child window,
the page never appeared, and `Close()` faulted on freed memory.

`ComPtr::operator=(T*)` therefore AddRefs (the pointer stays the caller's) and
`attach()` is the explicit "I own this reference" form used where a fresh
reference is handed over.

### The other one

`ICoreWebView2Controller` is **visible as soon as it exists**. The startup cover
needs it held back until the word has played, and the obvious-looking way to do
that - "just don't call `put_IsVisible(TRUE)`" - does nothing at all: the page
appears the moment it is ready, about a second in, and cuts the cover off partway
through. The symptom was the cover drawing "Shadow", then going black, which
reads like a drawing bug rather than a visibility one. `put_IsVisible(FALSE)` has
to be asked for explicitly.

## When the runtime is missing

The session stays alive and `CE_RUNNING` with its usual window; only the UI is
degraded. `WM_PAINT` draws a card naming the missing runtime, the install address
and the error, and points out that the CE API still works. While the WebView is
merely *starting*, a quieter line is drawn instead — an earlier version showed
the failure card during those one or two seconds, which was simply wrong. A
`ProcessFailed` (the browser process died) lands on the same card and does not
stop the session.

## Tests

| Where | What |
|---|---|
| `tests/bridge_tests.cpp` | Every command. Mode A fakes `CeApi` (refusals, request construction, paging clamps, event shape); mode B loads the real DLL and walks the same commands end to end. Needs the DLL path. |
| `tests/web/hexview.test.js` | The hex layout tables, the address-order nibble rule, per-mode value formatting, `%g`. Replaces `hex_geometry_tests.cpp`. |
| `tests/web/gating.test.js` | The enable/disable rules. |
| `tests/web/enums.test.js` | Parses `include/ce/api_v2.h` and `api.h` and asserts `format.js` mirrors every enum value. |
| `tools/dev_host.cpp` | Not a test: loads the DLL in-process so the page can be edited and reloaded. |

`bun test tests/web` needs no packages. `runtime_tests` and `runtime_v2_tests`
are unchanged and still the gate on the window contract — including three
load/stop/unload cycles, which is what proves the WebView is really gone before
`FreeLibraryAndExitThread`.

## Still missing

- The hex view's grouped modes render and edit, but selection is a byte range
  dragged over cells; there is no per-nibble hit test from a click, which the
  native control had via its text caret.
- No keyboard shortcut for the display modes (the native build had none either).
- `docs/img/webui/` holds the current screenshots.
