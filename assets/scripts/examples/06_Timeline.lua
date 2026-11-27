-- Save, load, rewind, or reset to see these counts follow the emulated clock.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

assert(timeline.define("example-poll-counts", 1, {frames = 0, observed = 0, polls = 0}))
local change = "—"
local states = {
    current = text("current", "текущие"),
    restored = text("restored", "восстановлены"),
    missing = text("missing", "нет в снимке"),
    incompatible = text("different source", "другой скрипт"),
    reset = text("reset", "сброшены")
}

local function render(counts)
    local frames = string.format(text("Frames: %d", "Кадры: %d"), counts.frames)
    local polls = string.format(text("Polls: %d/%d", "Опрос: %d/%d"), counts.polls, counts.observed)
    local detail = string.format("%s · %s", change, states[timeline.status()])
    local width = math.max(utf8.len(frames), utf8.len(polls), utf8.len(detail)) * 14 + 14
    draw.rect(10, 160, width, 59, 0x51bcafdf, 0x09121ddd, "hud")
    draw.text(17, 164, frames, 0x7fffd4ff, "hud")
    draw.text(17, 181, polls, 0xffffffff, "hud")
    draw.text(17, 198, detail, 0xffffffff, "hud")
end

emu.on("frame", function()
    local counts = timeline.get()
    counts.frames = counts.frames + 1
    local lagged = emu.lagged()
    if lagged ~= nil then
        counts.observed = counts.observed + 1
        if not lagged then counts.polls = counts.polls + 1 end
    end
    assert(timeline.set(counts))
    render(counts)
end)
emu.on("draw", function() render(timeline.get()) end)
for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function(e)
        change = e.event
        render(timeline.get())
    end)
end
