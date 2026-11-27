-- Live input-poll estimate. Each graph point covers 60 emulated frames.
local window, history = 60, 48
local points = {}
local latest, count, elapsed, active = 0, 0, 0, 0
local rate, unavailable

local function render()
    local text = "Measuring input activity"
    if unavailable then
        text = "Input-poll FPS unavailable"
    elseif count > 0 then
        text = string.format("Input-poll FPS %.1f / %.2f", points[latest] * rate, rate)
    end
    draw.text(12, 12, text, 0xffffffff, "hud")
    if count == 0 then return end

    local left, bottom, height, step = 12, 74, 40, 4
    draw.rect(left, bottom - height, (history - 1) * step, height,
        0x779598aa, 0x101c24c0, "hud")
    for age = count - 1, 1, -1 do
        local first = points[(latest - age - 1) % history + 1]
        local second = points[(latest - age) % history + 1]
        local x = left + (count - 1 - age) * step
        draw.line(x, bottom - first * height, x + step, bottom - second * height,
            0x7fffd4ff, "hud")
    end
end

local function clear()
    latest, count, elapsed, active = 0, 0, 0, 0
    rate, unavailable = nil, false
end

emu.on("frame", function()
    local fps, lagged = emu.fps(), emu.lagged()
    if fps ~= rate or lagged == nil then clear() end
    rate, unavailable = fps, lagged == nil or fps == nil
    if not unavailable then
        elapsed = elapsed + 1
        if not lagged then active = active + 1 end
        if elapsed == window then
            latest = latest % history + 1
            points[latest] = active / window
            count = math.min(count + 1, history)
            elapsed, active = 0, 0
        end
    end
    render()
end)

for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function()
        clear()
        render()
    end)
end
emu.on("draw", render)
