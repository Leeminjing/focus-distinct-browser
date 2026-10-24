# 首版实现

宿主只认识浏览器、调用者、租约、窗口和网页，不包含上层应用的业务生命周期或对象映射。

## 执行结构

`src/main.cpp` 管理进程、Win32、WebView2 和协议；`src/control.h` 是不依赖 Windows API 的控制权状态机；`src/json.h` 包装系统 `Windows.Data.Json`。JSON 投影来自 Windows SDK 的 C++/WinRT，不增加分发组件。

一个 STA UI 线程持有所有 HWND、WebView2 COM 对象和控制状态。管道读取线程按行读取数据，投递至 message-only window；UI 线程解析并处理请求。输入消息队列最多 128 条，单行最多 1 MiB。独立输出线程写 stdout，防止管道反压阻塞原生窗口；队列超过 64 MiB 时撤销连接。

WebView2 的创建、截图和 CDP 操作均使用异步回调。A 等待结果时，UI 消息循环继续处理 B。每个浏览器最多一个在途操作，后来的同页请求被明确拒绝为 `busy`，不建立可能在交接后恢复的动作队列。

## 创建与隔离

每个浏览器生成新的 GUID，并创建顶层原生窗口、独立用户数据目录、Environment、Controller 和网页对象。初始化和首次导航都成功后才返回创建成功；窗口提前关闭、初始化失败或超时都会返回错误。

用户数据目录为 `%LOCALAPPDATA%/DistinctBrowser/Profiles/<browser_id>`。宿主读取 Environment 实际使用的目录并核对，环境变量或系统配置若把它覆盖成共享目录，会使创建失败。一个浏览器实例对应一个网页和一个独立配置，不把窗口数量当作操作系统进程数量。

默认只选择稳定 WebView2 Runtime，启动浏览器前检查可用性。Loader 静态链接，CRT 使用 `/MT`。普通 Edge 浏览器、SDK 或 Python 都不是运行 EXE 的先决条件；WebView2 Runtime 仍然是。

## 观察与控制权

控制令牌覆盖“取得控制 → 观察 → 一段动作 → 释放”。租约采用单调时钟；定时器和每次实际派发前都检查到期。接管、最小化、隐藏、关闭、故障和断连都清除令牌与观察资格。

用户点击“接管”后立即阻止后续派发；有已发操作时显示“接管中”，回调收尾后转“人工控制”。最后一个已发步骤的真实成功仍可以返回；多步动作中尚未发送的步骤会被拒绝。交还只开放新租约，旧操作不会恢复。

CDP 输入直接发往指定 WebView2，不调用桌面 `SendInput`。多步 CDP 等前一回调再派发下一步。导航通过 WebView2 API：跨文档请求按自己的导航 ID 结算，与浏览器最新文档的加载状态分开处理；同文档请求根据 `SourceChanged` / `HistoryChanged` 和规范化目标 URL 结算。HTTP 重定向沿用同一 ID；不同 ID 的新导航使旧请求以 `navigation_superseded` 结束，旧完成事件不能清除新页面的加载标志。取消和正常失败保留 Runtime 原始状态并结算，不升级成宿主故障。导航或视口变化会使旧观察失效，防止继续使用明显过时的坐标。

截图通过 `CapturePreview` 写入内存 IStream，再编码为 PNG base64；CSS 视口来自 `Page.getLayoutMetrics`。真实窗口由 WebView2 直接渲染，没有截图播放层。

## 生命周期

最小化由 `WM_SIZE` 检测，隐藏由 `WM_SHOWWINDOW` 检测，派发前再检查 `IsIconic` / `IsWindowVisible`。遮挡或另一浏览器展示不会暂停当前窗口。恢复后保留原页面，要求用户点击交还并由新控制者重新观察。

关闭先撤销令牌并结算在途请求，再关闭 Controller 和 HWND。所有回调检查关闭或结算标志，窗口和标识不会被重建复用。WebView2 `ProcessFailed` 和 20 秒操作超时均进入故障状态；最小化、展示或交还不能清除故障，也不会重试结果不明的操作。

断连时保留已可用窗口供人工使用。宿主不拥有调用方主界面的窗口，因此调用方最小化不会改变浏览器资格。

## 验证范围与尚未支持

状态机、真实 WebView2 集成、管道异常和故障检查均有可执行测试。截图只验证网页区域；多显示器 DPI 切换、系统策略组合和长时间压力使用还需要额外覆盖。原型不做首个真人输入自动接管、多标签页、元素定位、下载管理、进程重启恢复或自动故障恢复。

## 微软参考

- [线程模型](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model)：COM 与回调保持在带消息循环的 STA UI 线程。
- [静态 Loader](https://learn.microsoft.com/en-us/microsoft-edge/webview2/how-to/static)：单 EXE 分发仍需要 WebView2 Runtime。
- [ICoreWebView2](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2)：CapturePreview、CDP、导航与输入顺序边界。
- [导航事件](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/navigation-events)：同文档导航没有 NavigationStarting，重叠导航及 HTTP 重定向的 ID 语义。
- [ReleaseChannels](https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2environmentoptions7)：选择稳定 Runtime。
- [进程模型](https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/process-model)：独立用户数据目录和运行时进程组。
- [前台激活](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setforegroundwindow)：展示窗口与前台激活结果分别报告。
