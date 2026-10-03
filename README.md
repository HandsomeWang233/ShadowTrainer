# ShadowTrainer

[English](README.md) | [中文](README_CN.md)

A memory scanner and trainer runtime for Windows, shipped as a single x64 DLL.
Load it into a process and it opens its own window, then scans, edits and freezes
that process's memory. No driver, no service, no helper executable, no files
written next to the host, and no separate target picker — the process it is
loaded into **is** the target.

![The Memory scan page](docs/img/webui/scan.png)

The interface is a WebView2 page. It is compiled into the DLL as resources and
served from a virtual origin, so it never touches the disk and a missing WebView2
runtime degrades the UI rather than stopping the session.

## Features

**Scanning** — the seven pages are Scan, Address list, Memory preview, Regions,
Modules, Threads and Pointer scan.

| | |
|---|---|
| Value types | Byte, 2 / 4 / 8 bytes (signed, unsigned, hex), Float, Double, UTF-8, UTF-16LE, array of bytes |
| First scan | Exact, unknown initial value, greater than, less than, between |
| Next scan | the above, plus changed / unchanged, increased / decreased, increased / decreased by a given amount |
| Text and AOB | exact and fixed-width changed / unchanged; AOB wildcards `??`, `A?`, `?F` |
| Floating point | Exact, plus the reference implementation's Rounded, Rounded (extreme) and Truncated |
| Results | written to a file, paged, cancellable, one level of undo; cancelling never overwrites the previous successful result |

**Address list** — add, edit and delete records; description, type, value and
freeze per record; pointer chains up to 16 levels, re-resolved on every write so
a frozen value follows its base pointer. A record whose freeze would overlap
another frozen range is refused rather than silently written.

**Memory preview** — a 256-byte hex/ASCII page with ten display modes (byte, 2, 4
and 8 bytes in hex and decimal, plus Float and Double). Bytes can be edited in
place: hex cells take one nibble at a time with the pending digit shown, decimal
and float cells commit on Enter and cancel on Esc, and every write — including a
range fill dragged across the grid — goes on a 256-entry undo stack. `Back` and
`Forward` move through the pages you have visited.

**Pointer scan** — reverse search from a target address for multi-level pointer
paths, with an optional static-module filter, producing records you can drop
straight into the address list. The four limits (collection slots, distinct
offsets per node, nodes per level, result count) are reported rather than hidden:
hitting one truncates the result set and says so.

**Process views** — Regions, Modules and Threads for the host process, as a
consistent snapshot per refresh. Read-only, and enforced by the access mask:
threads are opened with `THREAD_QUERY_LIMITED_INFORMATION`, so this code cannot
suspend, resume or terminate them. Memory protection, thread priorities and the
module list are never modified.

**Cheat tables** — the classic XML format, in a strict signed-32-bit subset (v1)
and with extended types (v2). Files from other tools import without losing what
this project cannot represent: unknown elements are kept and written back
verbatim, and record types it cannot execute — Auto Assembler scripts and the
like — come in as read-only opaque rows that survive export byte for byte.

