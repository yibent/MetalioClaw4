local runtime = require("runtime")
local ui = require("ui")

local width, height = ui.screen_size()
local screen = ui.screen({ background = 0xf7f7f7 })
local ground_y = math.floor(height * 0.72)
local dino_w = math.max(28, math.floor(width * 0.055))
local dino_h = math.max(38, math.floor(height * 0.11))
local dino_x = math.floor(width * 0.12)
local obstacle_w = math.max(20, math.floor(width * 0.035))
local obstacle_h = math.max(38, math.floor(height * 0.10))

ui.rect({ parent = screen, x = 0, y = ground_y, width = width, height = 3, color = 0x53565a })
local title = ui.label({
    parent = screen,
    text = "LUA DINO",
    x = 18,
    y = 14,
    color = 0x53565a,
})
local score_label = ui.label({
    parent = screen,
    text = "00000",
    x = width - 105,
    y = 14,
    width = 90,
    color = 0x53565a,
})
local message = ui.label({
    parent = screen,
    text = "TOUCH TO JUMP",
    x = math.floor(width * 0.36),
    y = math.floor(height * 0.34),
    color = 0x53565a,
})

local dino = ui.rect({
    parent = screen,
    x = dino_x,
    y = ground_y - dino_h,
    width = dino_w,
    height = dino_h,
    color = 0x3c4043,
    radius = 3,
})
local eye = ui.rect({
    parent = dino,
    x = dino_w - 9,
    y = 7,
    width = 4,
    height = 4,
    color = 0xf7f7f7,
    radius = 2,
})

local obstacles = {}
for i = 1, 3 do
    local obstacle_height = obstacle_h + (i % 2) * math.floor(obstacle_h * 0.35)
    obstacles[i] = {
        x = width + (i - 1) * math.floor(width * 0.48),
        width = obstacle_w,
        height = obstacle_height,
        object = ui.rect({
            parent = screen,
            x = width + (i - 1) * math.floor(width * 0.48),
            y = ground_y - obstacle_height,
            width = obstacle_w,
            height = obstacle_height,
            color = 0x5f6368,
            radius = 2,
        }),
    }
end

ui.load(screen)

local gravity = height * 2.8
local jump_velocity = -height * 1.05
local speed = width * 0.38
local dino_y = ground_y - dino_h
local velocity_y = 0
local running = false
local game_over = false
local score = 0
local last_frame = runtime.now_ms()
local next_frame = last_frame

local function reset_game()
    dino_y = ground_y - dino_h
    velocity_y = 0
    score = 0
    speed = width * 0.38
    running = true
    game_over = false
    for i, obstacle in ipairs(obstacles) do
        obstacle.x = width + (i - 1) * math.floor(width * 0.48)
        ui.update(obstacle.object, { x = math.floor(obstacle.x) })
    end
    ui.update(dino, { y = math.floor(dino_y), color = 0x3c4043 })
    ui.update(message, { hidden = true })
    ui.set_text(score_label, "00000")
end

local function jump()
    if game_over then
        reset_game()
        velocity_y = jump_velocity
    elseif not running then
        running = true
        velocity_y = jump_velocity
        ui.update(message, { hidden = true })
    elseif dino_y >= ground_y - dino_h - 1 then
        velocity_y = jump_velocity
    end
end

local function overlaps(obstacle)
    local padding = 4
    return dino_x + dino_w - padding > obstacle.x
        and dino_x + padding < obstacle.x + obstacle.width
        and dino_y + dino_h - padding > ground_y - obstacle.height
end

while true do
    local event = ui.poll_event(0)
    while event do
        if event.type == "pressed" then
            jump()
        end
        event = ui.poll_event(0)
    end

    local now = runtime.now_ms()
    local dt_ms = now - last_frame
    last_frame = now
    if dt_ms > 50 then
        dt_ms = 50
    end
    local dt = dt_ms / 1000

    if running and not game_over then
        velocity_y = velocity_y + gravity * dt
        dino_y = dino_y + velocity_y * dt
        if dino_y >= ground_y - dino_h then
            dino_y = ground_y - dino_h
            velocity_y = 0
        end
        ui.update(dino, { y = math.floor(dino_y) })

        for _, obstacle in ipairs(obstacles) do
            obstacle.x = obstacle.x - speed * dt
            if obstacle.x + obstacle.width < 0 then
                obstacle.x = obstacle.x + width + math.floor(width * 0.44)
            end
            ui.update(obstacle.object, { x = math.floor(obstacle.x) })
            if overlaps(obstacle) then
                game_over = true
                running = false
                ui.update(dino, { color = 0xb3261e })
                ui.update(message, {
                    text = "GAME OVER - TOUCH TO RESTART",
                    hidden = false,
                })
            end
        end

        score = score + dt * 10
        speed = math.min(width * 0.72, speed + width * 0.006 * dt)
        ui.set_text(score_label, string.format("%05d", math.floor(score)))
    end

    next_frame = next_frame + 33
    if next_frame < now then
        next_frame = now
    end
    runtime.sleep_until(next_frame)
end
