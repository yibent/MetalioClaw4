#include "lua_self_test.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lua_runtime.h"

#include <cstring>
#include <stdio.h>

namespace {

constexpr const char* TAG = "LuaSelfTest";
constexpr uint32_t kUiCapability = LUA_RUNTIME_CAP_UI_TEST;
constexpr uint32_t kLogCapability = LUA_RUNTIME_CAP_LOG_OUTPUT;
constexpr uint32_t kStartupDelayMs = 3000;
#ifdef CONFIG_LUA_SELF_TEST_TIMEOUT_MS
constexpr uint32_t kSuiteTimeoutMs = CONFIG_LUA_SELF_TEST_TIMEOUT_MS;
#else
constexpr uint32_t kSuiteTimeoutMs = 60000;
#endif
#ifdef CONFIG_LUA_SELF_TEST_STRESS_ROUNDS
constexpr int kStressRounds = CONFIG_LUA_SELF_TEST_STRESS_ROUNDS;
#else
constexpr int kStressRounds = 5;
#endif

// Kept in the firmware so the test does not depend on an SD card or a writable
// filesystem.  Every public Lua module and every UI object type is exercised.
constexpr const char kApiSuite[] = R"LUA(
local runtime = require("runtime")
local ui = require("ui")
local audio = require("audio")

local pass, fail = 0, 0
local function check(name, condition, detail)
  if condition then
    pass = pass + 1
    print("PASS " .. name)
  else
    fail = fail + 1
    print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or ""))
  end
end
local function case(name, fn)
  print("CASE " .. name)
  local ok, err = pcall(fn)
  check(name, ok, err)
end

print("SUITE lua_api begin")
case("lua standard library", function()
  check("table", table.concat({"l", "u", "a"}) == "lua")
  check("string", string.upper("lua") == "LUA")
  check("math", math.floor(3.9) == 3)
  local co = coroutine.create(function() coroutine.yield(7) end)
  local ok, value = coroutine.resume(co)
  check("coroutine", ok and value == 7)
end)

case("args json", function()
  check("args string", args.suite == "boot")
  check("args number", args.answer == 42)
  check("args boolean", args.enabled == true)
  check("args array", args.items[2] == "b")
  check("args nested", args.nested.ok == true)
end)

case("runtime clock", function()
  local before = runtime.now_ms()
  runtime.sleep(12)
  local after = runtime.now_ms()
  check("sleep elapsed", after >= before)
  runtime.sleep_until(after + 5)
  check("sleep_until", runtime.now_ms() >= after + 5)
  check("cancelled false", runtime.cancelled() == false)
end)

case("ui screen and primitives", function()
  local w, h = ui.screen_size()
  check("screen_size", w > 0 and h > 0)
  local screen = ui.screen({background = 0x101820})
  local rect = ui.rect({parent = screen, x = 4, y = 5, width = 100, height = 40,
                        color = 0x102030, radius = 4, event_id = "rect"})
  local circle = ui.circle({parent = screen, x = 20, y = 55, radius = 12,
                            color = 0x20c997, opacity = 220, event_id = "circle"})
  local line = ui.line({parent = screen, points = {{x = 0, y = 0}, {x = 50, y = 50}},
                        color = 0xffffff, width = 2, rounded = true, event_id = "line"})
  local arc = ui.arc({parent = screen, x = 80, y = 55, width = 44, height = 44,
                      start_angle = 20, end_angle = 280, line_width = 4,
                      color = 0xffcc00, event_id = "arc"})
  local image = ui.image({parent = screen, src = "A:ic_app_test_pass.spng",
                          x = 140, y = 4, rotation = 3, scale = 128,
                          pivot_x = 8, pivot_y = 8, offset_x = 0, offset_y = 0,
                          opacity = 240, event_id = "image"})
  local label = ui.label({parent = screen, x = 4, y = 100, width = 180,
                          text = "before", color = 0xffffff})
  local button = ui.button({parent = screen, x = 4, y = 130, width = 120, height = 40,
                            text = "run", color = 0x2563eb, text_color = 0xffffff,
                            radius = 6, event_id = "run"})
  check("objects created", rect and circle and line and arc and image and label and button)
  ui.set_text(label, "after")
  ui.update(rect, {x = 8, y = 9, width = 110, height = 42, color = 0x203040,
                   hidden = false, opacity = 230, z = 0})
  ui.update(line, {points = {{x = 1, y = 2}, {x = 60, y = 70}}, color = 0xabcdef,
                   line_width = 3})
  ui.update(arc, {start_angle = 30, end_angle = 260, line_width = 5})
  ui.update(image, {src = "A:ic_app_test_pass.spng", rotation = 5, scale = 192,
                    pivot_x = 4, pivot_y = 4, offset_x = 1, offset_y = 2})
  ui.update(label, {text = "updated", color = 0xeeeeee})
  check("objects updated", true)
  ui.load(screen)
  ui._test_inject_event(button, "pressed", 11, 22, 3, 4)
  local event = ui.poll_event(100)
  check("event fields", event and event.id == "run" and event.type == "pressed" and
        event.x == 11 and event.y == 22 and event.dx == 3 and event.dy == 4)
  check("event timeout", ui.poll_event(5) == nil)
  ui._test_inject_event(button, "released", 11, 22)
  local released = ui.poll_event(100)
  check("released event", released and released.type == "released")
  ui.delete(rect); ui.delete(circle); ui.delete(line); ui.delete(arc)
  ui.delete(image); ui.delete(label); ui.delete(button)
  check("objects deleted", true)
end)

