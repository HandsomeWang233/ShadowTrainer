# 文档

[English](README.md) | [中文](README_CN.md)

这里每篇文档都有一份简体中文对照版，文件名相同，只是多了 `_CN` 后缀。
两版承载的事实一致；在一份里改正了事实，就要在另一份里同步改正。

## 记录

| 文档 | 覆盖内容 | 何时阅读 |
|---|---|---|
| [`WEBUI_CN.md`](WEBUI_CN.md) | 界面：四个分层、内嵌资源管线、启动流程、桥接协议、WebView2 生命周期 | 你要改动 `web/`、`src/ui/` 或 `src/bridge/` 下的任何东西时 |
| [`COMPATIBILITY_CN.md`](COMPATIBILITY_CN.md) | 语义与边界：ABI 约定、类型与比较、浮点舍入、扫描规则、记录与指针、作弊表、指针扫描 | 你需要弄清某个调用在边界上做什么，或者它会拒绝什么时 |
| [`FEATURE_PARITY_CN.md`](FEATURE_PARITY_CN.md) | 迁移矩阵：每个能力项一行，含它的状态与验收边界 | 你想在看代码之前先知道某个功能能不能用时 |
| [`PROCESS_VIEWS_CN.md`](PROCESS_VIEWS_CN.md) | Regions / Modules / Threads：填充数组式 ABI、枚举实现、UI 接线、已知边界 | 你要动 `src/core/process_view.inc` 时 |
| [`POLISH_CN.md`](POLISH_CN.md) | 针对已实现功能的一批可靠性工作，以及它定下来的长期规则 | 你想弄清 Undo、当前值刷新或冻结冲突检查为什么是现在这个行为时 |
| [`STATUS_CN.md`](STATUS_CN.md) | 开发状态、批次历史、证据策略与已知缺口 | 你刚接手这个项目，或者某个测试变红时 |

[`agents/`](agents/) 存放三份供仓库维护技能读取的流程文档：
[`domain_CN.md`](agents/domain_CN.md)（如何使用这些记录）、
[`issue-tracker_CN.md`](agents/issue-tracker_CN.md)（工单与规格放在哪）以及
[`triage-labels_CN.md`](agents/triage-labels_CN.md)（标签词表）。
它们是配置而不是文档：其中被技能解析的字面量（标签名、`Status:` 之类的字段名）在两种语言版本里都保留英文原文。

## 图片

`img/webui/` 存放界面截图。
