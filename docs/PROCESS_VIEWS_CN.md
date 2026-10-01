# 宿主进程只读视图（Regions / Modules / Threads）

[English](PROCESS_VIEWS.md) | [中文](PROCESS_VIEWS_CN.md)

宿主进程的三个只读视图——它的内存区域、已加载模块与线程——以标签页形式出现在 WebView 页面中。它们只做枚举与展示；这里没有任何东西会改动宿主状态。本文涵盖它们对外暴露的 ABI、遍历如何工作、页面如何驱动它们，以及它们止步于何处。

## 范围

参考对象是 CE 的 `formmemoryregionsunit.pas`（468 行）、`frmEnumerateDLLsUnit.pas`（399 行）与 `frmThreadlistunit.pas`（1194 行），其中只取「枚举 + 展示」：

- CE 的区域窗口里唯一会改动宿主状态的动作是「设为可写」，而且由驱动门控。
- CE 的 DLL 窗口 100% 是枚举 / 展示 / 跳转——没有加载、卸载或注入。
- CE 的线程窗口约有 60-65% 是挂起 / 恢复 / 修改寄存器 / 清调试寄存器。这里一件都不做。

本批刻意排除在范围之外：修改内存保护属性、挂起或恢复线程、设置线程优先级、加载或卸载模块、跨进程枚举、符号 / PDB 名称、`NtQueryInformationThread` 这类未文档化的 API，以及区域表的映射文件名列（CE 的 `Extra` 列，需要 `GetMappedFileName`）。

只读不是靠自觉：线程遍历一开始就用 `OpenThread(THREAD_QUERY_LIMITED_INFORMATION)` 打开线程，所以在访问掩码上挂起或终止线程就不可能。

## ABI

```c
#define CE_V2_MAX_VIEW_ITEMS (1u << 20)
CE_API int CE_CALL CE_GetRegionsV2(CeRegionInfoV2* regions, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetModulesV2(CeModuleInfoV2* modules, uint32_t capacity, uint32_t* required);
CE_API int CE_CALL CE_GetThreadsV2(CeThreadInfoV2* threads, uint32_t capacity, uint32_t* required);
```

填充数组，而不是「先计数、再按下标取」：一次调用返回一份一致的快照，没有代际、缓存或刷新握手。

- `capacity` 与 `required` 计的是**元素**个数。`(nullptr, 0)` 加一个 `required` 指针就是大小查询。缓冲区过短时一个字节都不写，返回 `CE_INVALID_ARGUMENT`，并把 `required` 设好。缓冲区为 null 而容量非 0，以及 `required` 缺失，同样都返回 `CE_INVALID_ARGUMENT`。数量超过 `CE_V2_MAX_VIEW_ITEMS` 返回 `CE_UNSUPPORTED`。
- 容量约定与 `CE_GetResultV2` / `copy_text_v2` 一致；变的只是单位，从字节换成元素。
- `size` 与 `version` 是**输出**——实现会把它们写进每一个被填充的元素——与 `CeScanRequestV2` 那类输入结构体正好相反。这是刻意的：一个 5000 元素的数组，不该要求调用方逐元素预填一个版本号。
- `CE_V2_VERSION` 不变：`valid_v2` 要求 size 与 version 精确匹配，所以新增结构不会动到既有结构。

结构体大小被钉了两遍：一遍是 `src/bridge/ui_bridge.cpp` 里的 ABI `static_assert`（同一段还把 `CeScanStatusV2` 钉在 80 字节），另一遍是 `runtime_v2_tests` 里的运行期断言。

| 结构体 | 大小 |
|---|---|
| `CeRegionInfoV2` | 48 |
| `CeModuleInfoV2` | 552 |
| `CeThreadInfoV2` | 168 |

`valid_mask` 逐位标记字段的有效性。清零的位意味着该字段为零、不携带任何信息——不会有 `THREAD_PRIORITY_ERROR_RETURN` 这类 Win32 哨兵值漏进 ABI。

| 位 | 字段 | 置位条件 |
|---|---|---|
| 0 | `description` | `GetThreadDescription` 成功且名称非空 |
| 1 | `priority` | `GetThreadPriority` 未返回 `THREAD_PRIORITY_ERROR_RETURN` |
| 2 | `created` | `GetThreadTimes` 成功 |

`priority` 把 `GetThreadPriority` 的 -15..15 有符号值原样存进一个 `uint32_t`；`created` 是原始 FILETIME（自 1601-01-01 UTC 起的 100 ns 单位），不做换算。