case("audio backend", function()
  local h = audio.play("builtin:success", {loop = true, volume = 37})
  check("play handle", type(h) == "number" and h > 0)
  runtime.sleep(80)
  check("playing state", audio.is_playing(h) == true)
  audio.stop(h)
  check("stopped state", audio.is_playing(h) == false)
  local h1 = audio.play("builtin:success", {loop = true})
  local h2 = audio.play("builtin:success", {loop = true, volume = 0})
  check("multiple handles", h1 ~= h2)
  audio.stop_all()
  check("stop_all", audio.is_playing(h1) == false and audio.is_playing(h2) == false)
end)

print(string.format("SUITE lua_api end pass=%d fail=%d", pass, fail))
assert(fail == 0, "Lua API assertions failed")
)LUA";

constexpr const char kNoCapabilitySuite[] = R"LUA(
local uart = require("uart")
local ok, err = pcall(function() uart.open({port = 0}) end)
assert(not ok and string.find(err, "capability", 1, true) ~= nil)
print("PASS uart capability gate")
)LUA";

constexpr const char kUartPolicySuite[] = R"LUA(
local uart = require("uart")
local ok, err = pcall(function()
  uart.open({port = 0, baud_rate = 115200, data_bits = 8, stop_bits = 1,
             parity = "none", rx_buffer = 256})
end)
assert(not ok and string.find(err, "board policy", 1, true) ~= nil)
print("PASS uart board policy gate")
)LUA";

constexpr const char kUartValidationSuite[] = R"LUA(
local uart = require("uart")
local function rejects(options)
  local ok = pcall(function() uart.open(options) end)
  assert(not ok)
end
rejects({port = -1})
rejects({port = 0, baud_rate = 299})
rejects({port = 0, rx_buffer = 255})
rejects({port = 0, data_bits = 9})
rejects({port = 0, stop_bits = 3})
rejects({port = 0, parity = "bad"})
print("PASS uart argument validation")
)LUA";

// A small page that behaves like a real app: build children, become active,
// process input, animate state, play a sound, then tear down its children.
constexpr const char kVirtualLifecycleApp[] = R"LUA(
local runtime, ui, audio = require("runtime"), require("ui"), require("audio")
local screen = ui.screen({background = 0x111827})
local title = ui.label({parent = screen, x = 8, y = 8, width = 280, text = "boot"})
local button = ui.button({parent = screen, x = 8, y = 48, width = 140, height = 42,
                          text = "open", event_id = "open"})
ui.load(screen)
local rounds = args.rounds or 5
for page = 1, rounds * 5 do
  ui.set_text(title, "page-" .. page)
  ui.update(button, {hidden = page == 2, x = 8 + page, y = 48 + page})
  ui._test_inject_event(button, "pressed", page, page * 2, 1, 1)
  local event = ui.poll_event(100)
  assert(event and event.id == "open" and event.type == "pressed")
  local handle = audio.play("builtin:success", {loop = true, volume = (10 + page) % 101})
  runtime.sleep(24)
  assert(audio.is_playing(handle))
  audio.stop(handle)
  assert(not audio.is_playing(handle))
  if page % 5 == 0 then print("PROGRESS lifecycle page=" .. page .. "/" .. (rounds * 5)) end
end
ui.delete(button)
ui.delete(title)
print("PASS virtual lifecycle app: boot->load->input->update->audio->teardown")
)LUA";

// Fixed-rate frame loop with event traffic between frames. This catches
// ordering bugs where update/delete races with poll_event or audio callbacks.
constexpr const char kVirtualFrameApp[] = R"LUA(
local runtime, ui = require("runtime"), require("ui")
local screen = ui.screen({background = 0x0b1220})
local label = ui.label({parent = screen, x = 4, y = 4, width = 240, text = "frame-0"})
local ball = ui.circle({parent = screen, x = 10, y = 50, radius = 8, event_id = "ball"})
ui.load(screen)
local next_frame = runtime.now_ms()
local rounds = args.rounds or 5
for frame = 1, rounds * 60 do
  next_frame = next_frame + 16
  runtime.sleep_until(next_frame)
  ui.update(ball, {x = 10 + (frame % 80), y = 50 + (frame % 20)})
  ui.update(label, {text = "frame-" .. frame})
  if frame % 8 == 0 then
    ui._test_inject_event(ball, "moved", frame, frame + 1, 2, 0)
    local event = ui.poll_event(20)
    assert(event and event.type == "moved" and event.dx == 2)
  end
  if frame % 60 == 0 then print("PROGRESS frame_loop frame=" .. frame .. "/" .. (rounds * 60)) end
