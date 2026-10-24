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

| method | params | result / 行为 |
| --- | --- | --- |
| `create` | `label` | 在窗口、独立用户数据环境、Controller 和初始页面都可用后返回 `browser_id`、`hwnd`、`browser_process_id`、`state`；最长初始化等待 45 秒 |
| `present` | `browser_id` | 展示或恢复原窗口；返回 `visible`、`foreground`、`state`。`foreground:false` 表示 Windows 没有准许前台激活，页面仍然存在 |
| `acquire` | `browser_id`, `client_id`, 可选 `ttl_ms` | 返回新 `token`、`ttl_ms`。同一调用者再次申请也会收到 `busy`，续期应使用 `renew` |
| `renew` | `browser_id`, `token`, 可选 `ttl_ms` | 从处理请求的时刻重新计算租约；返回 `ttl_ms` |
| `observe` | `browser_id`, `token` | 返回 URL、标题、CSS 视口、PNG 和控制状态；完成后允许该租约执行动作 |
| `act` | `browser_id`, `token`, `action` | 执行一个动作。每个浏览器最多一个在途的 `observe` 或 `act` |
| `release` | `browser_id`, `token` | 立即撤销租约，返回空对象；已发操作仍收尾，其收尾前新调用者申请收到 `busy` |
| `close` | `browser_id` | 撤销控制、关闭原生窗口和 WebView2、使标识永久失效；返回空对象 |

`label` 最多 256 字节，`client_id` 最多 128 字节，均为非空字符串且不能含 NUL。`token` 为不透明的持有者凭据，每次取得控制都会生成新令牌。租约默认 30,000 ms，允许 1,000–300,000 ms；宿主使用单调时钟，并在实际派发前检查到期。

`present` 和 `close` 是连接级管理操作，不需要控制令牌。前台调用程序可针对 `ready.pid` 调用 Windows 的 `AllowSetForegroundWindow`，然后再调用 `present`。前台结果由 Windows 决定。

### 观察

```json
{"id":"3","method":"observe","params":{"browser_id":"...","token":"..."}}
```

返回结构：

```json
{"url":"https://example.org/","title":"Example","viewport":{"width":900,"height":600,"coordinate_space":"css_viewport"},"image":{"mime_type":"image/png","base64":"...","width":1350,"height":900},"state":"automatic"}
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

事件包括 `created`、`control_changed`、`paused`、`closed`、`faulted`、`navigation_completed`、`popup_blocked`。`detail` 可含原因、导航成功标志或被阻止的 URL；未规定的字段应忽略。通用 `protocol_error` 没有浏览器标识。

v0.3 的网页原生输入接管沿用 `control_changed`，首次输入的 `detail.reason` 为 `human_input`。没有在途操作时状态为 `manual`，否则先发 `taking_over`，已派发操作收尾后转 `manual`。悬停、展示和 CDP 输入不产生该原因。重复人工输入不重复撤销；用户交还后仍需新租约和新观察。输入并不撤回已经派发的命令或保证其页面副作用取消。

EOF、stdout 写失败或输出队列超过 64 MiB 都视为断连：统一撤销自动控制，保留已完成初始化的窗口，供用户人工操作；初始化中的窗口关闭。断连后不重新连接；最后一个窗口关闭后退出进程。正常连接期间，即使所有窗口已关闭，宿主仍可继续处理创建请求。

## 常见错误码

`invalid_request`、`unknown_method`、`runtime_missing`、`creation_failed`、`browser_not_found`、`browser_not_ready`、`busy`、`control_unavailable`、`window_paused`、`invalid_token`、`observation_required`、`page_loading`、`page_changed`、`invalid_action`、`navigation_failed`、`navigation_cancelled`、`navigation_superseded`、`observation_failed`、`capture_failed`、`action_failed`、`browser_faulted`、`browser_closed`。

失败不自动重试，尤其不要盲目重发带 `result_unknown:true` 的动作。`busy` 也不代表动作已入队；收到错误的调用者需要根据最新控制状态重新决定下一步。
