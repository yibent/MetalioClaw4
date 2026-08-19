local runtime = require("runtime")
local uart = require("uart")

local serial = uart.open({
    port = 3,
    baud_rate = 115200,
    data_bits = 8,
    stop_bits = 1,
    parity = "none",
    rx_buffer = 2048,
})

serial:write("Lua UART ready\r\n")

while not runtime.cancelled() do
    local event = serial:poll_event(250)
    if event then
        local data = serial:read(math.min(event.available, 1024), 0)
        if #data > 0 then
            serial:write(data)
        end
    end
end

serial:close()