## 枚举

`src/core/process_view.inc`，由 `src/core/core.cpp` 在 `typed_core.inc` / `typed_table.inc` 之后以文本方式包含。做成 `.inc` 而不是新增一个 `.cpp`，是为了让 `build.cmd` 与 `src/shadowtrainer.vcxproj` 里的源文件列表保持不动。

| 视图 | 遍历 | 说明 |
|---|---|---|
| Regions | `VirtualQuery`，从 `lpMinimumApplicationAddress` 到 `lpMaximumApplicationAddress` | 循环形状沿用 `typed_scan.cpp` 的范围遍历，并带一道 `next <= cursor` 的防御；`MEM_FREE` 与 `MEM_RESERVE` 也包含在内，因为 CE 的 State 列同样包含它们 |
| Modules | `CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, ...)` | 刻意**不**加 `TH32CS_SNAPMODULE32`：它的存在是为了给 64 位进程展示 WOW64 模块，两者组合会产生重复行。宿主可能正处在 `LoadLibrary` 里，所以按文档记载的 `ERROR_BAD_LENGTH` / `ERROR_PARTIAL_COPY` 路径重新取快照，最多四次。 |
| Threads | `TH32CS_SNAPTHREAD`，按 `th32OwnerProcessID` 过滤 | 当前线程走 `GetCurrentThread()` 伪句柄，跳过 Open/Close；`GetThreadPriority` / `GetThreadTimes` / `GetThreadDescription` 中任一失败，只清掉它自己对应的位，而不是让整次调用失败 |

三者都不取 `state_mutex`：它们不读任何 Core 状态，而 `tick_freezes` 每 80 ms 就要拿一次互斥量——持着它跑完整个区域遍历会拖住冻结线程。先例是 `format_value_v2`，它只读 `stopped`。

## UI

这三个标签页是导航索引 3/4/5（总共七页），由 `web/js/views.js` 根据桥推送的 `views` 区块渲染。一个页面由说明行、表格、计数标签，以及「上一页 / 下一页 / 刷新」组成。标签文字是 `<Kind>: <n> total | <first> - <last> | 512/page`，首次加载之前是 `<Kind>: not loaded`。

**快照由桥持有**：`Session` 里的行数组就是那份快照，翻页只重绘当前切片。每次翻页都重新枚举，会让行在读者眼皮底下移动，还会把遍历成本乘以页数。

刷新策略：`view.load` 只枚举一次，且只在该页从未加载过时执行；此后只有显式的 `view.refresh` 才会重新遍历。一次区域遍历是每个区域一次系统调用——小进程 300-500 次，繁重宿主几万次——所以它绝不能每点一次标签就跑一遍。扫描进行中发出的「刷新」会记下一个待处理标志，由既有的 100 ms 轮询定时器（裸 id 1，`src/ui/webui.cpp`）在桥空闲后执行：**不新增定时器**。

## 测试

- `runtime_v2_tests`：三个 `sizeof`；数量查询；区域从 `lpMinimumApplicationAddress` 起平铺整个地址空间，并覆盖调用者的栈地址，且 `MEM_COMMIT` 与 `MEM_FREE` 两者都在；模块表包含主 exe；恰好一个线程被标记为 current；缓冲区过短时什么都不写。取快照会与活动中的宿主竞争——区域会在大小查询与填充之间新增——所以 `fill_view` 会重试（四次，每次重读数量），断言也只依赖那些在活动宿主下依然成立的不变量（`tests/runtime_v2_tests.cpp`）。
- `typed_tests`：同样的检查直接走 `ce::Core`，外加 `shutdown()` 之后三者都返回 `CE_NOT_RUNNING`。
- `bridge_tests`：三种视图的 `view.load`、行内容（区域各列、模块路径、线程 current 标记）与标签，模式 A（假 `CeApi`）与模式 B（真 DLL）一视同仁。

## 已知边界

- 模块路径是内联的，定长 260（`MAX_PATH`），更长则截断；线程描述定长 64。
- 区域表包含 Free 与 Reserve，所以它的行数比「已提交区域」多；`state == MEM_FREE` 的行，其 `type` 与 `allocation_protect` 为 0，`protect` 为 `PAGE_NOACCESS`。
- 没有任何轮询，所以数据就是按下「刷新」那一刻的状态；在活动宿主里，线程与区域会持续变化。
- `CreateToolhelp32Snapshot` 的调用位置限制见 `docs/COMPATIBILITY_CN.md`——不要从 `DllMain` 调用它。
