-- Sonic the Hedgehog (USA, Europe), REV00. Read-only Kega Refusion tool.
-- Disassembly references (RAM layout, ReactToItem, AnglePos, FindFloor/Wall,
-- and the six boss controllers): https://github.com/sonicretro/s1disasm/tree/AS
-- TAS HUD and timer reference: https://github.com/flamewing/sonic-gens-hud
-- Collision tables are read from the user's cartridge. This script contains no ROM data.

local russian = emu.language == "ru"
local function T(en, ru)
    return russian and ru or en
end

local rom, ram
for _, domain in ipairs(memory.domains()) do
    if domain.id == 1 and domain.base == 0 and domain.size == 0x80000 then
        rom = domain.id
    elseif domain.id == 2 and domain.base == 0xff0000 and domain.size == 0x10000 then
        ram = domain.id
    end
end
local unsupported = T("Open Sonic the Hedgehog (USA, Europe), original REV00 cartridge",
    "Откройте оригинальный картридж Sonic the Hedgehog (USA, Europe), REV00")
assert(rom and ram and memory.range(rom, 0x180, 14) == "GM 00001009-00", unsupported)

-- Verify the whole cartridge over a short series of frames, within Lua's
-- per-event instruction budget. A retained header alone also matches hacks.
local crcTable, crc, romOffset, verified = {}, 0xffffffff, 0, false
for i = 0, 255 do
    local value = i
    for _ = 1, 8 do
        value = (value >> 1) ~ (-(value & 1) & 0xedb88320)
    end
    crcTable[i] = value
end

local byte, format = string.byte, string.format
local function verifyPage()
    local size = math.min(4096, 0x80000 - romOffset)
    local data = assert(memory.range(rom, romOffset, size), unsupported)
    local value, lookup = crc, crcTable
    for first = 1, size, 64 do
        local bytes = {byte(data, first, first + 63)}
        for i = 1, 64 do
            value = (value >> 8) ~ lookup[(value ~ bytes[i]) & 0xff]
        end
    end
    crc = value
    romOffset = romOffset + size
    if romOffset == 0x80000 then
        assert((crc ~ 0xffffffff) == 0xf9394e97, unsupported)
        crcTable, verified = nil, true
    end
end

local function word(data, offset)
    local high, low = byte(data, offset + 1, offset + 2)
    return high * 256 + low
end

local function u16(high, low)
    return high * 256 + low
end

local function signedWord(n)
    return ((n + 0x8000) & 0xffff) - 0x8000
end

local function signedByte(n)
    return ((n + 0x80) & 0xff) - 0x80
end

local PAL = {
    white = 0xf3f7ffff, cyan = 0x55dfedff, yellow = 0xffe594ff,
    violet = 0xe2aaffff, orange = 0xffa47fff,
    border = 0x497786e8, background = 0x09121de2,
}

local bosses = {
    [0x3d] = "GHZ", [0x77] = "LZ", [0x73] = "MZ", [0x7a] = "SLZ",
    [0x75] = "SYZ", [0x85] = "FZ",
}
local zones = {[0] = "GHZ", "LZ", "MZ", "SLZ", "SYZ", "SBZ"}
local names = {
    [0x01] = T("Sonic", "Соник"), [0x0d] = T("Signpost", "Табличка"),
    [0x11] = T("Bridge", "Мост"), [0x15] = T("Swing", "Качели"),
    [0x18] = T("Platform", "Платформа"), [0x1f] = "Crabmeat", [0x22] = "Buzz Bomber",
    [0x25] = T("Ring", "Кольцо"), [0x26] = T("Monitor", "Монитор"),
    [0x2b] = "Chopper", [0x36] = T("Spikes", "Шипы"), [0x40] = "Moto Bug",
    [0x41] = T("Spring", "Пружина"), [0x42] = "Newtron",
    [0x48] = T("Wrecking ball", "Шар босса"), [0x78] = "Caterkiller",
    [0x79] = T("Checkpoint", "Чекпойнт"), [0x84] = T("Crusher", "Пресс"),
}
for id, zone in pairs(bosses) do
    names[id] = T("Boss ", "Босс ") .. zone
end

local settings = {
    hud = true, objects = false, sensors = true, terrain = false,
    bosses = true, speed = true, trail = false,
}
local selectedSlot, boxLimit, terrainLimit, trailLimit = 0, 12, 24, 12
local historyLimit, attemptLimit, attemptStep = 128, 240, 4
local history, historyHead, historyCount = {}, 0, 0
local level, lastFrame, attempt, ghost, view
local refreshPanel = true

