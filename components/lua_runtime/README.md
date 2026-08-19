# Lua Runtime Component

`lua_runtime` is a standalone ESP-IDF component. It owns Lua VM creation,
background jobs, cancellation, timeouts, output capture, and the built-in UI
bridge. Applications share this component; they do not own or share a raw
`lua_State`.

## C API

```c
lua_runtime_job_config_t config = {
    .name = "demo",
    .path = "/sdcard/touch_demo.lua",
    .args_json = "{\"title\":\"Demo\"}",
    .timeout_ms = 0,
    .capabilities = 0,
};

lua_runtime_job_id_t job_id;
ESP_ERROR_CHECK(lua_runtime_start(&config, &job_id));
```

Use `lua_runtime_get_job()` to read state and captured output, and
`lua_runtime_stop()` to cooperatively cancel a job. Every job has an isolated
Lua VM and FreeRTOS task.

Custom native modules can be registered once with
`lua_runtime_register_module()` before jobs are started.

Hardware capabilities are explicit per job. `LUA_RUNTIME_CAP_UART` enables the
UART module, but a board must also register each allowed UART port with
`lua_runtime_register_uart_port()`. Registration fixes the port's TX/RX pins and
maximum baud rate; Lua cannot remap pins.

Board initialization registers an explicitly wired port before starting jobs:

```c
lua_runtime_uart_port_config_t uart_port = {
    .port = 3,
    .tx_pin = 30,
    .rx_pin = 31,
    .max_baud_rate = 921600,
};
ESP_ERROR_CHECK(lua_runtime_register_uart_port(&uart_port));

lua_runtime_job_config_t uart_job = {
    .name = "serial-tool",
    .path = "/sdcard/serial-tool.lua",
    .capabilities = LUA_RUNTIME_CAP_UART,
};
```

The pins above are illustrative. A board must register pins that are physically
available and not used by its display, storage, audio, modem, GPS, or console.

## Lua API

The built-in `runtime` module provides:

```lua
local runtime = require("runtime")
runtime.sleep(100)
local now = runtime.now_ms()
runtime.sleep_until(now + 33)
print(runtime.cancelled())
```

The built-in `audio` module is backed by the application audio service when an
audio backend is registered:

```lua
local audio = require("audio")
local jump = audio.play("/sdcard/game/jump.ogg", { volume = 80 })
audio.is_playing(jump)
audio.stop(jump)
audio.stop_all()
```

The current application backend accepts OGG/Opus files as short one-shot
effects. Playback handles support state queries and removal from the pending
sound-effect mixer without clearing system speech. Volume is applied during PCM
mixing, and `loop = true` wraps the decoded effect until its handle is stopped.
Up to four Lua sound-effect channels can play concurrently and mix with system
audio output. Decoded PCM is cached by content for repeated low-latency effects.

The built-in `uart` module is capability-gated and provides binary-safe serial
I/O:

```lua
local uart = require("uart")
local port = uart.open({
    port = 3,
    baud_rate = 115200,
    data_bits = 8,
    stop_bits = 1,
    parity = "none",
    rx_buffer = 2048,
})
port:write("AT\r\n")
local response = port:read(256, 1000)
local pending = port:available()
port:flush()
port:close()
```

`read()` and `poll_event()` periodically check runtime cancellation and timeout.
UART handles are exclusive and automatically released when their Lua VM closes.

The built-in `ui` module currently provides:

```text
ui.screen(options)
ui.screen_size()
ui.load(screen)
ui.rect(options)
ui.circle(options)
ui.line(options)
ui.arc(options)
ui.image(options)
ui.label(options)
ui.button(options)
ui.set_text(label, text)
ui.update(object, options)
ui.delete(object)
ui.poll_event(timeout_ms)
```

All LVGL operations use the project's display lock. Touch callbacks enqueue
plain events; Lua retrieves them through `ui.poll_event()` so LVGL never calls
into a Lua VM from the display thread.

Touch events include `pressed`, `moved`, `released`, `lost`, `x`, `y`, `dx`,
`dy`, and `time_ms`. The current hardware path is a single pointer, but move
tracking is preserved through LVGL's `PRESSING` events and is suitable for
swipes and drag controls.

Geometry and images can be created directly:

```lua
local ball = ui.circle({ parent = screen, x = 20, y = 20, radius = 12,
    color = 0xffcc00, opacity = 255, event_id = "ball" })

local ground = ui.line({ parent = screen, points = {
    { x = 0, y = 180 }, { x = 320, y = 180 },
}, color = 0xffffff, width = 3, rounded = true })

local gauge = ui.arc({ parent = screen, x = 220, y = 20,
    width = 72, height = 72, start_angle = 30, end_angle = 280,
    color = 0x20c997, line_width = 6 })

local sprite = ui.image({ parent = screen, src = "/sdcard/game/dino.png",
    x = 40, y = 120, rotation = 0, scale = 256,
    pivot_x = 16, pivot_y = 16, opacity = 255 })
```

Image sources can be an existing resource-partition path such as
`A:ic_app_back.spng`, or an absolute native filesystem path such as
`/sdcard/game/dino.png`. Native paths are exposed to LVGL through a read-only
filesystem adapter. The current firmware enables PNG decoding. The resource
partition's existing SPNG decoder remains available for `A:` assets.

`rotation` is expressed in degrees and `scale` uses LVGL units (`256` is 1x,
`128` is 0.5x, and `512` is 2x). `offset_x` and `offset_y` select an offset
inside an image, which is useful for sprite sheets. Any new graphical object
can receive touch events by setting `event_id`.

`ui.update()` can change common `x`, `y`, `width`, `height`, `color`,
`opacity`, `hidden`, and `z` properties. It can also replace line `points`, arc
`start_angle`, `end_angle`, and `line_width`, image `src`, `rotation`, `scale`,
`pivot_x`, `pivot_y`, `offset_x`, and `offset_y`, and label `text`. Updates are
applied in one display-lock operation. Combined with the monotonic runtime
clock, this supports fixed-rate game loops without accumulating frame drift.

See `examples/touch_demo.lua` for a basic rendering example and
`examples/dinosaur_game.lua` for a complete 30 FPS touch game with movement,
collision detection, scoring, and restart behavior. `examples/graphics_demo.lua`
demonstrates animated geometry, image transforms, and drag tracking.
