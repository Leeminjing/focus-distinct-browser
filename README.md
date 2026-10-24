# Distinct Browser

一个 Windows 11 x64 原生浏览器宿主：C++17、Win32 窗口、系统 WebView2 Runtime。每个浏览器有真实窗口、独立数据目录和控制权状态。一个进程可以代表多个逻辑调用者，通过重定向的 stdin/stdout 发送 UTF-8 JSON 行。

交付文件是 `build/distinct-browser.exe`。静态链接 WebView2 Loader 和 MSVC CRT，无需旁带 Loader DLL、VC++ Redistributable 或 .NET；机器仍须安装 WebView2 Evergreen Runtime。SDK 是构建依赖，Python 只用于演示和验收。

## 运行演示

```powershell
python demo/demo.py
```

演示在本地启动测试页面，打开 A、B 两个窗口，并行输入和点击；第二个调用者随后接管 A 的同一页面。窗口保持打开，可以体验原生“接管 / 交还”、地址栏导航、最小化和恢复。终端按 Enter 关闭演示窗口。

EXE 通过管道启动，不分配控制台。双击 EXE 不会启动演示；其他语言的调用方可以直接启动 EXE 并连接管道，不需要 Python。

## 构建

需要 MSVC x64 C++ 工具和 Windows SDK（含 C++/WinRT）。在 x64 Native Tools 命令环境启动 PowerShell 后运行：

```powershell
./scripts/build.ps1
```

脚本也会寻找 Visual Studio 或 `.tools/msvc/VC/Tools/MSVC` 中的编译器；可以指定 `-MsvcRoot`、`-WindowsSdkRoot`。首次构建下载固定的 WebView2 SDK `1.0.3650.58` 到 `.deps`，校验 SHA-256 后解压，不安装系统组件。

也可在已配置的 MSVC 开发环境中使用 CMake（先用上述脚本取得 SDK）：

```powershell
cmake -S . -B build/cmake -A x64
cmake --build build/cmake --config Release
ctest --test-dir build/cmake -C Release --output-on-failure
```

## 验收

```powershell
./scripts/build.ps1
python tests/integration.py
python tests/protocol.py
python tests/failures.py
```

编译后也可用 `./scripts/test.ps1` 一次运行全部检查。

构建脚本运行控制权状态机测试；其余测试直接启动真实 EXE 和 WebView2，不使用浏览器模拟对象：

- 双窗口并行、独立 Cookie / localStorage、真实 DOM 输入与点击、系统鼠标位置保持。
- 截图及 CSS 视口、展示原页面、共享控制、重新观察、原生按钮接管、已发导航收尾。
- 最小化 / 隐藏、恢复后交还、租约到期与续期、关闭后标识失效、断连保留人工窗口。
- 弹窗阻止、畸形 JSON、分片 UTF-8、行大小限制和 EOF。
- Runtime 缺失、用户数据目录被配置覆盖、测试专属 WebView2 进程故障、操作超时。

`failures.py` 的超时检查约需 20 秒，并会终止它自己创建的 WebView2 测试进程。截图和验收摘要写入 `artifacts/`。

## 使用边界

每个窗口只有一个页面；额外弹窗会被阻止并报告。自动操作仅提供导航、坐标点击、文字、有限命名按键和滚动，不开放任意脚本或通用 CDP 调用。首次真人输入不会自动接管，使用工具栏按钮交接。

同一浏览器只接受一个在途操作；并发请求收到 `busy`，不积压动作，也不自动重试。不同浏览器可以各自等待异步结果。操作已处理不代表网页业务成功，调用者需要观察结果。

截图是网页区域的当前画面，PNG 像素与 CSS 像素可能不同。页面脚本可以继续变化；截图和页面元数据也不是对动态页面的原子快照。

用户数据位于 `%LOCALAPPDATA%/DistinctBrowser/Profiles/<browser_id>/`。关闭不会自动删除这些目录，当前版本不支持进程重启后恢复原标识；如需清理，应在相关宿主和 WebView2 进程退出后处理。

完整接口见 [协议](docs/protocol.md)，实现边界见 [设计](docs/design.md)。
