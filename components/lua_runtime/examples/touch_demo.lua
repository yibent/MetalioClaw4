local ui = require("ui")

local screen = ui.screen({ background = 0x101418 })
ui.label({ parent = screen, text = "Lua Runtime", x = 24, y = 24, color = 0xffffff })
local status = ui.label({ parent = screen, text = "Touch the button", x = 24, y = 72, color = 0xa7b0ba })
ui.button({
    parent = screen,
    text = "Touch",
    event_id = "touch_button",
    x = 24,
    y = 120,
    width = 180,
    height = 64,
    color = 0x2563eb,
})
ui.load(screen)

while true do
    local event = ui.poll_event(100)
    if event and event.id == "touch_button" and event.type == "clicked" then
        ui.set_text(status, string.format("Touched at %d, %d", event.x, event.y))
    end
end