end
assert(runtime.now_ms() >= next_frame)
ui.delete(ball); ui.delete(label)
print("PASS virtual frame app: " .. (rounds * 60) .. " frames at 16ms with input")
)LUA";

// Deliberately exercises invalid call ordering. Every error must be recoverable
// via pcall and the VM must still be able to create a valid page afterwards.
constexpr const char kVirtualInvalidOrderApp[] = R"LUA(
local runtime, ui, audio = require("runtime"), require("ui"), require("audio")
local function rejects(fn)
  local ok = pcall(fn)
  assert(not ok)
end
rejects(function() ui.load() end)
rejects(function() ui.set_text(99, "bad") end)
rejects(function() ui.update(99, {x = 1}) end)
rejects(function() ui.delete(99) end)
rejects(function() runtime.sleep(-1) end)
rejects(function() audio.play("/missing/lua-test.ogg") end)
rejects(function() audio.play("builtin:success", {volume = 101}) end)
local rounds = args.rounds or 5
local screen = ui.screen({background = 0x1f2937})
ui.load(screen)
for i = 1, rounds * 10 do
  local panel = ui.rect({parent = screen, x = 2, y = 2, width = 200, height = 80})
  local label = ui.label({parent = panel, text = "recovered"})
  ui.set_text(label, "recovered-" .. i)
  ui.delete(panel)
  if i % 10 == 0 then print("PROGRESS invalid_order page=" .. i .. "/" .. (rounds * 10)) end
end
print("PASS virtual invalid-order app: errors recovered across " .. (rounds * 10) .. " pages")
)LUA";

// Resources are intentionally leaked at Lua level before an uncaught error.
// The runtime's VM finalizers must release the screen and audio ownership.
constexpr const char kVirtualCrashApp[] = R"LUA(
local ui, audio = require("ui"), require("audio")
local screen = ui.screen({background = 0x220f0f})
ui.load(screen)
audio.play("builtin:success", {loop = true, volume = 5})
error("intentional virtual app crash after acquiring resources")
)LUA";

constexpr const char kVirtualCleanupProbe[] = R"LUA(
local ui, audio = require("ui"), require("audio")
local screen = ui.screen({background = 0x102a43})
ui.load(screen)
local handle = audio.play("builtin:success", {loop = true, volume = 5})
assert(audio.is_playing(handle))
audio.stop_all()
assert(not audio.is_playing(handle))
print("PASS virtual cleanup probe: screen and audio ownership reusable")
)LUA";

// Four concurrent effects are legal; the fifth must fail deterministically and
// stop_all must remove only this job's handles.
constexpr const char kVirtualAudioRaceApp[] = R"LUA(
local runtime, audio = require("runtime"), require("audio")
local rounds = args.rounds or 5
for cycle = 1, rounds * 10 do
  local handles = {}
  for i = 1, 4 do handles[i] = audio.play("builtin:success", {loop = true, volume = i}) end
  local ok = pcall(function() audio.play("builtin:success", {loop = true}) end)
  assert(not ok)
  runtime.sleep(30)
  for i = 1, 4 do assert(audio.is_playing(handles[i])) end
  audio.stop_all()
  for i = 1, 4 do assert(not audio.is_playing(handles[i])) end
  if cycle % 10 == 0 then print("PROGRESS audio_race cycle=" .. cycle .. "/" .. (rounds * 10)) end
end
print("PASS virtual audio race app: channel limit and cleanup across " .. (rounds * 10) .. " cycles")
)LUA";

