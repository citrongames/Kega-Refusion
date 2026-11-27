-- Observe frames and controller polls, with counts that follow local saves and rewind.
assert(timeline.define("kegarefusion.examples.timeline-counter", 1, {
    frames = 0,
    polls = 0,
}))

local lastChange

local function drawCounts(counts)
    draw.text(12, 12, string.format("Frames: %d   Input-poll frames: %d",
        counts.frames, counts.polls), 0xffffffff, "hud")
    if lastChange then
        draw.text(12, 28, "Last timeline change: " .. lastChange, 0xffffffff, "hud")
    end
end

local function render()
    drawCounts(assert(timeline.get()))
end

local function timelineChanged()
    lastChange = assert(timeline.status())
    render()
end

emu.on("frame", function()
    local counts = assert(timeline.get())
    counts.frames = counts.frames + 1
    if emu.lagged() == false then
        counts.polls = counts.polls + 1
    end
    assert(timeline.set(counts))
    drawCounts(counts)
end)

emu.on("draw", render)
emu.on("loadstate", timelineChanged)
emu.on("rewind", timelineChanged)
emu.on("reset", timelineChanged)
render()
