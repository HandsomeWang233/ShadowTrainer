# Web UI

[English](WEBUI.md) | [中文](WEBUI_CN.md)

窗口是一个 WebView2 控件，显示由 `web/` 构建出的页面。以前放在这里的 UI——`ui.cpp`、`ui_theme.cpp`、`ui_dropdown.cpp` 以及它们驱动的那些自绘控件——已经没了；一份副本归档在 `baseline/pre-webui/` 下，仅供参考，构建里没有任何地方再引用它。

布局、标签、列宽、分页、状态字符串和语义都沿用原生 UI 的那一套。只有绘制和输入处理是新的。

## 分层

| 层 | 文件 | 负责什么 |
|---|---|---|
| 宿主层 | `src/ui/webui.cpp`、`src/ui/webui.hpp` | 窗口、定时器、热键、文件对话框、回退卡片 |
| 启动封面（cover） | `src/ui/cover.cpp`、`src/ui/cover.hpp` | 启动词序列，在还没有页面可显示时用 GDI 绘制 |
| 管道层（plumbing） | `src/ui/webview_host.cpp`、`src/ui/webview_host.hpp` | WebView2、内嵌资源服务器、消息通道 |
| 语义层 | `src/bridge/ui_bridge.cpp`、`src/bridge/ui_bridge_commands.inc`、`src/bridge/ui_bridge_sections.inc`、`src/bridge/json_min.inc` | 每一条命令、每一个状态字符串、工作线程、缓存 |
| 页面 | `web/**` | 渲染与输入。它不持有任何事实 |

前三层一起住在 `src/ui/`；第四层是 `src/bridge/` 里独立的模块，因为只有它有无界面测试。扫描引擎和 ABI 位于 `src/core/`，`src/runtime.cpp` 是顶层剩下的唯一一个编译单元。每个 `src/<module>/` 都是自己的包含根目录（`build.cmd` 和 `src/shadowtrainer.vcxproj` 都会把它们加进去），所以源码里一直写 `#include "core.hpp"`，而不是写成某个目录名。

这样拆分是有意义的，因为中间那层是唯一一个离不开浏览器就没法测试的层。`ui_bridge` 通过函数指针表（`CeApi`）而不是导出符号与核心通信，所以 `tests/bridge_tests.cpp` 能用假实现无界面地跑遍每个按钮动作，再对着真实 DLL 跑一遍。

`runtime.cpp` 和以前一模一样地调用 `ce::run_ui`；外部调用方依赖的窗口契约没有变：一个可见的、无主的顶层窗口，带 `SHADOWTRAINER_WINDOW` 属性，`WM_APP+1` 显示，`WM_APP+2` 停止，`WM_APP+3` 切换，`WM_CLOSE` 只隐藏，停止时卸载模块。

## 资源

页面以 `RCDATA` 编译进 DLL，并从虚拟源 `https://shadowtrainer.local/` 提供。不会写任何东西到磁盘，所以被注入的 DLL 永远不必在任意宿主可执行文件旁边找一个可写目录。

- 三张列表列出同样的十五个条目：`web/webui.rc` 内嵌文件，`web/resource_ids.h` 给它们编号，`src/ui/webview_host.cpp` 里的表声明每个 MIME 类型。一个条目带一个 `path`，它既是 URL，又是 `web/` 下的位置，因此必须与 `.rc` 对得上。弄错的两种方式失败方式不同：表里有而 `.rc` 没有内嵌的路径，在运行时报 404；`.rc` 里写了但磁盘上没有的路径，会让构建停下。`brand.png` 和 `app.ico` 是例外：`brand.png` 像其他任何资源一样被提供，而 `app.ico` 只为窗口类内嵌（`LoadImageW`/`WM_SETICON`，从不对外提供），所以只有这个图标不进表。
- URL 镜像 `web/`：页面请求 `/js/app.js`，拿到的是 `web/js/app.js`。一个字符串干两份活——表条目、`.rc`、页面自己的 `<link>`/`<script>`/`<img>` 都把它写成同样的形式——这也是 `web/index.html` 直接从磁盘打开时样式表、脚本和 logo 都完好的原因。
- `find_asset` 用**完全相等**把请求与表进行匹配，这就是请求路径上的全部防护：手写的 URL 只可能命中列出的十五个条目之一，绝不会有派生自请求的字符串被交给 `CreateFileW`。条目里带分隔符也改变不了这一点——真正会削弱它的是把完全匹配改成前缀匹配或模式匹配。`webui.rc` 和 `resource_ids.h` 留在 `web/` 根目录下，正是这一点让 `build.cmd` 可以继续传一个朴素的 `/Iweb`。
- `build.cmd` 带着 `/Iweb` 运行 `rc.exe`。（已验证：`rc.exe` 通过包含路径而不是工作目录解析 `RCDATA` 文件名，所以这条命令在这里和在 MSBuild 构建 `shadowtrainer.vcxproj` 时行为一致。）
- 每个文本资源都是纯 ASCII，所以 `rc.exe` 无法对它重新编码。两个二进制资源原样穿过：`RCDATA` 原样复制字节，`ICON` 由 `rc.exe` 自己解析。
- 请求由 `WebResourceRequested` 应答。过滤器是一个 URI **模式**：`https://shadowtrainer.local/*`。没有结尾的 `*`，就只有完全相同的 URL 能匹配，于是每个真实请求都会逃到网络上。

