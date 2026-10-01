# Documentation

[English](README.md) | [中文](README_CN.md)

Every document here has a Simplified Chinese twin with the same name plus a `_CN`
suffix. Both versions carry the same facts; fix a fact in one and fix it in the
other.

## The records

| Document | Covers | Read it when |
|---|---|---|
| [`WEBUI.md`](WEBUI.md) | The interface: the four layers, the embedded asset pipeline, the startup sequence, the bridge protocol, the WebView2 lifecycle | you are changing anything under `web/`, `src/ui/` or `src/bridge/` |
| [`COMPATIBILITY.md`](COMPATIBILITY.md) | Semantics and limits: the ABI contract, types and comparisons, floating-point rounding, scanning rules, records and pointers, cheat tables, the pointer scanner | you need to know what a call does at the edges, or what it refuses |
| [`FEATURE_PARITY.md`](FEATURE_PARITY.md) | The migration matrix: one row per capability, its status, and its acceptance boundary | you want to know whether something works before reading the code |
| [`PROCESS_VIEWS.md`](PROCESS_VIEWS.md) | Regions / Modules / Threads: the fill-an-array ABI, the enumeration, the UI wiring, the known boundaries | you are touching `src/core/process_view.inc` |
| [`POLISH.md`](POLISH.md) | One batch of reliability work on already-implemented features, and the durable rules it settled | you are looking for why Undo, the value refresh or the freeze conflict check behaves as it does |
| [`STATUS.md`](STATUS.md) | Development status, the batch history, the evidence policy and the known gaps | you are picking the project up, or a test is red |

## Images

`img/webui/` holds the interface screenshots.
