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
};

lua_runtime_job_id_t job_id;
ESP_ERROR_CHECK(lua_runtime_start(&config, &job_id));
```

Use `lua_runtime_get_job()` to read state and captured output, and
`lua_runtime_stop()` to cooperatively cancel a job. Every job has an isolated
Lua VM and FreeRTOS task.

Custom native modules can be registered once with
`lua_runtime_register_module()` before jobs are started.

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

The built-in `ui` module currently provides:

```text
ui.screen(options)
ui.screen_size()
ui.load(screen)
ui.rect(options)
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

`ui.update()` can change `x`, `y`, `width`, `height`, `color`, `hidden`, and
label `text` in one display-lock operation. Combined with the monotonic runtime
clock, this supports fixed-rate game loops without accumulating frame drift.

See `examples/touch_demo.lua` for a basic rendering example and
`examples/dinosaur_game.lua` for a complete 30 FPS touch game with movement,
collision detection, scoring, and restart behavior.