`build\x64\webui.res` 随页面一起变大，而 `dumpbin /dependents` 里始终没有 `WebView2Loader.dll`，因为 loader 是静态链接的——对一个被注入的模块来说，并列放置（side-by-side）一个 DLL 不是可选项。

### 开发循环

```bat
build\x64\dev_host.exe --dev
```

`SHADOWTRAINER_WEBUI_DIR` 让宿主从磁盘读取 `web/`，而不是读内嵌副本；`SHADOWTRAINER_WEBUI_DEV=1` 重新打开 DevTools，所以改样式表只需按 F5，不用重新构建。重新加载是安全的：桥接层活在 C++ 里，所以一次重新加载只会重发 `hello` 并重绘。

磁盘读取会为一次请求打开 `web/<path>`，这正是 `.rc` 内嵌的同一个字符串——没有第二份映射需要保持同步。回退是一直就有的那个：如果读取失败，`load_asset` 改为提供内嵌的 `RCDATA` 副本，**悄无声息**。没有任何东西测试那个分支，所以如果一次修改不再生效，先怀疑回退，再怀疑浏览器。

## 启动流程

`Shadow` 从右侧飞入，停在正中央，`Trainer` 的字母在它身后逐个长出，随后 UI 从窗口的精确中心向外铺开。**窗口播放这个词；页面播放揭幕动画。** 这个分工就是整个设计，它存在是因为窗口已经在屏幕上时，WebView2 还要一两秒才能起来。

### 由窗口绘制的文字

`src/ui/cover.cpp` 在页面还在启动时用 GDI 绘制它。它想做到的是页面自身的渲染，而非对页面的近似，所以它与样式表本该产出的东西一致：

- 同一个字体族（机器上有就用 `Segoe UI Variable Text`，否则用 `Segoe UI`）、同样的 em 尺寸（GDI 的 `lfHeight` 直接接受它）、同样的抗锯齿：用 `ANTIALIASED_QUALITY`，不用 ClearType，因为页面设置了 `-webkit-font-smoothing: antialiased`，得到的是灰度。在这里用 ClearType 会给每根竖笔加上红蓝彩边——在暗底上的大号白色字形下非常明显——还会让这个词的两份渲染看起来不一样；
- 同样的 `letter-spacing: -0.02em`，通过 `SetTextCharacterExtra` 施加。GDI 会把这份额外字距同时作用到 `GetTextExtent` 和 `TextOut`，所以测量出的宽度和绘制出的宽度都与页面保持一致。（实测：不施加它，这个词会宽出 12%，因为页面的字距是负的，而封面之前根本没有施加任何字距。）
- 同样的时间曲线，是解出来的而不是近似出来的：`bezier` 真正运行样式表里写的 `cubic-bezier(0.16, 1, 0.3, 1)` 和 `cubic-bezier(0.2, 0.9, 0.25, 1)`；
- 页面把字母长进的那个盒子与字母本身区分开，这里也是同样的区分。盒子沿贝塞尔曲线变宽；字母则**线性**淡入、上浮并缩放（从 `translateY(0.45em) scale(0.7)` 起），通过它自己的世界变换，原点在自己的盒子中心。反过来绕左边缘缩放，会让每个字母横向平移，把整行拖离中心最多 20px——而这正是第一版干的事；
- 对整行的盒子做裁剪，相当于 `line-height: 1` 元素上的 `overflow: hidden`，这样字母上升到位时是被这一行揭示出来，而不是被画到行外；
- 每一帧都重新把整行居中，所以屏幕上出现的东西始终留在窗口中央；
- 页面的两种颜色（`#f6faff`、`#b9e6ff`）、词背后的径向渐变光晕，以及它下方的 `INITIALISING` 一行。

