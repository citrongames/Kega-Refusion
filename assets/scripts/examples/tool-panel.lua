-- A separate Qt window. Works with any loaded game; no fixed RAM addresses.
-- Graph values estimate controller polling, not universal game-update FPS.
local panel = assert(ui.window("Game tools", {
    {type="row", children={
        {type="button", id="pause", text="Pause"},
        {type="button", id="resume", text="Resume"},
        {type="button", id="clear", text="Clear graph"}
    }},
    {type="group", text="Overlay", children={
        {type="checkbox", id="hud", text="Show HUD", value=true},
        {type="select", id="color", text="Text color",
            items={"Turquoise", "White", "Yellow"}, value=1}
    }},
    {type="number", id="period", text="Frames per sample",
        min=1, max=120, step=1, value=60},
    {type="text", id="note", text="Note", value="My observations"},
    {type="graph", id="activity", text="Input-poll estimate"},
    {type="label", id="status", text="Waiting for completed frames"}
}))

local points, elapsed, polls = {}, 0, 0
local sampleFrames, showHud, color = 60, true, 1
local colors = {0x7fffd4ff, 0xffffffff, 0xffdf60ff}
local dirty, refreshAt = true, 0
local estimate
local rate

local function clear()
    points, elapsed, polls, estimate = {}, 0, 0, nil
    dirty = true
    refreshAt = 0
end

local function render(e)
    if not panel or not showHud then return end
    local text = estimate and string.format("Input-poll estimate: %.1f", estimate)
        or "Measuring input activity"
    draw.text(12, 12, text, colors[color], "hud")
    draw.text(12, 28, string.format("Frame %d", e.frame), colors[color], "hud")
end

assert(ui.on(panel, function(e)
    if e.event == "close" then
        panel = nil
        return
    end
    if e.control == "pause" or e.control == "resume" then
        local ok, why = emu.pause(e.control == "pause")
        if not ok then ui.set(panel, "status", why) end
    elseif e.control == "clear" then
        clear()
        ui.set(panel, "activity", {})
    elseif e.control == "hud" then
        showHud = e.value
    elseif e.control == "color" then
        color = e.value
    elseif e.control == "period" then
        sampleFrames = math.floor(e.value)
        ui.set(panel, "period", sampleFrames)
        clear()
    end
end))

emu.on("frame", function(e)
    if not panel then return end
    local fps, lagged = emu.fps(), emu.lagged()
    if fps ~= rate then clear(); rate = fps end
    if fps and lagged ~= nil then
        elapsed = elapsed + 1
        if not lagged then polls = polls + 1 end
        if elapsed >= sampleFrames then
            estimate = fps * polls / elapsed
            if #points == 96 then table.remove(points, 1) end
            points[#points + 1] = estimate
            elapsed, polls, dirty = 0, 0, true
        end
    end
    if dirty then
        ui.set(panel, "activity", points)
        dirty = false
    end
    if e.frame >= refreshAt then
        ui.set(panel, "status", string.format("Frame %d   Refresh %.2f Hz",
            e.frame, fps or 0))
        refreshAt = e.frame + 12
    end
    render(e)
end)

for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function(e)
        clear()
        render(e)
    end)
end
emu.on("draw", render)
