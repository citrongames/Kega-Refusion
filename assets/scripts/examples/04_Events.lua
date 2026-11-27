-- Compare callbacks before/after execution. Pause, save/load, reset, and rewind.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local before, completed, poll
local lastChange = "—"

local function render(e)
    local side = e.instance == 0 and "A" or "B"
    local detail = poll == nil and "—" or (poll and text("yes", "да") or text("no", "нет"))
    local title = string.format("%s · %s @ %d", side, e.event, e.frame)
    local frames = string.format(text("Before %s / Frame %s", "До %s / Кадр %s"), before or "—", completed or "—")
    local status = string.format(text("Poll %s · %s", "Опрос %s · %s"), detail, lastChange)
    local width = math.max(utf8.len(title), utf8.len(frames), utf8.len(status)) * 14 + 14
    draw.rect(10, 94, width, 59, 0x718592c0, 0x09121ddd, "hud")
    draw.text(17, 98, title, 0x7fffd4ff, "hud")
    draw.text(17, 115, frames, 0xffffffff, "hud")
    draw.text(17, 132, status, 0xffffffff, "hud")
end

emu.on("beforeframe", function(e)
    before = e.frame -- This clock still belongs to the previous completed frame.
end)
emu.on("frame", function(e)
    completed = e.frame
    if before ~= completed - 1 then before = nil end
    local lagged = emu.lagged()
    poll = nil
    if lagged ~= nil then poll = not lagged end
    render(e)
end)
emu.on("draw", render) -- Redraw the same frame without sampling game activity.
for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function(e)
        before, completed, poll = nil, nil, nil
        lastChange = e.event
        render(e)
    end)
end