local function historyPoint(index)
    return history[(historyHead - historyCount + index - 1) % historyLimit + 1]
end

local function resetHistory()
    historyHead, historyCount = 0, 0
end

local function pushHistory(frame, p)
    historyHead = historyHead % historyLimit + 1
    historyCount = math.min(historyCount + 1, historyLimit)
    local sample = history[historyHead] or {}
    sample.frame, sample.x, sample.y = frame, p.x, p.y
    sample.speed = settings.speed and math.sqrt(p.vx * p.vx + p.vy * p.vy) / 256 or nil
    history[historyHead] = sample
end

local function trimHistory(frame)
    while historyCount > 0 and history[historyHead].frame > frame do
        historyHead = (historyHead - 2) % historyLimit + 1
        historyCount = historyCount - 1
    end
end

local function attemptPoint(p)
    return {x = p.x, y = p.y}
end

local function trimAttempt(frame)
    if not attempt then
        return
    end
    if frame < attempt.start then
        attempt = nil
        return
    end
    local points = attempt.points
    local last = (frame - attempt.start) // attemptStep + 1
    for i = #points, last + 1, -1 do
        points[i] = nil
    end
end

local function levelName(key)
    if key == 0x0103 then return "SBZ 3" end
    if key == 0x0502 then return "FZ" end
    return format("%s %d", zones[key >> 8] or "?", (key & 0xff) + 1)
end

local function readPlayer()
    local mode = memory.read(ram, 0xf600)
    if mode ~= 0x0c and mode ~= 0x08 then return nil end
    -- FE08 is the active debug editor, which uses the player slot as a cursor.
    local live = memory.range(ram, 0xfe08, 10)
    if not live or word(live, 0) ~= 0 then return nil end
    local key = word(live, 8)
    local raw = memory.range(ram, 0xd000, 0x40)
    if not raw or byte(raw, 1) ~= 1 then return nil end
    local xh, xl, sxh, sxl, yh, yl, syh, syl, vxh, vxl, vyh, vyl, ih, il, rh, rw =
        byte(raw, 0x09, 0x18)
    if rw == 0 or rh < 3 then return nil end
    local camera = memory.range(ram, 0xf700, 6)
    if not camera then return nil end
    local cxh, cxl, _, _, cyh, cyl = byte(camera, 1, 6)
    local status, _, routine, _, angle = byte(raw, 0x23, 0x27)
    local p = {
        key = key, mode = mode, raw = raw, x = signedWord(u16(xh, xl)),
        y = signedWord(u16(yh, yl)), sx = u16(sxh, sxl), sy = u16(syh, syl),
        vx = signedWord(u16(vxh, vxl)), vy = signedWord(u16(vyh, vyl)),
        inertia = signedWord(u16(ih, il)), rw = rw, rh = rh,
        angle = angle, routine = routine,
        air = (status & 2) ~= 0, rolling = (status & 4) ~= 0,
        platform = (status & 8) ~= 0, pushing = (status & 0x20) ~= 0,
        underwater = (status & 0x40) ~= 0,
        cx = signedWord(u16(cxh, cxl)), cy = signedWord(u16(cyh, cyl)),
        looping = (byte(raw, 2) & 0x40) ~= 0,
    }
    -- ReactToItem uses a separate rectangle, including REV00's duck frame.
    local reactHeight = p.rh - 3
    p.reactY = p.y - reactHeight
    if byte(raw, 0x1b) == 0x39 then
        p.reactY, reactHeight = p.reactY + 12, 10
    end
    p.reactH = 2 * reactHeight
    return p
end

local function slopeMode(angle)
    local bias = 0x1f
    if angle >= 0xe0 or (angle >= 0x60 and angle < 0x80) then bias = 0x20 end
    return (angle + bias) & 0xc0
end

local function motionState(p)
    if p.routine >= 6 then return T("dying", "гибель") end
    if p.routine == 4 then return T("hurt", "урон") end
    if p.platform then return format(T("on slot %02X", "на слоте %02X"), byte(p.raw, 0x3e)) end
    if p.air then return T("air", "воздух") end
    local mode = slopeMode(p.angle)
    if mode == 0x40 then return T("left wall", "левая стена") end
    if mode == 0x80 then return T("ceiling", "потолок") end
    if mode == 0xc0 then return T("right wall", "правая стена") end
    return T("ground", "земля")
end

local function visible(v, x, y, rx, ry)
    return x + rx >= 0 and x - rx < v.width and y + ry >= 0 and y - ry < v.height
end

local function validSlot(slot)
    return slot >= 0 and slot < 128
end

