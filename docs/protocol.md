# JSONL 协议 v1

一条连接对应一个宿主进程。stdin/stdout 必须是重定向的匿名管道；stdin 是 UTF-8 JSON 行，支持 LF 和 CRLF。每行最多 1 MiB，必须以换行结束。EOF 丢弃末尾未完成的一行并撤销所有租约。stdout 只输出 JSON；stderr 用于启动错误诊断。

请求编号是非空字符串，最长 128 个 UTF-8 字节，同一连接的在途请求不得重号。响应可乱序，必须按编号关联；事件与响应可以交错。对重复在途编号输出 `protocol_error` 事件，原请求继续处理，避免两个响应误用同一编号。

```json
{"id":"1","method":"create","params":{"label":"Browser A"}}
{"id":"1","ok":true,"result":{"browser_id":"...","hwnd":"...","browser_process_id":1234,"state":"available"}}
{"id":"2","ok":false,"error":{"code":"busy","message":"...","detail":{}}}
```

无法解析编号的请求使用 `id:null` 返回错误。空行忽略；超长行被丢弃到下一个换行并返回 `invalid_request`，后续行仍然可用。`hwnd` 是十进制字符串；仅用于原生窗口诊断，自动动作使用 `browser_id`。

启动成功先输出：

```json
{"event":"ready","protocol_version":1,"pid":1234}
```

初始化失败写 stderr，返回非零退出码。正常退出码为 0。

## 方法

### 可选的存活宿主重连（v0.6.0）

无参数启动保持本协议的 stdin/stdout 行为。显式使用 `distinct-browser.exe --reconnect` 时，宿主创建本地命名管道和受保护的端点文件；若 stdout 是管道，输出一次引导事件：

```json
{"event":"ready","protocol_version":1,"transport":"named_pipe","pid":1234,"host_id":"...","endpoint":".../endpoint.json"}
```

这条引导事件不表示管理连接已建立。客户端保存端点路径，核对端点文件及目录的当前登录 SID 私有 DACL、协议、宿主 EXE 路径、PID 与进程创建时间，并保持进程句柄。调用方可另外固定 EXE SHA-256；演示客户端支持 `expected_sha256`。打开本地管道后，使用 `GetNamedPipeServerProcessId` 核对服务器进程，不能仅依赖文件中的 PID 或管道名称。

端点中的 `credential_dpapi` 为 CurrentUser DPAPI 保护的随机凭据，不应记录到普通日志。首次管道帧为 `{host_id, protocol_version:1, credential}`，凭据需解保护后传入。握手上限 8 KiB、5 秒，失败直接断开且不提供浏览器信息。管道拒绝远程客户端，DACL 仅允许当前登录 SID 数据读写，不授予客户端创建服务器实例的权限。服务端还检查管道客户端进程的登录 SID。该边界不等于隔离同一登录用户下的恶意代码。

同一时刻只有一个管理连接；第二连接收到 Windows 管道忙碌错误，不抢占现有管理者。认证成功后管理管道输出 `ready`，含稳定的 `host_id`、新 `connection_id` 和递增 `generation`。此后所有响应/事件带 `host_id`、`connection_id`、`revision`。`revision` 为宿主浏览器事件序号；客户端先缓冲事件，再以 `snapshot.revision` 为基线应用更新的事件，避免快照期间的订阅空隙。

重连模式额外支持两个无需租约的方法：

| method | params | result |
| --- | --- | --- |
| `snapshot` | 空对象 | `host_id`、`connection_id`、`revision`、全部存活 `browsers`；每项含 `browser_id`、`label`、`state`、`busy`、`selected_page_id`、`pages`、`site_interactions`、`interrupted_operation` |
| `ping` | 空对象 | 空对象；保活，不授予控制 |

快照中的页面含现有 `list_pages` 字段以及 `visible`、`minimized`。页面集合和元数据在 STA 上读取，不声称与截图、DOM 或网络状态原子一致。`interrupted_operation` 最多保留该浏览器最近一次断连时的操作类型、来源页、派发/完成步数、`result_unknown` 和 `settled`，不含旧令牌和旧请求编号。它不是可重试队列，`settled:true` 也不保证网页业务成功。

