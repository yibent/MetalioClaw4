# Lua Agent Protocol (LAP) v1

设备端 App「远程脚本」打开后，会与后端建立 WebSocket，上报设备信息，然后等待服务端下发 Lua 脚本。设备用本地 `lua_runtime` 执行脚本，调用全局函数 `main`（可配置），把返回值以 JSON 发回。

本文是后端实现说明。设备端已按此协议落地。

## 1. 连接

### URL

默认：把 `api_endpoints.h` 里的 HTTP Host 换成 `ws`/`wss`，路径为 `/api/device-ws/v1`。

当前 Host 为 `https://max.sh.creativone.cn`，设备连接：

```text
wss://max.sh.creativone.cn/api/device-ws/v1
```

这与 CubeMax 设备网关路径一致。

覆盖优先级（高 → 低）：

1. NVS 命名空间 `lua_agent`，键 `url`（完整 `ws://` 或 `wss://` URL）
2. Kconfig `CONFIG_LUA_AGENT_WS_URL`（非空时）
3. 上面的默认推导

可选鉴权：NVS `lua_agent` / `token`。若 token 不含空格，设备会自动加上 `Bearer ` 前缀。

### Handshake 请求头

| Header | 值 |
|---|---|
| `Protocol-Version` | `1` |
| `Device-Id` | 设备 MAC，如 `aa:bb:cc:dd:ee:ff` |
| `Client-Id` | 设备 UUID |
| `Authorization` | 可选，`Bearer <token>` |

只使用 **文本帧**。二进制帧会被设备忽略。单帧上限约 **80KiB**（Lua 源码上限 64KiB + JSON 包装）。

### 生命周期

1. App 打开 → 连接 WebSocket
2. 连接成功后设备立刻发送 `hello`，不必等服务端先说话
3. 服务端可回 `hello_ok`（可选）
4. 服务端随时发送 `run`
5. 设备执行完回 `result`
6. 任一方可 `ping` / `pong`
7. 服务端可用 `cancel` 取消当前任务
8. App 关闭或断线时设备会停掉正在跑的 Lua 任务并断开

设备在 App 打开期间会自动重连：1s、2s、4s…上限 15s。重连后重新发 `hello`。

同一时刻只跑 **一个** Lua 任务。再来 `run` 会立刻 `result`，`status=rejected`，`error.code=busy`。

## 2. 消息信封

每条消息都是一个 UTF-8 JSON 对象：

```json
{
  "v": 1,
  "type": "hello | hello_ok | run | result | cancel | ping | pong | error",
  "id": "字符串，请求/响应用同一 id"
}
```

- `v` 必须为 `1`
- `run` / `cancel` / `result` 的 `id` 由**服务端**生成，设备原样回传
- `hello` / `ping` 的 `id` 由设备生成

## 3. 消息定义

### 3.1 `hello`（设备 → 服务端）

连接成功后立刻发送。

```json
{
  "v": 1,
  "type": "hello",
  "id": "h-1a2b3c4d",
  "protocol": "lua-agent",
  "ts_ms": 123456789,
  "device": {
    "uuid": "...",
    "mac": "aa:bb:cc:dd:ee:ff",
    "board": "metalio-claw-4",
    "chip": "esp32p4",
    "firmware": "1.2.3",
    "idf": "v5.5.2",
    "language": "zh-CN",
    "flash_size": 16777216,
    "heap_free": 123456,
    "cores": 2,
    "battery": { "level": 80, "charging": false },
    "lua": {
      "max_code_bytes": 65536,
      "max_output_bytes": 4096,
      "max_result_bytes": 8192,
      "capabilities": ["lua", "ui", "audio", "http", "speech", "device", "camera"]
    }
  },
  "data": {
    "protocol": "lua-agent",
    "device_id": "<board uuid>",
    "boot_id": "<boot uuid>",
    "firmware_version": "1.2.3",
    "lua_runtime": "claw4",
    "capabilities": ["lua", "ui", "audio", "http", "speech", "device", "camera"],
    "limits": {
      "max_script_bytes": 65536,
      "max_params_bytes": 16384,
      "max_chunk_bytes": 65536,
      "max_message_bytes": 81920,
      "max_log_bytes": 1024
    },
    "runtime": {
      "execution_model": "main_once",
      "api_version": "claw4.v1",
      "transfer_storage": "ram",
      "max_run_timeout_ms": 60000
    }
  },
  "system": { }
}
```

`system` 是设备已有的整机信息 JSON（分区、芯片、显示等），便于调试。CubeMax 用 `data.device_id`（Board UUID）登记设备。`protocol: "lua-agent"` 用来让 CubeMax 走 LAP，而不是旧的分片协议。

当前固件会上报的 Lua 能力：`lua`、`ui`、`audio`、`http`、`speech`、`device`，有摄像头时还有 `camera`。`uart` 运行时存在，但本板未注册 UART 口，不要默认下发需要串口的脚本。`ui` / `audio` / `speech` / `device` 始终可用，不必在 `run.capabilities` 里声明；`http` 默认打开；`camera` 必须在对应 `run` 里声明。

