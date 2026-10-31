# 多浏览器性能基线

只运行一个新建、专属的原生宿主，不连接已有 Focus、不更改隔离或协议。
依赖 Windows 11、WebView2 Runtime 138+、Python 3.12（仅标准库）。所有窗口
保持可见；窗口重叠不代表隐藏或最小化。测量期间避免操作测试窗口，否则接管、
失焦或最小化造成的错误会被保留，不会强制交还或自动重试动作。

```powershell
python -m unittest discover -s benchmarks -p test_baseline.py -v
python benchmarks/concurrency.py --exe artifacts/releases/v0.6.2/distinct-browser.exe
```

默认顺序：1、5、10、20 Browser，每组分别 1、3 Page；一个工作进程代表多个
逻辑调用者。创建并发上限 4；就绪后每个 Browser 一个 Python 工作线程，浏览器
内部仍顺序执行，跨浏览器并发。每页先观察、点击计数器、点击输入框、输入一个
字符，20 轮；另有一轮预热。三页场景逐页选择，每次切换后重新观察。
`--create-workers 1` 可改为保守串行准入，批量耗时不能与并行创建配置混比。

本地网页含固定种子的 620×220 彩色噪声画布，使 PNG 大小可复现；它不是实际
网站集合。输入和点击通过页面回报验证，CDP 返回耗时不等于业务完成耗时。
相同 Browser 内的页面必须共享 Cookie / localStorage，不同 Browser 必须隔离。

默认窗口 960×720 DIP，位置为桌面 (20,20)，不抢前台调整尺寸；报告记录实际
DPI、像素尺寸和 CSS 视口。创建完成后先观察空白页以符合租约约束，再导航到
测试页。`create_to_first_fixture_observe` 从开始请求创建，到首张测试页截图被
客户端解析完成；`batch_ready_ms` 还包括所有子页就绪。窗口缓存、Runtime 文件
缓存及 OS 缓存不清空：这是新 Profile、暖系统缓存测量，不能称为冷启动。

## 资源保护与结果

默认保留 max(2 GiB, 物理内存 15%) 的可用物理内存、2 GiB 可用提交量；原生
进程私有提交量上限 8 GiB。低内存机器可显式使用：

```powershell
python benchmarks/concurrency.py --create-workers 1 --reserve-gib 1.5 --reserve-fraction 0
```

这一配置与默认结果必须分开比较。保护触发后停止派发新操作，已经发出的请求
可能已产生副作用；不重放。`outcome_unknown` 表示未获确定回复，不等于宿主
报告失败。关闭本次宿主中的所有浏览器、断开连接、等待原生进程退出，并仅删除
本次 UUID Profile（拒绝重解析路径）；不删除缓存或其他浏览器。异常情况下只
终止自己启动、Popen 句柄所指的宿主；若有残留子进程，报告清理失败并停止矩阵，
不按进程名杀进程。

输出默认在忽略的 `artifacts/performance/<时间>/`：

- `report.json`：机器、EXE SHA / 文件版本 / 可用包源码 SHA、脚本与页面摘要、参数、所有场景。
- `<Nb-Pp>/case.json`：逐请求、逐进程采样、汇总、重连校验和关闭结果。
- `requests.csv`、`resources.csv`：请求耗时 / 错误和各进程组资源随时间变化。
- 诊断模式另有 `native-trace.jsonl`；异常堆栈只写本地 `failure.txt`。

结果每个场景写入一次。退出码 0=完成，77=资源保护（PowerShell 可能映射为 1），
1=失败。后续场景标为 `not_run`，不能用部分创建的数据冒充完整并发量。
中位数和 P95（nearest rank）只统计成功请求，同时保留错误率、样本数。
`old_token_probe` 的 `invalid_token` 是预期安全检查，不属于工作负载错误。
弹窗打开导致的 `page_changed` 也单独记录，并验证新页面实际就绪，绝不重复点击。

进程采样通过 Toolhelp32 枚举，仅跟踪宿主及其 `msedgewebview2.exe` 后代，保留
进程句柄和创建时间以避免 PID 复用。Python 客户端单列。采样间隔默认 0.5 秒，
可能漏掉在两次采样间启动并退出的进程；边界区间 CPU 归入采样时所在阶段。
新发现子进程的累计 CPU 计入发现区间，因此启动阶段短区间可能有尖峰。
`100%` CPU 表示一个逻辑核；`capacity_percent` 再除以逻辑核数量。
`PrivateUsage` 是私有提交量，工作集相加可能重复计入共享页，不能称为独占物理
内存。缺失的内存计数或采样失败不能当作零，资源覆盖字段和错误会保留。
`post_close` 显式采样应显示原生资源归零；Python 仍在生成报告，故客户端不会归零。

重连仅重新认证原宿主、获取真实快照；校验 Browser / Page / HWND / URL / title
（含 document UUID、输入、点击计数）和选中页不变、所有会话人工控制、旧令牌
失效。测试不自动交还，不恢复旧操作；不会把自动化记作物理设备验收。

## 截图链路诊断（独立产物）

```powershell
./scripts/build.ps1 -OutputDirectory build/perf -WindowsSdkRoot 'D:/Windows Kits/10'
python benchmarks/concurrency.py --exe build/perf/distinct-browser.exe --trace --counts 1 5 --pages 1
```

必须把 `instrumented_diagnostic` 与未修改正式 EXE 的 `release_baseline` 分开。
默认程序不写计时文件；`--trace` 仅给测试子进程设置
`DISTINCT_BROWSER_PERF_TRACE`。记录固定阶段名、微秒、字节数，不记录网页内容、
URL、图像、令牌或恢复凭据，不增加 RPC。Trace 为空会标为失败。

阶段为 `layout_metrics`、`capture_png_callback`、`base64_encode`、`image_json_build`、
`response_json_utf8`、`observe_host_until_enqueued`。后者包含前几项，不能相加；
也不能相减不同请求的中位数推导精确残余。`capture_png_callback` 包含异步等待、
浏览器截图/编码与回调调度，不是 PNG 编码器的独占 CPU 时间。
统计包含初始化空白页和预热，不能当作只含工作负载的分段计时。排队后的管道
写入和 Python 解码未单独计时；客户端 RTT 仍包含它们。文件写入有扰动，所以
这只是定位优先项，不能替代正式产物的延迟数据。

实测摘要和可复算的 JSON / CSV 放在 [`results/`](results/)。本机低内存、有其他
程序运行的数据仅代表所记录条件，不能推断独占整台机器时的容量上限。

原生统计语义依据：[GetProcessMemoryInfo](https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getprocessmemoryinfo)、
[PROCESS_MEMORY_COUNTERS_EX](https://learn.microsoft.com/en-us/windows/win32/api/psapi/ns-psapi-process_memory_counters_ex)、
[PROCESSENTRY32W](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/ns-tlhelp32-processentry32w)。
