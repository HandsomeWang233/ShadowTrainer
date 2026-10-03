# Development Status and Evidence Policy

[English](STATUS.md) | [中文](STATUS_CN.md)

This is the working record, not the pitch. It says what state the code is in, what
each batch changed, and what this project counts as evidence. For the user-facing
overview see [`../README.md`](../README.md); for the subsystem records see the
[documentation index](README.md).

## Where the project stands

- **Artifact**: `dist/x64/shadowtrainer.dll`, built by `build.cmd` with VS18 / MSVC
  and the Windows SDK. 
- **UI**: WebView2. The page is the HTML/CSS/JS under `web/`, compiled into the DLL
  as `RCDATA` and served from the virtual origin `https://shadowtrainer.local/`.
  Nothing is written to disk. See [`WEBUI.md`](WEBUI.md).
- **Only third-party dependency**: `third_party/webview2/` — Microsoft's WebView2
  SDK headers and the **static** loader library. Static is a hard requirement: the
  DLL is injected into an arbitrary host process and cannot rely on a
  side-by-side `WebView2Loader.dll`. `build.cmd` fails the build if
  `dumpbin /dependents` shows that import. Provenance and re-fetch procedure:
  `third_party/webview2/VERSION`, `tools/fetch-webview2.cmd`.
- **Public ABI**: the `CE_*` exports in `include/ce/` — 17 in `api.h` (version 1,
  unchanged) plus 30 in `api_v2.h` (version 2), 47 in total.
- **Repository hygiene**: `build/` and `dist/` are disposable output and safe to
  delete at any time; anything worth keeping belongs under `baseline/` or `docs/`.
  The published tree is a subset of this working copy — see `.gitignore`.

## Evidence policy

`build.cmd` **only builds**; tests are run explicitly afterwards.

| Suite | Needs |
|---|---|
| `core_tests`, `typed_tests`, `polish_core_tests`, `pointer_scan_tests`, `hotkey_tests` | nothing (DLL-independent) |
| `bridge_tests`, `runtime_tests`, `runtime_v2_tests` | the path to `dist/x64/shadowtrainer.dll` |
| `bun test tests\web` | no packages; runs the six JS suites against the real `web/js/*.js` |

**Known gap.** There is no configured Lazarus/FPC toolchain and no compiled
Tutorial on this machine, so the differential run against the reference
implementation has never been executed. The self-built test host is not a
substitute for it. If a test is red, check this section and
[`FEATURE_PARITY.md`](FEATURE_PARITY.md) before treating it as a regression.