local touchSizes
local touchType = {[0] = T("enemy/boss", "враг/босс"), T("item", "предмет"),
    T("hazard", "урон"), T("special", "особая")}
local touchColor = {[0] = PAL.orange, PAL.yellow, 0xff7a91ff, PAL.violet}
local function touchRadius(col)
    local size = col & 0x3f
    if size == 0 or size > 0x24 then return nil end
    if not touchSizes then
        touchSizes = {byte(assert(memory.range(rom, 0x1ae2e, 0x24 * 2)), 1, 0x24 * 2)}
    end
    return touchSizes[size * 2 - 1], touchSizes[size * 2]
end

local function overlap(a, length, b, radius)
    local left = b - radius - a
    if left < 0 then return left + 2 * radius > 0 end
    return left <= length
end

local function bossState(v, slot, id, col, hp, status, routine, phase, flash, fzFlash, counter)
    if routine ~= 2 then return end
    local info = {label = bosses[id], slot = slot, hp = hp, max = 8, flash = flash}
    if id == 0x85 then
        info.flash = phase == 2 and fzFlash or 0
        if phase >= 0x0c then info.max = 1 end
        if phase == 0x0e then
            info.flash = col == 0 and (counter >> 16) or 0
        end
        if phase == 2 then
            info.state = (counter & 0x8000) == 0 and T("crusher", "пресс") or T("between attacks", "пауза атаки")
        elseif phase == 4 then
            info.state = T("plasma", "плазма")
        else
            info.state = phase >= 0x0c and T("escape", "побег") or T("transition", "переход")
        end
        if hp > 8 then info.hp = 0 end -- REV00 underflow during the crusher phase
    elseif status & 0x80 ~= 0 then
        info.state = T("defeated", "побеждён")
    else
        info.state = col ~= 0 and T("contact enabled", "принимает касания") or T("contact off", "касания отключены")
    end
    v.boss = info
end