**Hotkey** — the backtick key ` toggles the window. Sampling only, no keyboard
hook: host input is never intercepted, and the host's own keys keep working.

## Requirements

- Windows 10 or 11, **x64 only**. 
- To build: Visual Studio with the Desktop C++ workload (VS18 / MSVC) and a
  Windows SDK. `build.cmd` hardcodes the VS root near the top; change that one
  line if yours is installed elsewhere.
- To run: the [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).
  Without it everything still works except the interface, which falls back to a
  card saying so.

There are no other dependencies. The WebView2 SDK is vendored under
`third_party/webview2/`, and its **static** loader is linked in — a
side-by-side `WebView2Loader.dll` cannot be relied on when the module is loaded
into someone else's process. `build.cmd` fails the build if that import appears.

## Build

```bat
build.cmd
```

Produces `dist/x64/shadowtrainer.dll`. No installer, no packaging step.

## Load it

Any standard loading mechanism will do — `LoadLibraryW`, a debugger, an
injector; this project does not ship one. The DLL starts its window as soon as it
is loaded.

An external caller that wants to drive the window holds a load reference and
follows this order: request a stop, wait for it, then release the DLL. Stopping
unloads the module, so a later session means loading it again.

| Window contract | |
|---|---|
| Window | an unowned top-level window carrying the `SHADOWTRAINER_WINDOW` property |
| `WM_APP+1` | show |
| `WM_APP+2` | stop |
| `WM_APP+3` | toggle |
| `WM_CLOSE` | hides the window; it does not stop the session |

Hiding never stops a scan or a freeze.

## The API

`include/ce/api.h` and `include/ce/api_v2.h` are plain C, with 48 exports in
total: 17 in version 1, unchanged since it was frozen, and 31 in version 2, which
adds explicit struct versions, caller-provided buffers and the extended types.
Version 1 is not going away and is not quietly truncated — a v1 call against a
session it cannot represent is refused, not rounded off.

Everything is synchronous; there are no callbacks. A typical v2 call takes a
buffer and a capacity and reports the required size, so the same function answers
"how much do I need?" when called with `(nullptr, 0)`.

The boundary conditions, the exact comparison and rounding semantics, and what
each version refuses are documented in [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md).

## Repository layout

```
include/ce/    the public ABI: api.h (v1) and api_v2.h (v2)
src/core/      the scanning engine, the typed read/write layer, the pointer scanner
src/ui/        the window, the WebView2 host, the startup cover, the hotkey
src/bridge/    the JS↔C++ command bridge and its JSON
web/           the page: index.html, css/, js/, img/
third_party/   the WebView2 SDK (headers, static loader, Microsoft's licence)
docs/          subsystem records, in English and Chinese
```

`src/runtime.cpp` is the only translation unit at the top; each module directory
is its own include root. The page under `web/` also opens straight from disk —
an asset path is used as the URL and as the file location, so
`web/index.html` works on its own with its styles and scripts intact.

## Developing the interface

```bat
build\x64\dev_host.exe --dev
```

`dev_host` loads the DLL in-process with `web/` read from disk instead of the
embedded copies, so a stylesheet edit is F5 instead of a rebuild.
[`docs/WEBUI.md`](docs/WEBUI.md) covers the page, the asset pipeline, the bridge
protocol and the WebView2 lifecycle.

## Status

This is a work in progress with a deliberately narrow scope, and it is worth
being blunt about it. Scanning, the address list, freezing, the hex editor, the
pointer scanner, the process views, cheat-table import/export and the whole
interface are implemented and tested. The following are **not** implemented and
are not stubbed out to look as if they were: the debugger, disassembly and
assembly, Lua, Auto Assembler execution, the training-target format, symbol and
structure handling, plugins, Mono/CLR/Java support and speedhack. Driver, DBVM
and remote-service capabilities are explicitly out of scope, as is scanning a
process other than the one that loaded the module.

`docs/FEATURE_PARITY.md` is the row-by-row account of what is done, what is
partial and what is missing. If you want to know whether something works, read
that table before you read the code.

## Documentation

| | |
|---|---|
| [`docs/README.md`](docs/README.md) | index of the documentation |
| [`docs/WEBUI.md`](docs/WEBUI.md) | the interface: layers, assets, the startup sequence, the bridge protocol |
| [`docs/COMPATIBILITY.md`](docs/COMPATIBILITY.md) | semantics, limits and the ABI contract |
| [`docs/FEATURE_PARITY.md`](docs/FEATURE_PARITY.md) | the migration matrix |
| [`docs/PROCESS_VIEWS.md`](docs/PROCESS_VIEWS.md) | the read-only Regions / Modules / Threads views |
| [`docs/STATUS.md`](docs/STATUS.md) | development status, batch history and evidence policy |

Every document has a `_CN` twin written in Simplified Chinese.

## Repository contents

Everything needed to build and run the runtime is published here, test sources
and development tools included. What is not committed is either regenerable or
local: build output (`build/`, `dist/`), Visual Studio state and MSVC
intermediates, the agent scratch directory `.scratch/`, and `AGENTS.md`. See
`.gitignore`.

`baseline/pre-webui/` is an archive of the native interface that the WebView2
page replaced — its sources, its tests, its probe screenshots and the documents
that describe them. Nothing in the build refers to it and it is not a rollback
path; it is kept because there is no version control history to fall back on.

## Third-party

`third_party/webview2/` is Microsoft's WebView2 SDK, redistributed under its own
terms — see `third_party/webview2/LICENSE.txt` and `NOTICE.txt`. Nothing else is
vendored, and the WebView2 runtime itself is not bundled: the machine-wide
evergreen runtime is used.

## License

No licence file is published yet, so this project is currently all rights
reserved by default. If you intend to reuse it, open an issue first.
