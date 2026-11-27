-- For the original Kega Lua Overlay fixture, revision KR-LUA1.
-- tests/lua_runtime_tests.cpp generates its Mega Drive cartridge.
assert(memory.range(1, 0x1c0, 7) == "KR-LUA1",
    "Open the generated Kega Lua Overlay fixture (KR-LUA1)")

local function render()
    local x = memory.read(2, 0x20, 2)
    local y = memory.read(2, 0x22, 2)
    local speed = memory.read(2, 0x24, 2, "be", true)
    local halfWidth = memory.read(2, 0x26, 2)
    local halfHeight = memory.read(2, 0x28, 2)
    if not (x and y and speed and halfWidth and halfHeight) then
        return
    end

    draw.rect(x - halfWidth, y - halfHeight, halfWidth * 2, halfHeight * 2,
        0x7fffd4ff, 0x7fffd448)
    draw.circle(x, y, 2, 0xffffffff)
    draw.text(12, 12, string.format("X %d   Y %d   Speed %.2f", x, y, speed / 256),
        0xffffffff, "hud")
end

for _, event in ipairs({"frame", "reset", "loadstate", "rewind", "draw"}) do
    emu.on(event, render)
end