local function scanSlot(v, slot, id, render, ...)
    if id == 0 then return end
    local col, hp, status, _, routine = select(31, ...) -- object offsets $20-$24
    if settings.bosses and bosses[id] and routine == 2 then
        local timerHi, timerLo, countHi, countLo, phase, fzFlash = select(47, ...)
        local flash = select(61, ...)
        local counter = (timerHi << 24) | (timerLo << 16) | (countHi << 8) | countLo
        bossState(v, slot, id, col, hp, status, routine, phase, flash, fzFlash, counter)
    end
    if not settings.objects or col == 0 or (render & 0x8c) ~= 0x84 then return end
    local rx, ry = touchRadius(col)
    if not rx then return end
    local xh, xl, _, _, yh, yl = select(7, ...)
    local x, y = signedWord(xh * 256 + xl), signedWord(yh * 256 + yl)
    if not visible(v, x - v.p.cx, y - v.p.cy, rx, ry) then return end
    v.visibleBoxes = v.visibleBoxes + 1
    if #v.boxes < boxLimit or slot == selectedSlot then
        v.boxes[#v.boxes + 1] = {slot = slot, x = x, y = y, rx = rx, ry = ry, col = col}
    end
end

local function scanBlock(v, slot, ...)
    scanSlot(v, slot, ...)
    scanSlot(v, slot + 1, select(65, ...))
    scanSlot(v, slot + 2, select(129, ...))
    scanSlot(v, slot + 3, select(193, ...))
end

local function readObjects(v)
    local zone = v.p.key >> 8
    local bossAct = (v.p.key & 0xff) == 2 and zone <= 5
    if not (settings.objects or (settings.bosses and bossAct)) then return end
    for start = 32, 96, 64 do
        local slots = math.min(128 - start, 64)
        local data = memory.range(ram, 0xd000 + start * 0x40, slots * 0x40)
        if data then
            for offset = 0, slots - 4, 4 do
                local first = offset * 0x40 + 1
                scanBlock(v, start + offset, byte(data, first, first + 0xff))
            end
        end
    end
end

local function inspectSlot(v)
    if not v then return T("Enter a level to inspect an object.", "Войдите в уровень для просмотра объекта.") end
    local address = 0xd000 + selectedSlot * 0x40
    local raw = selectedSlot == 0 and v.p.raw or memory.range(ram, address, 0x40)
    local id = raw and byte(raw, 1) or 0
    if id == 0 then return format(T("Slot %02X  $FF%04X  empty", "Слот %02X  $FF%04X  пуст"), selectedSlot, address) end
    local render = byte(raw, 2)
    local col, _, status, _, routine, _, _, _, subtype = byte(raw, 0x21, 0x29)
    local heading = format(T("Slot %02X  $FF%04X  ID %02X  %s", "Слот %02X  $FF%04X  ID %02X  %s"),
        selectedSlot, address, id, names[id] or T("Object", "Объект"))
    local x, y = signedWord(word(raw, 8)), signedWord(word(raw, 12))
    if (render & 0x0c) == 0 then y = signedWord(word(raw, 10)) end
    local line = format(T("X %d  Y %d  routine %02X  state %02X", "X %d  Y %d  подпрограмма %02X  флаги %02X"),
        x, y, routine, status)
    local tail = format(T("Subtype %02X  render %02X  collision %02X", "Подтип %02X  отрисовка %02X  коллизия %02X"),
        subtype, render, col)
    local rx, ry = touchRadius(col)
    if selectedSlot >= 32 and rx and (render & 0x0c) == 4 then
        local active = (render & 0x80) ~= 0
        local touching = active and overlap(v.p.x - 8, 16, x, rx) and overlap(v.p.reactY, v.p.reactH, y, ry)
        tail = tail .. "\n" .. format(T("%s  %dx%d  %s", "%s  %dx%d  %s"), touchType[col >> 6],
            rx * 2, ry * 2, touching and T("overlap", "пересечение")
            or (active and T("enabled", "включено") or T("offscreen", "за экраном")))
    end
    return heading .. "\n" .. line .. "\n" .. tail
end

-- The foreground layout in RAM indexes 256px chunks of 16px blocks.
-- REV00's six ROM collision indexes map block IDs to a signed profile.
local indexLengths = {[0x64a00] = 410, [0x64b9a] = 200, [0x64c62] = 400,
    [0x64df2] = 500, [0x64fe6] = 500, [0x651da] = 600}
local indexes, vertical, horizontal, profileBytes = {}, {}, {}, {}

local function profile(id, wall)
    local cache, base = vertical, 0x62a00
    if wall then cache, base = horizontal, 0x63a00 end
    if not cache[id] then
        local data = profileBytes[base]
        if not data then
            data = assert(memory.range(rom, base, 4096))
            profileBytes[base] = data
        end
        local first = id * 16 + 1
        local values = {byte(data, first, first + 15)}
        for i = 1, 16 do values[i] = signedByte(values[i]) end
        cache[id] = values
    end
    return cache[id]
end

local function tile(v, x, y)
    local key = ((y & 0x7f0) << 11) | (x & 0x7ff0)
    local known = v.tiles[key]
    if known ~= nil then return known or nil end
    local cell = ((y >> 1) & 0x380) + ((x >> 8) & 0x7f) + 1
    local chunk = v.chunks[cell]
    if not chunk then
        chunk = byte(v.layout, cell)
        v.chunks[cell] = chunk
    end
    local offset = 0xff00 -- REV00's no-chunk collision word
    if chunk ~= 0 then
        if chunk & 0x80 ~= 0 then
            chunk = chunk & 0x7f
            if v.p.looping then
                chunk = chunk + 1
                if chunk == 0x29 then chunk = 0x51 end
            end
        end
        offset = ((chunk - 1) * 0x200 + ((y & 0xf0) << 1) + ((x & 0xf0) >> 3)) & 0xffff
    end
    local block = memory.read(ram, offset, 2, "be")
    local id = block and (block & 0x7ff) or 0
    if not block or block & 0x6000 == 0 or id == 0 or id >= #v.index then
        v.tiles[key] = false
        return nil
    end
    local shape = byte(v.index, id + 1)
    if shape == 0 then
        v.tiles[key] = false
        return nil
    end
    local result = {shape = shape, xflip = block & 0x800 ~= 0, yflip = block & 0x1000 ~= 0,
        top = block & 0x2000 ~= 0, sides = block & 0x4000 ~= 0}
    v.tiles[key] = result
    return result
end

local function heightAt(block, cross, wall, reverse)
    local crossFlip, depthFlip = block.xflip, block.yflip
    if wall then crossFlip, depthFlip = block.yflip, block.xflip end
    local column = cross & 0xf
    if crossFlip then column = 15 - column end
    local height = profile(block.shape, wall)[column + 1]
    return depthFlip ~= reverse and -height or height
end

-- Re-evaluate the two grounded AnglePos probes at the displayed position.
-- The two-tile search and mirrored low nibble match FindFloor/FindWall.
local function probeTile(v, x, y, wall, reverse)
    local block = tile(v, x, y)
    if not block or not block.top then return 0 end
    local cross = wall and y or x
    return heightAt(block, cross, wall, reverse)
end

local function probe(v, x, y, dx, dy)
    local wall, reverse = dx ~= 0, dx < 0 or dy < 0
    local depth = (wall and x or y) & 0xf
    if reverse then depth = depth ~ 0xf end
    local height = probeTile(v, x, y, wall, reverse)
    local step
    if height == 16 or (height < 0 and height + depth < 0) then
        step = -16
    elseif height <= 0 then
        step = 16
    else
        return 15 - depth - height
    end
    local nextHeight = probeTile(v, x + dx * step, y + dy * step, wall, reverse)
    if nextHeight > 0 then return step + 15 - depth - nextHeight end
    if nextHeight < 0 and nextHeight + depth < 0 then return step - depth - 1 end
    if step < 0 then return step + 15 - depth end
    return nil
end

local function collisionMap(v)
    local index = memory.read(ram, 0xf796, 4, "be")
    local length = indexLengths[index]
    if not length then return false end
    indexes[index] = indexes[index] or memory.range(rom, index, length)
    v.index = indexes[index]
    v.layout = memory.range(ram, 0xa400, 0x400)
    v.tiles, v.chunks = {}, {}
    return v.layout and v.index
end

local function probesActive(p)
    return not p.air and not p.platform and p.routine == 2
end

local function terrainDirection(p)
    local mode = probesActive(p) and slopeMode(p.angle) or 0
    if mode == 0x40 then return -1, 0 end
    if mode == 0x80 then return 0, -1 end
    if mode == 0xc0 then return 1, 0 end
    return 0, 1
end

local function drawSensors(v)
    local p = v.p
    local dx, dy = terrainDirection(p)
    local footX, footY = p.x + dx * p.rh, p.y + dy * p.rh
    v.probes = {}
    for i = 1, 2 do
        local side = i == 1 and p.rw or -p.rw
        local x = footX + (dx == 0 and side or 0)
        local y = footY - (dy == 0 and side or 0)
        local distance = probe(v, x, y, dx, dy)
        v.probes[i] = distance and format("%+d", distance) or "--"
        local sx, sy = x - p.cx, y - p.cy
        local color = i == 1 and PAL.yellow or PAL.cyan
        local reach = distance or 20
        draw.line(sx, sy, sx + reach * dx, sy + reach * dy, color)
        draw.circle(sx, sy, 1.45, color)
        if distance and distance ~= 0 then
            draw.circle(sx + distance * dx, sy + distance * dy, 1.3, color, color)
        end
    end
end

local function drawTerrain(v)
    local p = v.p
    local dx, dy = terrainDirection(p)
    local ax, ay = (p.x + dx * p.rh) & ~0xf, (p.y + dy * p.rh) & ~0xf
    local lines, omitted = 0, 0
    local function contour(x, y, x2, y2, color)
        if lines >= terrainLimit then omitted = omitted + 1; return end
        lines = lines + 1
        draw.line(x - p.cx, y - p.cy, x2 - p.cx, y2 - p.cy, color)
    end
    for y = ay - 16, ay + 16, 16 do
        for x = ax - 16, ax + 16, 16 do
            local block = tile(v, x, y)
            if block and visible(v, x + 8 - p.cx, y + 8 - p.cy, 8, 8) then
                local color = block.top and (block.sides and 0x65edc8cc or 0xffdf90dd) or 0xdba8ffdd
                local start, previous, direction
                for column = 0, 16 do
                    local height = column < 16 and heightAt(block, column, false, false) or 0
                    local edge = height ~= 0 and (height > 0 and y + 16 - height or y - height) or nil
                    local sign = height > 0 and 1 or (height < 0 and -1 or 0)
                    if start and (edge ~= previous or direction ~= sign) then
                        contour(x + start, previous, x + column, previous, color)
                        if edge and sign == direction then contour(x + column, previous, x + column, edge, color) end
                        start = nil
                    end
                    if edge and not start then start = column end
                    previous, direction = edge, sign
                end
            end
        end
    end
    v.terrainOmitted = omitted
end

local function drawTrail(v)
    if not settings.trail then return end
    local p = v.p
    local first = math.max(1, historyCount - trailLimit * 4)
    for i = first, historyCount - 4, 4 do
        local a, b = historyPoint(i), historyPoint(i + 4)
        local x, y, x2, y2 = a.x - p.cx, a.y - p.cy, b.x - p.cx, b.y - p.cy
        local dx, dy = x - x2, y - y2
        if (dx ~= 0 or dy ~= 0) and -96 < dx and dx < 96 and -96 < dy and dy < 96
            and (visible(v, x, y, 1, 1) or visible(v, x2, y2, 1, 1)) then
            draw.line(x, y, x2, y2, 0x67e8ffc4)
        end
    end
end

local function comparison(v)
    v.ghost = nil
    if not attempt or not ghost then return nil end
    if v.p.key ~= ghost.key or attempt.key ~= ghost.key
        or attempt.x ~= ghost.x or attempt.y ~= ghost.y
        or attempt.sx ~= ghost.sx or attempt.sy ~= ghost.sy then
        return T("Ghost needs the same start position.", "Для сравнения начните с той же позиции.")
    end
    local elapsed = v.frame - attempt.start
    local point = ghost.points[elapsed // attemptStep + 1]
    local current = attempt.points[elapsed // attemptStep + 1]
    if not point or not current then
        return T("Ghost comparison ended.", "Сравнение с призраком завершено.")
    end
    v.ghost = point
    v.ghostDx, v.ghostDy = current.x - point.x, current.y - point.y
    return format(T("Ghost @ %df  dX %+.0f  dY %+.0f", "Призрак %d к.  ΔX %+.0f  ΔY %+.0f"),
        elapsed - elapsed % attemptStep, v.ghostDx, v.ghostDy)
end

local function bossText(boss)
    if not boss then return T("No active boss.", "Активного босса нет.") end
    return format(T("%s boss  HP %d/%d  flash %df  %s", "Босс %s  HP %d/%d  защита %d к.  %s"),
        boss.label, boss.hp, boss.max, boss.flash, boss.state)
end

local function drawOverlay(v)
    local p = v.p
    drawTrail(v)
    local sensors = settings.sensors and probesActive(p)
    if (settings.terrain or sensors) and collisionMap(v) then
        if settings.terrain then drawTerrain(v) end
        if sensors then drawSensors(v) end
    end
    for _, item in ipairs(v.boxes) do
        local color = item.slot == selectedSlot and PAL.white or touchColor[item.col >> 6]
        draw.rect(item.x - p.cx - item.rx, item.y - p.cy - item.ry,
            item.rx * 2, item.ry * 2, color)
    end
    local x, y = p.x - p.cx, p.y - p.cy
    if settings.objects and visible(v, x, y, p.rh, p.rh) then
        draw.rect(x - 8, p.reactY - p.cy, 16, p.reactH, PAL.cyan, 0x4fdbf126)
    end
    if settings.hud then
        local mode = slopeMode(p.angle)
        local rx, ry = p.rw, p.rh
        if not p.air and not p.platform and (mode == 0x40 or mode == 0xc0) then rx, ry = ry, rx end
        if visible(v, x, y, rx, ry) then
            draw.rect(x - rx, y - ry, rx * 2, ry * 2, 0x80d9faba)
        end
    end
    if v.ghost then
        local x, y = v.ghost.x - p.cx, v.ghost.y - p.cy
        if visible(v, x, y, 8, 12) then draw.rect(x - 8, y - 12, 16, 24, PAL.violet, 0xcb94f633) end
    end
    local rows = {}
    if settings.hud then
        rows[1] = format("X%d+$%04X Y%d+$%04X", p.x, p.sx, p.y, p.sy)
        rows[2] = format("VX%+.3f VY%+.3f", p.vx / 256, p.vy / 256)
        rows[3] = format(T("I%+.3f  angle $%02X %+.0f°", "I%+.3f  угол $%02X %+.0f°"),
            p.inertia / 256, p.angle, signedByte(p.angle) * 360 / 256)
        rows[4] = levelName(p.key) .. "  " .. motionState(p)
        local flags = (p.rolling and T("roll ", "клубок ") or "")
            .. (p.pushing and T("push ", "толчок ") or "")
            .. (p.underwater and T("water", "вода") or "")
        if flags ~= "" then rows[#rows + 1] = flags end
    end
    if v.probes then
        rows[#rows + 1] = format(T("Probes  A %s  B %s", "Зонды  A %s  B %s"), v.probes[1], v.probes[2])
    end
    if v.boss then
        local boss = v.boss
        rows[#rows + 1] = format(T("%s HP %d/%d  flash %df", "%s HP %d/%d  защита %dк"),
            boss.label, boss.hp, boss.max, boss.flash)
    end
    if v.ghost then
        rows[#rows + 1] = format(T("Ghost ΔX%+.0f ΔY%+.0f", "Призрак ΔX%+.0f ΔY%+.0f"),
            v.ghostDx, v.ghostDy)
    end
    if #rows == 0 then return end
    local extent = 0
    for _, line in ipairs(rows) do
        local length = utf8.len(line)
        if length > extent then extent = length end
    end
    -- The 14px HUD font uses a cell up to 14px wide. The short rows fit
    -- the minimum 400px view even for signed values at their limits.
    draw.rect(7, 7, extent * 14 + 12, #rows * 16 + 7, PAL.border, PAL.background, "hud")
    for i, line in ipairs(rows) do
        draw.text(13, 11 + (i - 1) * 16, line, i == 1 and PAL.cyan or PAL.white, "hud")
    end
end

local function readView(frame)
    if not verified then return nil end
    local p = readPlayer()
    if not p then return nil end
    local screen = emu.screen()
    return {p = p, frame = frame, width = screen.width, height = screen.height, boxes = {}, visibleBoxes = 0}
end

local panel = ui.window(T("Sonic 1 Lab  |  REV00", "Лаборатория Sonic 1  |  REV00"), {
    {type = "label", id = "status", text = T("Checking REV00. Advance frames to finish.",
        "Проверяется REV00. Продолжите эмуляцию для завершения.")},
    {type = "group", text = T("Overlays", "Оверлеи"), children = {
        {type = "row", children = {
            {type = "checkbox", id = "hud", text = T("Motion", "Движение"), value = settings.hud},
            {type = "checkbox", id = "sensors", text = T("Ground probes", "Зонды поверхности"), value = settings.sensors},
            {type = "checkbox", id = "terrain", text = T("Tile profiles", "Профили тайлов"), value = settings.terrain},
        }},
        {type = "row", children = {
            {type = "checkbox", id = "objects", text = T("Touch boxes", "Хитбоксы"), value = settings.objects},
            {type = "checkbox", id = "bosses", text = T("Boss", "Босс"), value = settings.bosses},
            {type = "checkbox", id = "trail", text = T("Trail", "След"), value = settings.trail},
            {type = "checkbox", id = "speed", text = T("Speed graph", "График скорости"), value = settings.speed},
        }},
        {type = "label", id = "limits", text = ""},
    }},
    {type = "group", text = T("Object slot", "Слот объекта"), children = {
        {type = "row", children = {
            {type = "number", id = "slot", text = T("Slot (decimal)", "Слот (десятичный)"), min = 0, max = 127, step = 1, value = 0},
            {type = "button", id = "prev", text = T("Previous active", "Предыдущий активный")},
            {type = "button", id = "next", text = T("Next active", "Следующий активный")},
        }},
        {type = "label", id = "object", text = ""},
        {type = "label", id = "boss", text = ""},
    }},
    {type = "graph", id = "speedPlot", text = T("Speed |V| (px/frame)", "Скорость |V| (пикс./кадр)")},
    {type = "group", text = T("Attempt comparison", "Сравнение попыток"), children = {
        {type = "row", children = {
            {type = "button", id = "record", text = T("Start attempt", "Начать попытку")},
            {type = "button", id = "keep", text = T("Keep as ghost", "Сохранить как призрак")},
            {type = "button", id = "clear", text = T("Clear ghost", "Очистить призрак")},
        }},
        {type = "label", id = "ghostStatus", text = ""},
    }},
})

local function refresh(v)
    if not panel then return end
    if not verified then
        ui.set(panel, "status", format(T("Checking REV00 (%d%%). Resume to finish.",
            "Проверяется REV00 (%d%%). Продолжите эмуляцию."), romOffset * 100 // 0x80000))
    elseif v then
        ui.set(panel, "status", format(T("REV00  |  %s  |  Frame %d", "REV00  |  %s  |  Кадр %d"),
            levelName(v.p.key), v.frame))
    else
        ui.set(panel, "status", T("REV00 verified. Enter a level.", "REV00 подтверждена. Войдите в уровень."))
    end
    ui.set(panel, "object", inspectSlot(v))
    ui.set(panel, "boss", settings.bosses and v and bossText(v.boss) or "")
    local limits = ""
    if v and settings.objects then
        limits = format(T("Touch boxes %d/%d on screen", "Хитбоксы %d/%d на экране"), #v.boxes, v.visibleBoxes)
    end
    if v and settings.terrain and v.terrainOmitted and v.terrainOmitted > 0 then
        limits = limits .. (limits ~= "" and "   |   " or "")
            .. format(T("Tile profile segments clipped %d", "Скрыто сегментов профиля: %d"), v.terrainOmitted)
    end
    ui.set(panel, "limits", limits)
    local points = {}
    if v and settings.speed then
        for i = 1, historyCount do
            local point = historyPoint(i)
            if point.speed then points[#points + 1] = point.speed end
        end
    end
    ui.set(panel, "speedPlot", points)
    ui.enable(panel, "record", v ~= nil and v.p.mode == 0x0c and v.p.routine == 2)
    ui.enable(panel, "keep", attempt ~= nil and #attempt.points >= 2)
    ui.enable(panel, "clear", ghost ~= nil)
    local progress
    if attempt then
        local frames = (#attempt.points - 1) * attemptStep
        progress = format(T("Attempt %df  (%d/%d samples)", "Попытка %d к.  (%d/%d замеров)"),
            frames, #attempt.points, attemptLimit)
        if v and v.comparison then progress = progress .. "\n" .. v.comparison end
    elseif ghost then
        progress = format(T("Ghost ready: %s at X %d  Y %d", "Призрак готов: %s, X %d  Y %d"),
            levelName(ghost.key), ghost.x, ghost.y)
    else
        progress = T("Start at a checkpoint. Keep a run, then retry from the same position.",
            "Начните с чекпойнта. Сохраните попытку и повторите с той же позиции.")
    end
    ui.set(panel, "ghostStatus", progress)
end

local function stepSlot(direction)
    local candidate = selectedSlot
    -- A user action walks the fixed slot table without retaining a stale list.
    for _ = 1, 128 do
        candidate = (candidate + direction) % 128
        local id = memory.read(ram, 0xd000 + candidate * 0x40)
        if id and id ~= 0 then return candidate end
    end
    return selectedSlot
end

if panel then
    ui.on(panel, function(event)
        if event.event == "close" then
            panel, attempt, ghost, settings.speed = nil, nil, nil, false
            if not settings.trail then resetHistory() end
            return
        end
        local control = event.control
        if settings[control] ~= nil then
            settings[control] = event.value
            if not settings.speed and not settings.trail then resetHistory() end
        elseif control == "slot" then
            local slot = math.floor(event.value)
            if validSlot(slot) then selectedSlot = slot end
            ui.set(panel, "slot", selectedSlot)
        elseif (control == "prev" or control == "next") and view then
            selectedSlot = stepSlot(control == "prev" and -1 or 1)
            ui.set(panel, "slot", selectedSlot)
        elseif control == "record" and view and view.p.mode == 0x0c and view.p.routine == 2 then
            local p = view.p
            attempt = {key = p.key, start = view.frame, x = p.x, y = p.y,
                sx = p.sx, sy = p.sy, points = {attemptPoint(p)}}
        elseif control == "keep" and attempt and #attempt.points >= 2 then
            ghost, attempt = attempt, nil
        elseif control == "clear" then
            ghost = nil
        end
        if view then view.comparison = comparison(view) end
        refresh(view)
    end)
    refresh(nil)
else
    print(T("Sonic 1 Lab panel unavailable. Default overlays remain active.",
        "Панель Sonic 1 недоступна. Стандартные оверлеи активны."))
end

local function renderView(v)
    if not v then return end
    v.comparison = comparison(v)
    readObjects(v)
    drawOverlay(v)
end

local function frame(event)
    if not verified then
        verifyPage()
        if verified or romOffset % 0x8000 == 0 then refresh(nil) end
        return
    end
    local v = readView(event.frame)
    local key = v and (v.p.key * 256 + v.p.mode)
    if key ~= level or (lastFrame and event.frame <= lastFrame) then
        resetHistory()
        attempt = nil
        level, refreshPanel = key, true
    end
    lastFrame, view = event.frame, v
    if v then
        if (settings.speed or settings.trail) and event.frame % 2 == 0 then
            pushHistory(event.frame, v.p)
        end
        if attempt and #attempt.points < attemptLimit then
            local elapsed = event.frame - attempt.start
            if elapsed >= attemptStep and elapsed % attemptStep == 0 then
                attempt.points[#attempt.points + 1] = attemptPoint(v.p)
            end
        end
        renderView(v)
    end
    if refreshPanel or event.frame % 8 == 0 then
        refresh(v)
        refreshPanel = false
    end
end

local function redraw(event)
    view = readView(event.frame)
    renderView(view)
end

local function restored(event)
    if event.event == "rewind" then
        trimHistory(event.frame)
        trimAttempt(event.frame)
    else
        resetHistory()
        attempt = nil
    end
    lastFrame, refreshPanel = event.frame, true
    view = readView(event.frame)
    local key = view and (view.p.key * 256 + view.p.mode)
    if key ~= level then
        resetHistory()
        attempt = nil
    end
    level = key
    renderView(view)
end

verifyPage()
emu.on("frame", frame)
emu.on("draw", redraw)
emu.on("reset", restored)
emu.on("loadstate", restored)
emu.on("rewind", restored)