### 3.2 `hello_ok`（服务端 → 设备，可选）

```json
{
  "v": 1,
  "type": "hello_ok",
  "id": "h-1a2b3c4d",
  "session": "optional-server-session-id"
}
```

设备不阻塞等待这条消息。没收到也可以直接 `run`。

### 3.3 `run`（服务端 → 设备）

```json
{
  "v": 1,
  "type": "run",
  "id": "req-123",
  "script": "function main(args)\n  return { sum = args.a + args.b }\nend\n",
  "entry": "main",
  "args": { "a": 1, "b": 2 },
  "timeout_ms": 15000,
  "capabilities": ["http"]
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 此次任务 id，会出现在对应 `result` 里 |
| `script` | 是 | 完整 Lua 源码，UTF-8，最大 **64KiB** |
| `entry` | 否 | 入口函数名，默认 `"main"` |
| `args` | 否 | 任意 JSON。会变成 Lua 全局 `args`，并作为 `main` 的第一个参数 |
| `timeout_ms` | 否 | 超时。省略 = 30000。`0` = 不超时。非 0 时上限 600000（10 分钟） |
| `capabilities` | 否 | 字符串数组。省略时默认 `http` + `log`。允许值：`http`、`uart`、`log`、`camera`。`ui` / `audio` / `speech` / `device` 始终可用，不必声明 |

执行顺序：

1. 加载并运行 chunk（顶层代码，用来定义函数）
2. 取全局 `entry`（默认 `main`），必须是 function
3. 调用 `main(args)`
4. 把返回值编码成 JSON，放进 `result.value`

Lua 约定：

```lua
function main(args)
  -- args 是 JSON 解码后的 table（或你传的其它 JSON 值）
  return { ok = true, n = args.n }
end
```

返回值编码：

- 0 个返回值 → `value: null`
- 1 个返回值 → 该值本身
- 多个返回值 → JSON 数组
- `nil` → `null`
- 数字 / 布尔 / 字符串 → 对应 JSON
- table：若是密集的 `1..n` 数组则编成 JSON 数组，否则编成对象（非字符串 key 会转成字符串）
- function / userdata / thread → `"<function>"` 这类字符串
- 结果 JSON 超过 8KiB 时 `value` 为 `null`，且 `value_truncated: true`

`print(...)` 会进入 `result.output`（最多 4KiB）。

### 3.4 `result`（设备 → 服务端）

成功：

```json
{
  "v": 1,
  "type": "result",
  "id": "req-123",
  "ok": true,
  "status": "done",
  "value": { "sum": 3 },
  "value_truncated": false,
  "output": "",
  "output_truncated": false,
  "duration_ms": 12
}
```

失败：

```json
{
  "v": 1,
  "type": "result",
  "id": "req-123",
  "ok": false,
  "status": "failed",
  "error": {
    "code": "lua_error",
    "message": "ERROR: stdin:1: attempt to index a nil value\n"
  },
  "value": null,
  "value_truncated": false,
  "output": "ERROR: ...",
  "output_truncated": false,
  "duration_ms": 8
}
```

`status`：

| status | 含义 |
|---|---|
| `done` | `main` 正常返回 |
| `failed` | Lua 运行时错误，或没有入口函数 |
| `timeout` | 超过 `timeout_ms` |
| `cancelled` | 被 `cancel` 或 App 退出打断 |
| `rejected` | 尚未开跑就被拒绝（busy / 参数非法 / 脚本过大） |

`error.code`：

| code | 何时 |
|---|---|
| `busy` | 已有任务在跑，或运行时任务槽满 |
| `invalid` | 缺字段、JSON 坏了、未知 `type` |
| `too_large` | 脚本 > 64KiB，或 `args` > 16KiB |
| `no_entry` | 找不到 `entry` 函数 |
| `lua_error` | 脚本 / `main` 抛错 |
| `timeout` | 超时 |
| `cancelled` | 取消 |
| `not_found` | `cancel` 的 id 对不上当前任务 |

### 3.5 `cancel`（服务端 → 设备）

```json
{ "v": 1, "type": "cancel", "id": "req-123" }
```

`id` 必须是正在跑的那条 `run`。设备会协同取消 Lua 任务，随后回 `result`（`status=cancelled`）。对不上则回 `error` / `not_found`。

### 3.6 `ping` / `pong`

任一方：

```json
{ "v": 1, "type": "ping", "id": "p-1", "ts_ms": 123 }
```

对方原样带回 `id` 和 `ts_ms`：

```json
{ "v": 1, "type": "pong", "id": "p-1", "ts_ms": 123 }
```

设备每 20s 发一次 JSON `ping`，并顺带发 WebSocket 控制帧 ping。若 60s 收不到任何文本消息，设备会断开重连。

### 3.7 `error`（设备 → 服务端）

协议级错误，不是某次 `run` 的业务失败：

```json
{
  "v": 1,
  "type": "error",
  "id": "req-123",
  "error": { "code": "invalid", "message": "unknown type" }
}
```

## 4. 时序

```text
Device                               Server
   |-- TCP+WS handshake ---------------->|
   |-- hello --------------------------->|
   |<------------ hello_ok (optional) ---|
   |<------------ run (id=req-1) --------|
   |   (load script, call main(args))    |
   |-- result (id=req-1, ok=true) ------>|
   |<------------ run (id=req-2) --------|
   |-- result (id=req-2) --------------->|
   |-- ping ---------------------------->|
   |<------------ pong ------------------|