客户端每 5 秒发送 `ping` 即可保活；连续 30 秒没有完整输入帧，或 EOF、输出失败、已验证客户端进程退出，均使连接失效。读取到半帧字节不会无限延长这一期限。失效标记先由传输线程设置，UI 下一次派发检查时就拒绝旧连接；已经发出的操作仍按真实结果收尾，不宣称撤回。断连取消下载和未决权限，保留就绪窗口；新连接读取真实终态，不重新请求。

重连后仍是人工控制或接管中，旧租约和旧观察均无效。必须等待旧在途操作收尾、原生工具栏明确交还、新 `acquire`、新 `observe`，才能继续动作。旧代次的输入队列、响应及回调不能完成新代次的同编号请求。宿主存活期间保持原 `browser_id` / `page_id` / HWND，不重新导航或重建页面。宿主退出时端点失效；即使崩溃留下端点文件，客户端也须验证进程身份并报告不可恢复，不自动创建替代浏览器。

跨 Windows 用户 ACL 实机和物理设备仍待独立验收，不属于本仓库自动化测试的通过声明。上层应用应单独验证其安装更新与重连生命周期。

| method | params | result / 行为 |
| --- | --- | --- |
| `create` | `label` | 在窗口、独立用户数据环境、Controller 和初始页面都可用后返回 `browser_id`、`page_id`、`hwnd`、`browser_process_id`、`state`；最长初始化等待 45 秒 |
| `present` | `browser_id`, 可选 `page_id` | 展示或恢复原窗口；指定页面时选择并展示该页，省略时展示当前页。返回 `visible`、`foreground`、`state`。`foreground:false` 表示 Windows 没有准许前台激活，页面仍然存在 |
| `acquire` | `browser_id`, `client_id`, 可选 `ttl_ms` | 返回新 `token`、`ttl_ms`。同一调用者再次申请也会收到 `busy`，续期应使用 `renew` |
| `renew` | `browser_id`, `token`, 可选 `ttl_ms` | 从处理请求的时刻重新计算租约；返回 `ttl_ms` |
| `observe` | `browser_id`, `token` | 返回 URL、标题、CSS 视口、PNG 和控制状态；完成后允许该租约执行动作 |
| `act` | `browser_id`, `token`, `action` | 执行一个动作。每个浏览器最多一个在途的 `observe` 或 `act` |
| `release` | `browser_id`, `token` | 立即撤销租约，返回空对象；已发操作仍收尾，其收尾前新调用者申请收到 `busy` |
| `close` | `browser_id` | 撤销控制、关闭原生窗口和 WebView2、使标识永久失效；返回空对象 |
| `list_pages` | `browser_id` | 返回 `selected_page_id` 和 `pages` 列表；不需要租约，每项含 `page_id`、`opener_page_id`、`url`、`title`、`hwnd`、`ready`、`loading` |
| `select_page` | `browser_id`, `token`, `page_id` | 选择并展示已有页面，返回 `page_id`、`foreground`；清除旧观察，不重新加载 |
| `close_page` | `browser_id`, `token`, `page_id` | 关闭一个页面；关闭当前页时选择最近使用的存活页；关闭最后一页使浏览器标识失效 |
| `list_downloads` | `browser_id` | 只读查询，无需租约；返回 `downloads` 列表和浏览器级 `interaction_pending` |

`label` 最多 256 字节，`client_id` 最多 128 字节，均为非空字符串且不能含 NUL。`token` 为不透明的持有者凭据，每次取得控制都会生成新令牌。租约默认 30,000 ms，允许 1,000–300,000 ms；宿主使用单调时钟，并在实际派发前检查到期。

`present` 和 `close` 是连接级管理操作，不需要控制令牌。前台调用程序可针对 `ready.pid` 调用 Windows 的 `AllowSetForegroundWindow`，然后再调用 `present`。前台结果由 Windows 决定。

