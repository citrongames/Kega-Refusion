-- Run on the original Mega Drive Kega Lua Overlay fixture (KR-LUA1).
-- Generate it with lua_runtime_tests --write-fixture /path/to/overlay.md.
assert(memory.range(1, 0x1c0, 7, 0) == "KR-LUA1",
    "Open the generated Kega Lua Overlay fixture (KR-LUA1)")

local cpu, loop = 1, 0x23c
assert(memory.peek(cpu, loop, 2) == 0x60fe, "Unexpected fixture loop address")
local workRam
for _, domain in ipairs(memory.domains()) do
    if domain.base == 0xff0000 and domain.writable then
        workRam = domain.id
        break
    end
end
assert(workRam, "Fixture work RAM is unavailable")

assert(memory.write(workRam, 0x2a, 0x1234, 2))
assert(memory.label(cpu, loop, "FixtureLoop"))
assert(memory.watch(workRam, 0x2a, 2, "Fixture marker"))

local callbacks, lastFrameCallbacks = 0, 0
local lastFrameTruncated = false
assert(memory.on(cpu, "exec", loop, nil, function()
    callbacks = callbacks + 1
end))

local function render()
    local suffix = lastFrameTruncated and " (budget reached)" or ""
    draw.text(12, 12, string.format("Loop callbacks: %d%s", lastFrameCallbacks, suffix),
        0xffffffff, "hud")
end

emu.on("frame", function()
    local stats = memory.hookstats()
    lastFrameCallbacks, callbacks = callbacks, 0
    lastFrameTruncated = stats and stats.truncated
    render()
end)
emu.on("draw", render)
