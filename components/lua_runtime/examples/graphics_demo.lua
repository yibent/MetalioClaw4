local runtime = require("runtime")
local ui = require("ui")

local screen = ui.screen({ background = 0x101418 })
local width, height = ui.screen_size()

ui.line({
    parent = screen,
    points = {
        { x = 0, y = height - 42 },
        { x = width, y = height - 42 },
    },
    color = 0x768390,
    width = 3,
})

local ball = ui.circle({
    parent = screen,
    x = 24,
    y = height - 70,
    radius = 14,
    color = 0xffc857,
    event_id = "ball",
})

local arc = ui.arc({
    parent = screen,
    x = width - 82,
    y = 18,
    width = 62,
    height = 62,
    start_angle = 0,
    end_angle = 45,
    color = 0x40c9a2,
    line_width = 6,
})

local icon = ui.image({
    parent = screen,
    src = "A:ic_app_back.spng",
    x = 18,
    y = 18,
    pivot_x = 22,
    pivot_y = 22,
    scale = 256,
    opacity = 220,
})

ui.load(screen)

local started = runtime.now_ms()
local next_frame = started
while not runtime.cancelled() do
    local event = ui.poll_event(0)
    if event and event.id == "ball" and
        (event.type == "pressed" or event.type == "moved") then
        ui.update(ball, { x = event.x - 14, y = event.y - 14 })
    end

    local elapsed = runtime.now_ms() - started
    local phase = elapsed % 2000
    local angle = math.floor((elapsed % 3600) / 10)
    local end_angle = math.floor(45 + phase * 270 / 2000)
    local scale = 192 + math.floor(phase * 128 / 2000)

    ui.update(arc, { end_angle = end_angle })
    ui.update(icon, { rotation = angle, scale = scale })

    next_frame = next_frame + 33
    runtime.sleep_until(next_frame)
end