### 多页面（v0.4）

每个 `browser_id` 是一个隔离会话，包含一个 Environment / 默认 Profile、多个真实页面及一份控制租约。每页对应一个原生顶层窗口和不可复用的 `page_id`。`create.hwnd` 对应初始页面，不代表以后所有页面；`present` 展示当前选中页，`close` 关闭全部页面。

`observe` 和 `act` 可附加 `page_id`。省略时作用于当前页；显式指定的页面必须属于该浏览器且正被选中，否则返回 `page_not_found` 或 `page_not_selected`，不会改投其他页。观察结果包含截图对应的 `page_id`。选择其他页、新页面成为当前页、关闭当前页，均使旧观察失效；重新观察前动作返回 `observation_required`。观察期间切页返回 `page_changed`。

`select_page`、`close_page` 检查浏览器级令牌和唯一在途操作槽，不要求事先观察。已有 `observe` / `act` 在途时返回 `busy`，不能使用另一个页面绕过互斥。用户通过原生窗口切换页面不自动撤销租约，但仍会使旧观察失效。任何页面的人工输入或接管按钮都会撤销整个浏览器的租约；交还后需要重新申请并观察。

网页 `window.open` / `target=_blank` 创建的新页使用来源 Environment 和 Profile，通过 WebView2 的关联新窗口接口接入，保留 Cookie、localStorage 及原本的 opener / noopener 语义。原页面保留；空白弹窗也可观察。命名窗口由 WebView2 复用。每浏览器最多 16 页（包括初始化中的页面），超出或初始化失败发出 `page_creation_failed`，不生成未受管理窗口。

新页初始化最长 45 秒。失败、来源页关闭或整个浏览器关闭时完成等待中的 Deferral，并关闭未完成的页。初始化已完成的子页可以在来源页关闭后继续存在。网页调用 `window.close()` 只关闭该页。某页发生 WebView2 进程故障时，保守地将整个浏览器会话标为 `faulted`。

最小化或隐藏任何页面会暂停该浏览器的自动操作；所有剩余页面恢复可见后才能交还。可通过不需要租约的 `present(browser_id, page_id)` 逐一恢复后台暂停页。展示另一页同样清除旧观察，不授予或撤销租约。遮挡或切换其他应用不暂停。断连保留所有已初始化页面供人工查看，最后一个窗口关闭才退出宿主。

### 观察

v0.5 的观察结果增加 `site_interactions`，包含当前页的 `downloads`、`permissions` 记录，以及浏览器级 `interaction_pending`。这些是查询时的状态，不保证与截图原子一致。旧客户端可忽略该字段；协议版本仍为 1。

```json
{"id":"3","method":"observe","params":{"browser_id":"...","token":"..."}}
```

返回结构：

```json
{"page_id":"...","url":"https://example.org/","title":"Example","viewport":{"width":900,"height":600,"coordinate_space":"css_viewport"},"image":{"mime_type":"image/png","base64":"...","width":1350,"height":900},"state":"automatic"}
```

`image` 是网页截图，不含原生控制栏。坐标使用网页视口 CSS 像素；截图位置换算为点击坐标时，横向乘 `viewport.width / image.width`，纵向乘 `viewport.height / image.height`。PNG 上限 24 MiB。

新租约必须成功观察后再操作。导航、URL 变化、窗口大小变化会使已有观察失效；需要再次观察。观察过程中导航或改变视口会返回 `page_changed`；导航还在进行时返回 `page_loading`。网页脚本仍可能改变内容，调用者需要按实际情况频繁观察。

### 动作