随后完整的词会停留两秒再淡出：长到足以被读出来，而不是在 UI 出现前闪一下。

这些没有一样能靠跑测试来检查——测试期间没办法看窗口——所以 `cover.cpp` 是一个不依赖窗口的独立编译单元。这样，一个一次性的探针就能把它的若干帧渲染进位图，和浏览器的帧并排放在一起，上面那个变换 bug 就是这么找到的，词的基线落在页面的同一像素上也是这么确认的。

### 离屏合成

封面升起期间，窗口不直接绘制任何东西。`paint_boot` 把这一帧合成进一个客户区大小的 `CreateCompatibleBitmap`，然后一次性位块传送（blit）上去。改成直接画到窗口上，会看到每一个中间状态——底色填满、光晕叠加其上、然后是词——而在每秒六十帧下，这些步骤读起来就是闪烁；一帧静止画面只会让它更糟而不是更好，报上来的就是这样。`WM_ERASEBKGND` 被回答为「已经擦除」也是同样道理：在那里擦除，会让光秃秃的底色在合成帧盖上来之前的一瞬间出现在屏幕上。

这不是一次性能修复。在 1360x900 下，一个合成帧加上位块传送测得 0.67ms，而帧预算是 16.7ms，所以从来就没有速度问题——问题只是让一个没画完的帧被人看到。

### 由合成器驱动

要让动效和页面的一致，有两样东西必须改，两者都和绘制速度无关。

**时钟。** `now_ms()` 用的是 `QueryPerformanceCounter`，不是 `GetTickCount64`。后者只在系统时钟跳动时才前进——大约每 15.6ms 一次，除非进程里有东西提高了定时器精度——所以每帧采样一次，交给动画的是一个以台阶跳变、台阶之间静止不动的时间。于是帧要么画同一瞬间，要么画往前好几毫秒的瞬间，无论帧到得多快，读起来都是不均匀的运动。

**节拍。** 帧由一个第二线程调用 `DwmFlush` 来投递，它在下一次合成时返回——这正是浏览器驱动页面那一份副本所用的同一个时钟。`SetTimer` 是那个显而易见的来源，也是错的那个：它只精确到系统时钟节拍，而 `WM_TIMER` 是低优先级消息，所以帧与帧之间的间隔会漂移好几毫秒。节拍器从第一个绘制帧开始，在交接时停止，所以一个已经稳定的窗口不会白白每秒重绘六十次。

`DwmFlush` 和上面那个圆角调用一样，是从 `dwmapi.dll` 手工解析出来的；如果它缺失，或者合成被关掉——比如远程会话——flush 就会失败，循环回退成 16ms 的 sleep。

### 由页面播放的揭幕动画

`Host::start(..., hold_visible)` 是促成这次交接的东西。控制器被创建出来但保持隐藏，所以页面不可能出现在封面背后；词播完后 `Host::reveal()` 把它显示出来。这也是封面能撑过加载的原因——页面在它身后等待，已经完整渲染好，直到被需要为止。

`boot.js` 等窗口调用 `window.__shadowtrainerReveal`，然后通过一个从窗口精确中心长出来的洞把幕布掀起。遮罩保留洞以外的一切，所以应用是先露出中心，看起来像从中间向外铺开。

窗口在封面结束时发起这个调用，之后每当页面说 `hello` 时再发一次。第二次正是重新加载所依赖的：封面每个进程只播一次，所以重新加载会错过第一次调用，否则就得把超时等完。`boot.js` 有一个 8 秒的回退，`index.html` 有一个 12 秒的，后者是给那种根本加载不起来的模块图兜底的。

### 单独打开时

