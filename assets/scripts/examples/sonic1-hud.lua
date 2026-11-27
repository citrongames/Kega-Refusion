-- Positions, velocities and collision radii for Sonic the Hedgehog REV00.
-- Open the original Mega Drive cartridge, run this script and enter a level.
-- Header and object fields: https://github.com/sonicretro/s1disasm/blob/AS/sonic.asm
-- https://github.com/sonicretro/s1disasm/blob/AS/_Constants.asm
-- Work RAM map: https://github.com/cvghivebrain/s1disasm/blob/main/RAM%20Addresses.asm

-- Cartridge ROM and CD Program RAM can share base and size.
local cartridgeRom, mainRam = 1, 2
local rom, ram
for _, domain in ipairs(memory.domains()) do
    if domain.id == cartridgeRom and domain.size == 0x80000 then
        rom = domain.id
    elseif domain.id == mainRam and domain.size == 0x10000 then
        ram = domain.id
    end
end
assert(rom and ram and memory.range(rom, 0x150, 9) == "SONIC THE"
    and memory.range(rom, 0x168, 8) == "HEDGEHOG"
    and memory.range(rom, 0x180, 14) == "GM 00001009-00"
    and memory.read(rom, 0x18e, 2, "be") == 0x264a,
    "Open Sonic the Hedgehog (USA, Europe), original REV00 cartridge")

-- Work RAM offsets relative to $FF0000.
local player = 0xd000
local gameMode = 0xf600
local cameraX, cameraY = 0xf700, 0xf704

local function word(offset)
    return memory.read(ram, offset, 2, "be", true)
end

local function render()
    local mode = memory.read(ram, gameMode, 1)
    if mode ~= 0x0c and mode ~= 0x08 then
        return
    end

    local x, y = word(player + 0x08), word(player + 0x0c)
    local vx, vy = word(player + 0x10), word(player + 0x12)
    local cx, cy = word(cameraX), word(cameraY)
    local ry = memory.read(ram, player + 0x16, 1)
    local rx = memory.read(ram, player + 0x17, 1)
    if not (x and y and vx and vy and cx and cy and rx and ry)
        or rx == 0 or ry == 0 then
        return
    end

    -- The rectangle shows the object's current collision radii.
    local sx, sy = x - cx, y - cy
    local screen = emu.screen()
    if sx + rx >= 0 and sx - rx < screen.width
        and sy + ry >= 0 and sy - ry < screen.height then
        draw.rect(sx - rx, sy - ry, rx * 2, ry * 2, 0x7fffd4ff, 0x7fffd448)
        draw.circle(sx, sy, 2, 0xffffffff)
    end
    -- Velocity words have eight fractional bits and are reported in pixels/frame.
    draw.text(12, 12, string.format("X %d  Y %d   VX %.2f  VY %.2f",
        x, y, vx / 256, vy / 256), 0xffffffff, "hud")
end

for _, event in ipairs({"frame", "reset", "loadstate", "rewind", "draw"}) do
    emu.on(event, render)
end