| action | 字段 | 语义 |
| --- | --- | --- |
| `navigate` | `url` | 支持 `http:`、`https:`、`about:`、`file:`，最多 8192 字节。跨文档导航等匹配的 `NavigationCompleted`；同文档导航以目标 URL 匹配的 `SourceChanged` / `HistoryChanged` 收尾。成功返回 `navigation_completed:true`、`same_document` |
| `click` | `x`, `y` | CSS 视口内的左键按下、抬起，分两次 CDP 调用顺序完成 |
| `scroll` | `x`, `y`, `delta_y`, 可选 `delta_x` | 发出滚轮事件；位移每轴范围 -10,000–10,000，横向默认 0 |
| `text` | `text` | 向当前网页焦点插入非空文字，最多 65,536 字节；调用者应先点击输入位置 |
| `key` | `key` | 依次发送按下、抬起；支持下表的命名按键 |

按键支持 `Enter`、`Tab`、`Escape`、`Backspace`、`Delete`、`ArrowLeft`、`ArrowRight`、`ArrowUp`、`ArrowDown`、`Home`、`End`、`PageUp`、`PageDown`、`Space`。普通文字使用 `text`。

```json
{"id":"4","method":"act","params":{"browser_id":"...","token":"...","action":{"type":"click","x":80,"y":100}}}
```

输入动作成功返回 `command_processed:true`、`business_success_verified:false` 和 `control_still_valid`。这表示 CDP 回调已完成，调用者还需观察网页业务结果。即使最后一条已发命令成功，操作收尾时租约也可能已经失效。

同文档跳转（包括重复跳到当前锚点）也会清除旧观察资格，下一步动作前需要再次 `observe`。HTTP 重定向沿用原导航 ID，仍等待整个导航完成。后续不同 ID 的文档导航取代在途请求时，原请求返回 `navigation_superseded`，替代导航继续运行；其旧完成事件不会清除替代页面的加载状态。

正常取消或加载失败不会把浏览器置为 `faulted`，租约仍然可用。WebView2 报告 `OPERATION_CANCELED` 时返回 `navigation_cancelled`，其他失败返回 `navigation_failed`，详情中的 `web_error_status` 保留 Runtime 原始错误值。例如页面执行 `window.stop()` 在当前验证的 Runtime 上返回 `CONNECTION_ABORTED`（9），因此按加载失败结算；不把这个值伪装成 `OPERATION_CANCELED`（14）。真正没有结束信号的在途操作仍保留 20 秒故障超时。

对于多步动作，接管发生后不再派发剩余步骤，因此可能只有按下已发生；宿主不会声称已经撤回这些效果。失败详情含 `dispatched_steps`、`completed_steps`、`result_unknown`；已经派发过的操作不能据此假定没有产生效果。

## 控制状态与事件

### 下载与网站权限（v0.5）

下载开始或支持的网站权限请求会撤销所属浏览器的租约，`control_changed.detail.reason` 分别为 `download_started`、`permission_requested`。在途动作如实收尾，未派发步骤停止。下载 / 权限仍待处理时，“交还”不会开放租约；结束后仍保持人工控制，必须显式交还、取得新令牌并重新观察。宿主不会自动恢复点击、导航或下载。

`download_changed.detail` 包含 `download_id`、原始 `page_id`、`url`、绝对保存 `path`、`state`、`reason`、`bytes_received`、`total_bytes`（未知为 -1）。状态为 `in_progress`、`completed`、`cancelled`、`interrupted`；进度事件最多每 250 ms 一次，终态立即发出。`list_downloads` 可在人工控制时读取相同记录。下载面板提供取消按钮，`Ctrl+J` 重新打开；没有执行文件或自动重试接口。每浏览器最多 4 笔在途下载，最近 32 条记录；同一页同一 URL 的在途重复请求被拒绝。`download_rejected` 提供 `reason`，重复时也包含现有 `download_id`。

文件保存到系统 Downloads 下的 `Distinct Browser/<browser_id>/<download_id>/download-<清理后的文件名>`，不覆盖既有下载目录。连续 30 秒没有进展中断并报告 `no_progress_timeout`；用户取消报告 `human_cancel`，Runtime 中断保留数字原因。关闭来源页、断连或故障会取消该页下载；关闭下载面板仅隐藏面板。文件传输是独立于导航的结果：导航可能先返回 `navigation_failed`（Runtime `CONNECTION_ABORTED`，9），随后产生下载事件；若下载事件先到，导航可返回 `download_started:true`、`download_id`、`page_id`、`control_still_valid:false`。两种情况均应查询下载，不重放导航。