// Rebuilds a page several times while input, animation and audio overlap. The
// page object is deliberately deleted as a parent so child ownership is also
// exercised on every navigation transition.
constexpr const char kVirtualNavigationApp[] = R"LUA(
local runtime, ui, audio = require("runtime"), require("ui"), require("audio")
local screen = ui.screen({background = 0x101827})
local header = ui.label({parent = screen, x = 6, y = 4, width = 300, text = "home"})
ui.load(screen)
local rounds = args.rounds or 5
for page = 1, rounds * 8 do
  local panel = ui.rect({parent = screen, x = 4, y = 28, width = 300, height = 150,
                         color = 0x1f3b5b, radius = 3})
  local title = ui.label({parent = panel, x = 8, y = 8, width = 260,
                          text = "page-" .. page})
  local next = ui.button({parent = panel, x = 8, y = 58, width = 130, height = 38,
                          text = "next", event_id = "next"})
  ui.set_text(header, "page-" .. page)
  ui._test_inject_event(next, "pressed", page, 1, 0, 0)
  local pressed = ui.poll_event(100)
  assert(pressed and pressed.id == "next" and pressed.type == "pressed")
  ui._test_inject_event(next, "released", page, 1, 0, 0)
  local released = ui.poll_event(100)
  assert(released and released.type == "released")
  local handle = audio.play("builtin:success", {loop = true, volume = page % 101})
  runtime.sleep(18)
  assert(audio.is_playing(handle))
  audio.stop(handle)
  ui.delete(panel)
  if page % 8 == 0 then print("PROGRESS navigation page=" .. page .. "/" .. (rounds * 8)) end
end
ui.delete(header)
print("PASS virtual navigation app: " .. (rounds * 8) .. " page transitions")
)LUA";

// Fills and drains the synthetic input queue while objects are being updated.
// The injector uses the same bounded queue policy as a real touch producer.
constexpr const char kVirtualEventStormApp[] = R"LUA(
local runtime, ui = require("runtime"), require("ui")
local screen = ui.screen({background = 0x0b1220})
local buttons = {}
for i = 1, 4 do
  buttons[i] = ui.button({parent = screen, x = 4 + i * 42, y = 32, width = 36, height = 30,
                          text = tostring(i), event_id = "b" .. i})
end
ui.load(screen)
local rounds = args.rounds or 5
for cycle = 1, rounds * 4 do
  for sequence = 1, 32 do
    local button = buttons[((sequence - 1) % 4) + 1]
    ui.update(button, {x = 4 + ((sequence - 1) % 4 + 1) * 42 + (cycle % 3)})
    ui._test_inject_event(button, "moved", sequence, cycle, 1, -1)
  end
  -- Queue capacity is 24. Overflow must discard the oldest event and retain
  -- the newest 24 in order.
  for sequence = 9, 32 do
    local event = ui.poll_event(100)
    assert(event and event.x == sequence and event.dx == 1 and event.dy == -1)
  end
  assert(ui.poll_event(0) == nil)
  runtime.sleep(2)
  if cycle % 4 == 0 then print("PROGRESS event_storm cycle=" .. cycle .. "/" .. (rounds * 4)) end
end
for i = 1, 4 do ui.delete(buttons[i]) end
print("PASS virtual event storm app: " .. (rounds * 4) .. " overflow bursts")
)LUA";

// Exercises nested parent/child ownership and stale-handle rejection after a
// parent is deleted, then immediately creates a replacement hierarchy.
constexpr const char kVirtualHierarchyApp[] = R"LUA(
local ui = require("ui")
local rounds = args.rounds or 5
local screen = ui.screen({background = 0x18202b})
ui.load(screen)
for cycle = 1, rounds * 10 do
  local root = ui.rect({parent = screen, x = 2, y = 2, width = 280, height = 180})
  local branch = ui.rect({parent = root, x = 4, y = 4, width = 250, height = 140})
  local label = ui.label({parent = branch, x = 4, y = 4, width = 200, text = "nested"})
  local button = ui.button({parent = branch, x = 4, y = 40, width = 120, height = 36,
                            text = "child", event_id = "child"})
  ui.update(label, {text = "nested-" .. cycle})
  ui._test_inject_event(button, "changed", cycle, 0)
  assert(ui.poll_event(100).id == "child")
  ui.delete(root)
  local ok = pcall(function() ui.update(label, {text = "stale"}) end)
  assert(not ok)
  if cycle % 10 == 0 then print("PROGRESS hierarchy cycle=" .. cycle .. "/" .. (rounds * 10)) end
end
print("PASS virtual hierarchy app: " .. (rounds * 10) .. " cascaded trees")
)LUA";

// Alternates stop/restart operations so handles are reused instead of only
// being released in a bulk stop_all call.
constexpr const char kVirtualAudioInterleaveApp[] = R"LUA(
local runtime, audio = require("runtime"), require("audio")
local rounds = args.rounds or 5
for cycle = 1, rounds * 12 do
  local handles = {}
  for i = 1, 4 do handles[i] = audio.play("builtin:success", {loop = true, volume = (cycle + i) % 101}) end
  runtime.sleep(12)
  audio.stop(handles[2]); audio.stop(handles[4])
  assert(not audio.is_playing(handles[2]) and not audio.is_playing(handles[4]))
  local replacement1 = audio.play("builtin:success", {loop = true, volume = 11})
  local replacement2 = audio.play("builtin:success", {loop = true, volume = 22})
  assert(audio.is_playing(handles[1]) and audio.is_playing(handles[3]))
  assert(audio.is_playing(replacement1) and audio.is_playing(replacement2))
  audio.stop(handles[1]); audio.stop(handles[3]); audio.stop(replacement1); audio.stop(replacement2)
  if cycle % 12 == 0 then print("PROGRESS audio_interleave cycle=" .. cycle .. "/" .. (rounds * 12)) end