```

## 5. 脚本示例

加法：

```lua
function main(args)
  return { sum = (args.a or 0) + (args.b or 0) }
end
```

对应 `run`：

```json
{
  "v": 1,
  "type": "run",
  "id": "add-1",
  "script": "function main(args)\n  return { sum = (args.a or 0) + (args.b or 0) }\nend\n",
  "args": { "a": 20, "b": 22 }
}
```

设备 `result.value`：

```json
{ "sum": 42 }
```

带 HTTP 的脚本必须在 `run` 里打开 `http` 能力（或省略 `capabilities`，默认已包含 http）：

```lua
local http = require("http")
local runtime = require("runtime")

function main(args)
  local res, err = http.get(args.url)
  if not res then
    return { ok = false, error = tostring(err) }
  end
  return {
    ok = true,
    status = res.status,
    n = #res.body,
    now_ms = runtime.now_ms(),
  }
end
```

脚本也可以建 UI（`require("ui")`）。任务结束或取消后，界面会回到「远程脚本」状态页。长时间 UI 循环请自己查 `runtime.cancelled()`，并考虑把 `timeout_ms` 设为 `0`。

CubeMax「编程 / 应用 / 智能交互」节点会下发下面这些脚本 API（Claw4 实现为准）：

```lua
local camera = require("camera")
local text, err = camera.explain("图里有什么")

local speech = require("speech")
speech.say("计时结束")

local device = require("device")
device.set_brightness(80)  -- 0-100，背光
device.set_volume(70)      -- 0-100
device.vibrate(300)        -- 毫秒，GPIO22 PWM 马达
device.notify("ok")        -- 屏幕短通知
```

## 6. 设备端限制

| 项 | 值 |
|---|---|
| Lua 源码 | ≤ 64KiB |
| `print` 捕获 | ≤ 4KiB |
| `main` 返回值 JSON | ≤ 8KiB |
| `args` JSON | ≤ 16KiB |
| 并发任务 | 1 |
| 默认超时 | 30s |
| 接收缓冲区 | 80KiB |

## 7. 最小后端（Python）

依赖：`pip install websockets`

```python
import asyncio, json, websockets

SCRIPT = r"""
function main(args)
  return { hello = args.name, n = args.n + 1 }
end
"""

async def handler(ws):
    hello = json.loads(await ws.recv())
    assert hello["type"] == "hello"
    print("device", hello["device"]["mac"], hello["device"]["uuid"])
    await ws.send(json.dumps({
        "v": 1, "type": "hello_ok", "id": hello["id"], "session": "demo"
    }))
    await ws.send(json.dumps({
        "v": 1,
        "type": "run",
        "id": "req-1",
        "script": SCRIPT,
        "args": {"name": "claw", "n": 41},
        "timeout_ms": 10000,
    }))
    while True:
        msg = json.loads(await ws.recv())
        print("recv", msg["type"], msg)
        if msg["type"] == "ping":
            await ws.send(json.dumps({
                "v": 1, "type": "pong",
                "id": msg.get("id", ""),
                "ts_ms": msg.get("ts_ms"),
            }))
        elif msg["type"] == "result":
            break

async def main():
    async with websockets.serve(handler, "0.0.0.0", 8080, max_size=80 * 1024):
        print("ws://0.0.0.0:8080  —  set NVS lua_agent/url to ws://<pc-ip>:8080")
        await asyncio.Future()

asyncio.run(main())
```

本地调试时在设备 NVS 写入：

- 命名空间：`lua_agent`
- 键：`url`
- 值：`ws://<电脑局域网 IP>:8080`

然后打开桌面上的「远程脚本」。

## 8. 建议的服务端职责

1. 接受 WS，校验 `Device-Id` / `Client-Id`（以及 token）
2. 记录 `hello.device`，按 uuid/mac 路由任务
3. 为每次 `run` 生成唯一 `id`，保存直到收到对应 `result`
4. 下发的脚本必须定义 `function main(args)`
5. 一次只给一台设备发一个 `run`；收到 `busy` 就等上一个 `result`
6. 对 `ping` 回 `pong`；不要依赖设备永不掉线
7. 需要中止时发 `cancel`，`id` 与那次 `run` 相同