没有桥接层时，没有东西播放那个词，也没人发出放行信号，所以 `boot.prepare()` 等一拍然后掀起。幕布仍然在，所以浏览器预览只会看到揭幕动画，别的什么都没有。

### 保持低开销

里面没有任何地方使用 `filter`：对活文字做模糊会让元素每帧重新光栅化。封面用朴素的 GDI 绘制，页面那一半是在平坦底色上盖一层遮罩，这已经是它能做到的最便宜的做法。

## 桥接协议

一条 JSON 通道，双向。

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

- **每一个 64 位量都以十进制字符串穿越**——地址、id、代际、计数、尺寸。JSON 数字无法精确承载一个 `uint64`，JavaScript 的 `Number` 也装不下。`json::Writer::u64` 发出字符串；`Value::as_u64` 两种形式都接受。
- 状态字符串只在 C++ 里合成（`status_name()` 和旧的 `"<operation>: <name> (<code>)" + " - <detail>"` 格式）。页面原样渲染它们，从不自己构造一条。
- 第一条命令是 `hello`，用整个模型来应答（绕过缓存）；事件在那之后才开始。所以页面重新加载是无状态的。
- 区块：`status`、`scan`、`pointer`、`results`、`resultRows`、`records`、`recordRows`、`views`、`controls`。行是**增量**推送的（`{reset, from, rows}`），所以扫描期间一次被动的重新加载只重绘出现过的部分。
- 改变某个列表的命令（`results.page`、`record.refresh`、`view.load`、`ptr.refresh` 等）会用它们所改变的那个区块来应答，这样页面就从回复里绘制，而不用等下一个节拍。
- `window.*` 和 CT 文件对话框通过 `Session::Host` 转发给宿主；桥接层无法打开原生文件选择器，也不假装能。

## 节拍

`webui.cpp` 保留原生 UI 的两个定时器：轮询用 100 ms，反引号采样器用 16 ms。每次轮询调用 `Session::pump(visible, minimized)`，也就是旧的 `WM_TIMER` 主体：读取扫描状态（一次 `CE_BUSY` 快照是一次*落空*，不是一个状态——保留上一次良好的遥测，那一节拍不碰任何数据 API）、在取消挂起期间反复发出 `CE_CancelScan`、轮询指针任务、join 一个已完成的工作线程、消费延迟刷新标志，然后加载被动结果行（16 rows / 64 KiB / 12 ms）并对实时记录值采样（间隔 500 ms，轮转）。隐藏窗口只抑制最后两项。

一个工作线程完成时发出 `WM_APP+5`，这样完成在一毫秒级就能被看到，而不是等到下一个节拍；`pump` 也会注意到它，所以定时器是安全网，而不是机制本身。

## 门控

有意拆开，因为每次按键都重发一整张启用/禁用映射会显得很蠢：

- 运行时推送由核心状态直接推出的东西：`idle`、`busy`、`haveScan`、`canUndo`、`recordsStale`、选区、页边界、视图边界、内存标志和指针任务。
- `web/js/gating.js` 推导字段级规则——哪些比较方式能发起扫描、格式在第一次提交的扫描后就锁定、舍入只适用于精确的 Float/Double 扫描、导入的不透明记录可以移除但永远不能写入或冻结。
- 命令到达时，运行时会**重新检查同一批判定条件**，并回复 `CE_BUSY`，而不是默默忽略它——这是对原生 UI 那种默默拒绝的有意改变。

`tests/web/gating.test.js` 把规则表钉住。

## 仍由原生实现的部分

窗口本身、`WM_NCHITTEST` 的六像素缩放边框、自定义的非客户区处理、最小化/最大化/关闭、圆角（Windows 11 上是 DWM，否则用窗口区域）、反引号采样器、两个定时器、`WM_APP+1/2/3`、文件对话框，以及那张代替页面的卡片。

拖动和缩放交还给 Windows：页面带着屏幕坐标发出 `window.drag` / `window.beginResize`，宿主运行 `ReleaseCapture()`，随后投递一个 `WM_NCLBUTTONDOWN`，正是它启动了 Windows 自己的移动/缩放循环。

## WebView2 生命周期