end
print("PASS virtual audio interleave app: " .. (rounds * 12) .. " restart cycles")
)LUA";

constexpr const char kVirtualStopResourceApp[] = R"LUA(
local runtime, ui, audio = require("runtime"), require("ui"), require("audio")
local screen = ui.screen({background = 0x321010})
local label = ui.label({parent = screen, text = "running"})
ui.load(screen)
local handle = audio.play("builtin:success", {loop = true, volume = 9})
while true do
  ui.set_text(label, "tick-" .. runtime.now_ms())
  assert(audio.is_playing(handle))
  runtime.sleep(15)
end
)LUA";

constexpr const char kVirtualOwnerApp[] = R"LUA(
local runtime, ui = require("runtime"), require("ui")
local screen = ui.screen({background = 0x102030})
ui.load(screen)
print("OWNER acquired")
runtime.sleep(args.owner_ms or 1000)
print("OWNER released")
)LUA";

constexpr const char kVirtualContenderApp[] = R"LUA(
local ui = require("ui")
local ok, err = pcall(function() ui.screen({background = 0x301020}) end)
assert(not ok and string.find(err, "already owned", 1, true) ~= nil)
print("PASS contender rejected while owner active")
)LUA";

constexpr const char kVirtualWorkerApp[] = R"LUA(
local runtime = require("runtime")
runtime.sleep(args.hold_ms or 2000)
print("worker completed without cancellation")
)LUA";

constexpr const char kTimeoutSuite[] = R"LUA(
local runtime = require("runtime")
while true do runtime.sleep(10) end
)LUA";

constexpr const char kOutputSuite[] = R"LUA(
for i = 1, 64 do print(string.rep("x", 120)) end
)LUA";

volatile bool s_started = false;

void LogOutput(const char* name, esp_err_t result, const char* output) {
    ESP_LOGI(TAG, "%s result=%s", name, esp_err_to_name(result));
    if (!output)
        return;
    const char* cursor = output;
    while (*cursor) {
        const char* newline = strchr(cursor, '\n');
        const size_t length = newline ? static_cast<size_t>(newline - cursor) : strlen(cursor);
        ESP_LOGI(TAG, "[%s] %.*s", name, static_cast<int>(length), cursor);
        if (!newline)
            break;
        cursor = newline + 1;
    }
}

bool Run(const char* name, const char* code, const char* args, uint32_t timeout,
         uint32_t capabilities, esp_err_t expected = ESP_OK) {
    const int64_t started_us = esp_timer_get_time();
    const size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "CASE_BEGIN name=%s timeout_ms=%u capabilities=0x%lx", name,
             static_cast<unsigned>(timeout), static_cast<unsigned long>(capabilities));
    char output[4096] = {};
    lua_runtime_job_config_t config = {
        .name = name,
        .code = code,
        .path = nullptr,
        .args_json = args,
        .timeout_ms = timeout,
        .stack_size = 16 * 1024,
        .priority = 4,
        .capabilities = capabilities | kLogCapability,
    };
    const esp_err_t result = lua_runtime_run(&config, output, sizeof(output));
    // Successful jobs already mirror their PASS lines live. Replay captured
    // output only for expected-error cases, where the error text is useful.
    if (result != ESP_OK || expected != ESP_OK)
        LogOutput(name, result, output);
    const bool passed = result == expected;
    const size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "CASE_%s name=%s expected=%s actual=%s elapsed_ms=%u heap_delta=%d",
             passed ? "PASS" : "FAIL", name, esp_err_to_name(expected),
             esp_err_to_name(result),
             static_cast<unsigned>((esp_timer_get_time() - started_us) / 1000),
             static_cast<int>(heap_after) - static_cast<int>(heap_before));
    // lua_runtime marks the job terminal before its task clears the slot.
    // Let that final cleanup run before starting the next virtual app.
    vTaskDelay(pdMS_TO_TICKS(20));
    return passed;
}

bool RunStress(const char* name, const char* code, uint32_t base_timeout,
               uint32_t timeout_per_round, uint32_t capabilities,
               esp_err_t expected = ESP_OK) {
    char args[48] = {};
    snprintf(args, sizeof(args), "{\"rounds\":%d}", kStressRounds);
    const uint32_t timeout = base_timeout + static_cast<uint32_t>(kStressRounds) * timeout_per_round;
    return Run(name, code, args, timeout, capabilities, expected);
}

