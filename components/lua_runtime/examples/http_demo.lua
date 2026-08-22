local http = require("http")

local res, err = http.get("https://example.com/", {
    timeout_ms = 15000,
    headers = { ["Accept"] = "text/html" },
})
if not res then
    print("request failed: " .. tostring(err))
    return
end

print("status=" .. tostring(res.status))
print("content-type=" .. tostring(res.headers["content-type"] or ""))
print(res.body)