- `CoInitializeEx(APARTMENTTHREADED)` 在 UI 线程上做一次，在 `run_ui` 返回时释放。`DllMain` 里不发生任何与 COM 有关的事。
- 窗口被创建、显示并发布，`CE_RUNNING` 被设置，这些都发生在 WebView 创建**之前**。运行时慢或者缺失，永远不会拖慢会话。
- 用户数据文件夹是 `%LOCALAPPDATA%\ShadowTrainer\WebView2\<pid>`——按进程分开，因为 WebView2 拒绝共享配置文件；也绝不放在宿主可执行文件旁边，那里可能不可写。死进程留下的文件夹会在下次启动时清扫；还活着的那个在关闭时**不**删除，因为浏览器进程还在用它。删掉它就会杀掉那个进程（随后它会报 `ProcessFailed` 并显示卡片），而同一进程里下一次加载会继承这片残骸。
- 关闭顺序：移除事件处理器、`controller->Close()`、然后释放。先释放 web view 会让 WebView2 启动它自己的拆卸流程，那会泵送消息并把它们派发进半释放的对象里。
- 拆卸运行在消息循环自己的栈上，而不是在某个窗口过程内部；会话一结束循环就结束：任何仍然排队等着 WebView2 自有窗口的消息，都会被派发进已经拆卸掉的对象里。

### 代价最大的那个坑

完成处理器的 arguments 是**借用引用（borrowed reference）**——调用方拥有它们，并在 `Invoke` 返回时释放。不 `AddRef` 就把它存下来，它会立刻悬空。现象极具误导性：每一次调用都返回 `S_OK`，控制器回调触发了，`put_Bounds`、`put_IsVisible` 和 `Navigate` 全都成功——可 WebView 就是从不创建它的子窗口，页面从不出现，而 `Close()` 在一块已释放的内存上出错。

因此 `ComPtr::operator=(T*)` 会 AddRef（指针仍归调用方所有），而 `attach()` 是显式的「我拥有这个引用」形式，用在转交一份全新引用的地方。

### 另一个坑

`ICoreWebView2Controller` **一存在就可见**。启动封面需要把它按住，等词播完，而那个看起来显而易见的做法——「干脆别调用 `put_IsVisible(TRUE)`」——压根没有用：页面一就绪就冒出来，大约一秒进来，把封面从中间截断。现象是封面画出「Shadow」，然后变全黑，这读起来像绘制 bug，而不像可见性问题。`put_IsVisible(FALSE)` 必须显式请求。

## 运行时缺失时

会话仍活着，`CE_RUNNING` 带着它惯常的窗口；只有 UI 降级了。`WM_PAINT` 画出一张卡片，写明缺失的运行时、安装地址和错误，并指出 CE API 仍然可用。当 WebView 只是*正在启动*时，改画一行更安静的文字——早先的版本在那一两秒里显示失败卡片，那是彻头彻尾错了。一次 `ProcessFailed`（浏览器进程死了）落到同一张卡片上，并且不会停止会话。

## 测试

| 位置 | 内容 |
|---|---|
| `tests/bridge_tests.cpp` | 每一条命令。模式 A 用假实现伪造 `CeApi`（拒绝、请求构造、分页钳制、事件形状）；模式 B 加载真实 DLL，把同样的命令端到端走一遍。需要 DLL 路径。 |
| `tests/web/hexview.test.js` | 十六进制布局表、地址顺序的半字节规则、按模式的值格式化、`%g`。取代 `hex_geometry_tests.cpp`。 |
| `tests/web/gating.test.js` | 启用/禁用规则。 |
| `tests/web/enums.test.js` | 解析 `include/ce/api_v2.h` 和 `api.h`，断言 `format.js` 镜像了每一个枚举值。 |
| `tools/dev_host.cpp` | 不是测试：在进程内加载 DLL，好让页面能被编辑和重新加载。 |

`bun test tests/web` 不需要任何包。`runtime_tests` 和 `runtime_v2_tests` 没有变，仍然是窗口契约上的门——其中包括三轮加载/停止/卸载循环，正是它证明了在 `FreeLibraryAndExitThread` 之前 WebView 真的没了。

## 仍缺失的部分

- 十六进制视图的分组模式能渲染和编辑，但选择是在单元格上拖出来的一个字节区间；没有从点击出发的逐半字节命中测试，而原生控件通过它的文本插入符是有这个的。
- 显示模式没有键盘快捷键（原生版本也没有）。
- `docs/img/webui/` 存放当前的截图。