bool RunOutputCase() {
    ESP_LOGI(TAG, "CASE_BEGIN name=lua_output");
    lua_runtime_job_config_t config = {
        .name = "lua_output", .code = kOutputSuite, .path = nullptr, .args_json = "{}",
        .timeout_ms = 3000, .stack_size = 16 * 1024, .priority = 4,
        .capabilities = 0,
    };
    lua_runtime_job_id_t id = 0;
    if (lua_runtime_start(&config, &id) != ESP_OK)
        return false;
    for (int i = 0; i < 300; ++i) {
        lua_runtime_job_info_t info = {};
        char output[256] = {};
        if (lua_runtime_get_job(id, &info, output, sizeof(output)) == ESP_OK &&
            info.state >= LUA_RUNTIME_JOB_DONE) {
            ESP_LOGI(TAG, "lua_output state=%d length=%u truncated=%d sample=%.120s",
                     static_cast<int>(info.state), static_cast<unsigned>(info.output_length),
                     info.output_truncated, output);
            const bool passed = info.state == LUA_RUNTIME_JOB_DONE && info.output_truncated;
            ESP_LOGI(TAG, "CASE_%s name=lua_output", passed ? "PASS" : "FAIL");
            vTaskDelay(pdMS_TO_TICKS(20));
            return passed;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool RunStopCase() {
    ESP_LOGI(TAG, "CASE_BEGIN name=lua_stop");
    lua_runtime_job_config_t config = {
        .name = "lua_stop", .code = kTimeoutSuite, .path = nullptr, .args_json = "{}",
        .timeout_ms = 5000, .stack_size = 16 * 1024, .priority = 4,
        .capabilities = kLogCapability,
    };
    lua_runtime_job_id_t id = 0;
    if (lua_runtime_start(&config, &id) != ESP_OK)
        return false;
    vTaskDelay(pdMS_TO_TICKS(80));
    if (lua_runtime_stop(id) != ESP_OK)
        return false;
    for (int i = 0; i < 100; ++i) {
        lua_runtime_job_info_t info = {};
        char output[256] = {};
        if (lua_runtime_get_job(id, &info, output, sizeof(output)) == ESP_OK &&
            info.state >= LUA_RUNTIME_JOB_DONE) {
            ESP_LOGI(TAG, "lua_stop state=%d output=%s", static_cast<int>(info.state), output);
            const bool passed = info.state == LUA_RUNTIME_JOB_STOPPED;
            ESP_LOGI(TAG, "CASE_%s name=lua_stop", passed ? "PASS" : "FAIL");
            vTaskDelay(pdMS_TO_TICKS(20));
            return passed;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool RunStopResourceCase() {
    ESP_LOGI(TAG, "CASE_BEGIN name=virtual_stop_cleanup");
    lua_runtime_job_config_t config = {
        .name = "virtual_stop_resource", .code = kVirtualStopResourceApp, .path = nullptr,
        .args_json = "{}", .timeout_ms = 10000, .stack_size = 16 * 1024, .priority = 4,
        .capabilities = kUiCapability | kLogCapability,
    };
    lua_runtime_job_id_t id = 0;
    if (lua_runtime_start(&config, &id) != ESP_OK)
        return false;
    vTaskDelay(pdMS_TO_TICKS(120));
    if (lua_runtime_stop(id) != ESP_OK)
        return false;
    for (int i = 0; i < 200; ++i) {
        lua_runtime_job_info_t info = {};
        char output[512] = {};
        if (lua_runtime_get_job(id, &info, output, sizeof(output)) == ESP_OK &&
            info.state >= LUA_RUNTIME_JOB_DONE) {
            const bool passed = info.state == LUA_RUNTIME_JOB_STOPPED;
            ESP_LOGI(TAG, "virtual_stop_cleanup state=%d output=%s", static_cast<int>(info.state),
                     output);
            ESP_LOGI(TAG, "CASE_%s name=virtual_stop_cleanup", passed ? "PASS" : "FAIL");
            vTaskDelay(pdMS_TO_TICKS(20));
            return passed;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

bool RunUiContentionCase() {
    ESP_LOGI(TAG, "CASE_BEGIN name=virtual_ui_contention");
    for (int cycle = 1; cycle <= kStressRounds; ++cycle) {
        char owner_args[48] = {};
        snprintf(owner_args, sizeof(owner_args), "{\"owner_ms\":%d}", 500 + (cycle % 4) * 100);
        lua_runtime_job_config_t owner_config = {
            .name = "virtual_owner", .code = kVirtualOwnerApp, .path = nullptr,
            .args_json = owner_args, .timeout_ms = 3000, .stack_size = 16 * 1024,
            .priority = 4, .capabilities = kUiCapability | kLogCapability,
        };
        lua_runtime_job_config_t contender_config = {
            .name = "virtual_contender", .code = kVirtualContenderApp, .path = nullptr,
            .args_json = "{}", .timeout_ms = 3000, .stack_size = 16 * 1024, .priority = 4,
            .capabilities = kUiCapability | kLogCapability,
        };
        lua_runtime_job_id_t owner_id = 0;
        if (lua_runtime_start(&owner_config, &owner_id) != ESP_OK)
            return false;
        bool owner_acquired = false;
        for (int poll = 0; poll < 100; ++poll) {
            lua_runtime_job_info_t owner_info = {};
            char owner_output[256] = {};
            if (lua_runtime_get_job(owner_id, &owner_info, owner_output,
                                    sizeof(owner_output)) == ESP_OK) {
                owner_acquired = strstr(owner_output, "OWNER acquired") != nullptr;
                if (owner_info.state >= LUA_RUNTIME_JOB_DONE && !owner_acquired)
                    break;
            }
            if (owner_acquired)
                break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        if (!owner_acquired) {
            lua_runtime_stop(owner_id);
            ESP_LOGE(TAG, "contention cycle=%d owner never acquired display", cycle);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS((cycle % 3) * 10));
        lua_runtime_job_id_t contender_id = 0;
        if (lua_runtime_start(&contender_config, &contender_id) != ESP_OK) {
            lua_runtime_stop(owner_id);
            return false;
        }

        bool owner_done = false;
        bool contender_done = false;
        bool owner_ok = false;
        bool contender_ok = false;
        for (int i = 0; i < 300 && (!owner_done || !contender_done); ++i) {
            lua_runtime_job_info_t owner_info = {};
            lua_runtime_job_info_t contender_info = {};
            char owner_output[512] = {};
            char contender_output[512] = {};
            if (!owner_done && lua_runtime_get_job(owner_id, &owner_info, owner_output,
                                                   sizeof(owner_output)) == ESP_OK &&
                owner_info.state >= LUA_RUNTIME_JOB_DONE) {
                owner_done = true;
                owner_ok = owner_info.state == LUA_RUNTIME_JOB_DONE;
            }
            if (!contender_done && lua_runtime_get_job(contender_id, &contender_info,
                                                       contender_output,
                                                       sizeof(contender_output)) == ESP_OK &&
                contender_info.state >= LUA_RUNTIME_JOB_DONE) {
                contender_done = true;
                contender_ok = contender_info.state == LUA_RUNTIME_JOB_DONE;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!owner_done || !contender_done || !owner_ok || !contender_ok) {
            ESP_LOGE(TAG, "contention cycle=%d owner_done=%d contender_done=%d owner_ok=%d "
                          "contender_ok=%d",
                     cycle, owner_done, contender_done, owner_ok, contender_ok);
            return false;
        }
        ESP_LOGI(TAG, "PROGRESS ui_contention cycle=%d/%d", cycle, kStressRounds);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "CASE_PASS name=virtual_ui_contention cycles=%d", kStressRounds);
    return true;
}

bool RunJobSaturationCase() {
    ESP_LOGI(TAG, "CASE_BEGIN name=virtual_job_saturation");
    constexpr int kMaxAttempts = 16;
    lua_runtime_job_id_t ids[kMaxAttempts] = {};
    int started = 0;
    esp_err_t saturation_result = ESP_OK;
    lua_runtime_job_config_t config = {
        .name = "virtual_worker", .code = kVirtualWorkerApp, .path = nullptr,
        .args_json = "{\"hold_ms\":2000}", .timeout_ms = 5000,
        .stack_size = 12 * 1024, .priority = 3, .capabilities = kLogCapability,
    };
    for (; started < kMaxAttempts; ++started) {
        saturation_result = lua_runtime_start(&config, &ids[started]);
        if (saturation_result != ESP_OK)
            break;
    }
    ESP_LOGI(TAG, "job_saturation started=%d next_result=%s", started,
             esp_err_to_name(saturation_result));

    bool passed = started >= 2 && started < kMaxAttempts && saturation_result == ESP_ERR_NO_MEM;
    for (int i = 0; i < started; ++i) {
        if (lua_runtime_stop(ids[i]) != ESP_OK)
            passed = false;
    }
    for (int i = 0; i < started; ++i) {
        bool done = false;
        for (int poll = 0; poll < 300; ++poll) {
            lua_runtime_job_info_t info = {};
            if (lua_runtime_get_job(ids[i], &info, nullptr, 0) == ESP_OK &&
                info.state >= LUA_RUNTIME_JOB_DONE) {
                done = true;
                if (info.state != LUA_RUNTIME_JOB_STOPPED)
                    passed = false;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!done)
            passed = false;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI(TAG, "CASE_%s name=virtual_job_saturation started=%d",
             passed ? "PASS" : "FAIL", started);
    return passed;
}

void Task(void*) {
    vTaskDelay(pdMS_TO_TICKS(kStartupDelayMs));
    const int64_t started_us = esp_timer_get_time();
    const size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "===== unattended Lua self-test BEGIN =====");
    ESP_LOGI(TAG, "free_heap_before=%u", static_cast<unsigned>(free_before));
    ESP_LOGI(TAG, "stress_rounds=%d (timeouts scale with rounds)", kStressRounds);

    int failed = 0;
    if (!Run("lua_api", kApiSuite,
             "{\"suite\":\"boot\",\"answer\":42,\"enabled\":true,\"items\":[\"a\",\"b\"],\"nested\":{\"ok\":true}}",
             kSuiteTimeoutMs, kUiCapability))
        ++failed;
    if (!RunStress("virtual_lifecycle", kVirtualLifecycleApp, 3000, 300, kUiCapability))
        ++failed;
    if (!RunStress("virtual_lifecycle_reenter", kVirtualLifecycleApp, 3000, 300,
                   kUiCapability))
        ++failed;
    if (!RunStress("virtual_frame_loop", kVirtualFrameApp, 3000, 1300, kUiCapability))
        ++failed;
    if (!RunStress("virtual_invalid_order", kVirtualInvalidOrderApp, 3000, 200,
                   kUiCapability))
        ++failed;
    if (!Run("virtual_crash_cleanup", kVirtualCrashApp, "{}", 5000, kUiCapability,
              ESP_FAIL))
        ++failed;
    if (!Run("virtual_cleanup_probe", kVirtualCleanupProbe, "{}", 5000, kUiCapability))
        ++failed;
    if (!RunStress("virtual_audio_race", kVirtualAudioRaceApp, 3000, 500, 0))
        ++failed;
    if (!RunStress("virtual_navigation", kVirtualNavigationApp, 3000, 350, kUiCapability))
        ++failed;
    if (!RunStress("virtual_event_storm", kVirtualEventStormApp, 3000, 250, kUiCapability))
        ++failed;
    if (!RunStress("virtual_hierarchy", kVirtualHierarchyApp, 3000, 250, kUiCapability))
        ++failed;
    if (!RunStress("virtual_audio_interleave", kVirtualAudioInterleaveApp, 3000, 400, 0))
        ++failed;
    if (!RunStopResourceCase())
        ++failed;
    if (!Run("virtual_stop_cleanup_probe", kVirtualCleanupProbe, "{}", 5000, kUiCapability))
        ++failed;
    if (!RunUiContentionCase())
        ++failed;
    if (!RunJobSaturationCase())
        ++failed;
    if (!Run("uart_gate", kNoCapabilitySuite, "{}", 3000, 0))
        ++failed;
    if (!Run("uart_policy", kUartPolicySuite, "{}", 3000, LUA_RUNTIME_CAP_UART))
        ++failed;
    if (!Run("uart_validation", kUartValidationSuite, "{}", 3000, LUA_RUNTIME_CAP_UART))
        ++failed;
    constexpr int skipped = 1;
    ESP_LOGW(TAG, "CASE_SKIP name=uart_loopback reason=no board-approved free UART loopback");
    if (!Run("lua_timeout", kTimeoutSuite, "{}", 250, 0, ESP_ERR_TIMEOUT))
        ++failed;
    if (!RunStopCase())
        ++failed;
    if (!RunOutputCase())
        ++failed;

    const size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const int64_t elapsed_ms = (esp_timer_get_time() - started_us) / 1000;
    ESP_LOGI(TAG, "free_heap_after=%u delta=%d"
                 " (includes one-time audio decode/cache allocation)",
             static_cast<unsigned>(free_after),
             static_cast<int>(free_after) - static_cast<int>(free_before));
    ESP_LOGI(TAG,
             "===== unattended Lua self-test %s: failed=%d skipped=%d elapsed_ms=%u =====",
             failed == 0 ? "PASS" : "FAIL", failed, skipped,
             static_cast<unsigned>(elapsed_ms));
    s_started = false;
    vTaskDelete(nullptr);
}

}  // namespace

namespace LuaSelfTest {

void Start() {
#ifdef CONFIG_LUA_SELF_TEST_ON_BOOT
    if (s_started) {
        ESP_LOGW(TAG, "self-test already running");
        return;
    }
    s_started = true;
    if (xTaskCreate(Task, "lua_self_test", 24 * 1024, nullptr, 4, nullptr) != pdPASS) {
        s_started = false;
        ESP_LOGE(TAG, "failed to create self-test task");
    } else {
        ESP_LOGI(TAG, "self-test scheduled after boot");
    }
#else
    ESP_LOGD(TAG, "self-test disabled (CONFIG_LUA_SELF_TEST_ON_BOOT=n)");
#endif
}

}  // namespace LuaSelfTest
