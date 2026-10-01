# 开发状态与证据策略

[English](STATUS.md) | [中文](STATUS_CN.md)

这是工作记录，不是宣传稿。它说明代码当前处于什么状态、每个批次改动了什么，以及本项目把什么算作证据。面向用户的概览见 [`../README_CN.md`](../README_CN.md)；各子系统的记录见[文档索引](README_CN.md)。

## 项目现状

- **产物**：`dist/x64/shadowtrainer.dll`，由 `build.cmd` 配合 VS18 / MSVC 和 Windows SDK 构建。
- **UI**：WebView2。页面是 `web/` 下的 HTML/CSS/JS，作为 `RCDATA` 编译进 DLL，并从虚拟源 `https://shadowtrainer.local/` 提供。不向磁盘写入任何内容。见 [`WEBUI_CN.md`](WEBUI_CN.md)。
- **唯一的第三方依赖**：`third_party/webview2/`——微软的 WebView2 SDK 头文件与**静态**加载器库。静态是硬性要求：DLL 会被注入任意宿主进程，不能依赖并列放置的 `WebView2Loader.dll`。若 `dumpbin /dependents` 显示出该导入，`build.cmd` 会让构建失败。来源与重新获取流程：`third_party/webview2/VERSION`、`tools/fetch-webview2.cmd`。
- **公共 ABI**：`include/ce/` 中的 `CE_*` 导出——`api.h` 中 17 个（版本 1，未变），外加 `api_v2.h` 中 30 个（版本 2），共 47 个。
- **仓库卫生**：`build/` 与 `dist/` 是可丢弃的产物，随时可以删除；值得保留的东西都属于 `baseline/` 或 `docs/` 之下。发布树是本工作副本的子集——见 `.gitignore`。

## 证据策略

`build.cmd` **只负责构建**；测试在其后显式运行。

| 测试套件 | 依赖 |
|---|---|
| `core_tests`、`typed_tests`、`polish_core_tests`、`pointer_scan_tests`、`hotkey_tests` | 无（不依赖 DLL） |
| `bridge_tests`、`runtime_tests`、`runtime_v2_tests` | `dist/x64/shadowtrainer.dll` 的路径 |
| `bun test tests\web` | 不需要任何包；对真实的 `web/js/*.js` 运行三个 JS 测试套件 |

**已知缺口。** 本机没有配置 Lazarus/FPC 工具链，也没有编译好的 Tutorial，因此针对参考实现的差分运行从未执行过。自建的测试宿主不能替代它。如果某个测试是红的，先查看本节与 [`FEATURE_PARITY_CN.md`](FEATURE_PARITY_CN.md)，再把它当作回归。