`permission_requested`、`permission_resolved` 的 `detail` 包含 `request_id`、`page_id`、来源 `url`、`kind`、`state`、`reason`。状态为 `pending`、`allowed_once`、`denied`、`failed`。原生提示允许用户决定本次请求；不写入 Profile，不存在 RPC 授权方法。未知类型或不支持单次授权的 Runtime 拒绝；每浏览器最多 8 个待决请求，120 秒超时、来源导航 / 关闭、断连和故障均拒绝并只完成一次 Deferral。观察中最多保留最近 16 条权限记录。

| state | 含义 |
| --- | --- |
| `available` | 可以申请租约；若旧操作仍在途，需要先等它收尾 |
| `automatic` | 租约生效 |
| `taking_over` | 用户已接管并撤销令牌，等待已发操作回调 |
| `manual` | 人工控制，点击“交还”才可再次申请 |
| `paused` | 用户最小化或隐藏窗口，令牌已撤销；恢复窗口并点击“交还”才可再次申请 |
| `faulted` | 故障或操作超过 20 秒，令牌失效；关闭此浏览器后才能创建新的浏览器 |
| `closed` | 窗口关闭，标识失效 |

“暂停”只阻止新的自动命令，不冻结页面脚本、请求或服务器操作。被其他窗口遮挡不暂停。`present` 不重新加载页面，也不自动清除 `paused`。

浏览器事件使用以下结构：

```json
{"event":"control_changed","browser_id":"...","state":"manual","detail":{"reason":"takeover"}}
```

事件包括 `created`、`control_changed`、`paused`、`closed`、`faulted`、`navigation_completed`，以及 `page_created`、`page_selected`、`page_closed`、`page_creation_failed`。页面事件在 `detail.page_id` 标明页面，创建事件另有 `opener_page_id`，选择和关闭事件另有 `reason`。创建失败包含 `reason` 和 `opener_page_id`，只在实际分配了页面标识时包含 `page_id`。导航事件附加 `detail.page_id`。旧的 `popup_blocked` 已由受管理页面事件替代。未规定的字段和未识别事件应忽略。通用 `protocol_error` 没有浏览器标识。

v0.3 的网页原生输入接管沿用 `control_changed`，首次输入的 `detail.reason` 为 `human_input`。没有在途操作时状态为 `manual`，否则先发 `taking_over`，已派发操作收尾后转 `manual`。悬停、展示和 CDP 输入不产生该原因。重复人工输入不重复撤销；用户交还后仍需新租约和新观察。输入并不撤回已经派发的命令或保证其页面副作用取消。

EOF、stdout 写失败或输出队列超过 64 MiB 都视为断连：统一撤销自动控制，保留已完成初始化的窗口，供用户人工操作；初始化中的窗口关闭。断连后不重新连接；最后一个窗口关闭后退出进程。正常连接期间，即使所有窗口已关闭，宿主仍可继续处理创建请求。

## 常见错误码

`invalid_request`、`unknown_method`、`runtime_missing`、`creation_failed`、`browser_not_found`、`browser_not_ready`、`busy`、`control_unavailable`、`window_paused`、`invalid_token`、`observation_required`、`page_loading`、`page_changed`、`invalid_action`、`navigation_failed`、`navigation_cancelled`、`navigation_superseded`、`observation_failed`、`capture_failed`、`action_failed`、`browser_faulted`、`browser_closed`。

多页面另有 `page_not_found`、`page_not_selected`、`page_not_ready`、`page_closed`。关闭正在操作的子页返回 `page_closed`，详情仍说明已派发步骤和未知副作用；不会把剩余动作投递到其他页面。

失败不自动重试，尤其不要盲目重发带 `result_unknown:true` 的动作。`busy` 也不代表动作已入队；收到错误的调用者需要根据最新控制状态重新决定下一步。
