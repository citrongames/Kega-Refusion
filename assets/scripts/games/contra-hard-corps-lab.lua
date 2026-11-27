-- Contra: Hard Corps (USA, Korea), GM T-95106-00, CRC32 C579F45E.
-- Read-only combat inspection. Addresses come from the cartridge's 68000
-- player update ($3200), shot pools ($5104), and rectangle test ($7EC0).
local russian = emu.language == "ru"
local function T(en, ru)
    return russian and ru or en
end

local ram, rom
for _, domain in ipairs(memory.domains()) do
    if domain.id == 1 and domain.base == 0 and domain.size == 0x200000 then
        rom = domain.id
    elseif domain.id == 2 and domain.base == 0xff0000 and domain.size == 0x10000 then
        ram = domain.id
    end
end

local unsupported = T("Open the original Contra: Hard Corps (USA, Korea) cartridge.",
    "Откройте оригинальный картридж Contra: Hard Corps (USA, Korea).")
assert(rom and ram
    and memory.range(rom, 0x180, 14) == "GM T-95106-00 "
    and memory.range(rom, 0x3200, 8) == "\x43\xf8\xd7\x00\x61\x00\x00\x9e"
    and memory.range(rom, 0x5110, 4) == "\x47\xf8\xaf\xc0"
    and memory.range(rom, 0x7ec0, 8) == "\x02\x42\x00\xff\x08\x02\x00\x05"
    and memory.range(rom, 0x19bb4, 6) == "\x33\x7c\x01\x18\x00\x36",
    unsupported)

local actorBase, actorStride, actorCount = 0xb000, 0x80, 78
local pools = {
    {base = actorBase, stride = actorStride, count = actorCount},
    {base = 0xd800, stride = 0x40, count = 14},
    {base = 0xdc00, stride = 0x40, count = 14},
    {base = 0xd700, stride = 0x80, count = 2},
}
local byte, format = string.byte, string.format
local function word(data, offset)
    return data[offset + 1] * 256 + data[offset + 2]
end

local function s16(value)
    return ((value + 0x8000) & 0xffff) - 0x8000
end

local function fixed(data, offset)
    return s16(word(data, offset)) + word(data, offset + 2) / 65536
end

local function bytesAt(address, length)
    local data = memory.range(ram, address, length)
    if data then return {byte(data, 1, length)} end
end

-- Only types whose damage handler confirms this RAM field. Some controllers
-- reuse +$36 for unrelated data. Several damage routines keep the actor at HP 0.
local healthTypes = {
    [0x002d] = {limit = 0x118, name = T("Boss", "Босс")},
    [0x002e] = {limit = 0x14, cooldown = 0x48, name = T("Breakable", "Разрушаемый объект")},
    [0x0120] = {limit = 1, name = T("Walker", "Пехотинец")},
    [0x0121] = {limit = 1, name = T("Walker", "Пехотинец")},
    [0x0124] = {limit = 0x28, name = T("Turret", "Турель")},
    [0x0167] = {limit = 0x28, name = T("Turret", "Турель")},
    [0x02b9] = {limit = 0x28, name = T("Turret", "Турель")},
    [0x0185] = {limit = 0x96, accumulated = true, name = T("Boss component", "Часть босса")},
}
local enemyShots = {[0x0127] = true, [0x0128] = true, [0x0187] = true}
local character = {[0] = "Ray", "Sheena", "Fang", "Browny"}
local color = {
    text = 0xf1f6ffff, player = 0x64e6f5ff, protected = 0xd6e7ffa8,
    actor = 0xe6edf0c0, selected = 0xffffffff, solid = 0x85ecb6ed,
    shot = 0x62b9ffff, hurt = 0xffd16aff, attack = 0xff686dff,
    both = 0xff8fcfff, background = 0x09121de4,
}
local layers = {hud = true, players = true, actors = false, contacts = true, shots = true, solids = false}
local panel, hook, hookReason, hookFrame, contacts, truncated
local view, selectedPool, selectedSlot = nil, 1, 0
local checkpoint, practiceFrame, practiceNotice
local history, log, previousHealth = {}, {}, {}
local historyHead, historyCount, previousFrame = 0, 0, nil
local refreshNeeded = true

local function catchContact(event)
    if hookFrame ~= event.frame then
        hookFrame, contacts = event.frame, {}
    end
    local registers = memory.registers(event.cpu, event.instance)
    if not registers then return end
    local r = registers.values
    local flags = r.D2 & 0x30
    if flags == 0 then return end
    local address = r.A1 & 0xffffff
    local right, left = s16(r.D4), s16(r.D5)
    local bottom, top = s16(r.D6), s16(r.D7)
    if right < left or bottom < top then return end
    contacts[#contacts + 1] = {address = address, flags = flags,
        right = right, left = left, bottom = bottom, top = top}
end

local function clearContacts()
    hookFrame, contacts, truncated = nil, nil, false
end

local function updateHook()
    clearContacts()
    if layers.contacts then
        hook, hookReason = memory.on(1, "exec", 0x7ec0, nil, catchContact)
    elseif hook then
        local removed, reason = memory.off(hook)
        if removed then hook = nil end
        hookReason = reason
    end
end

local function storedRect(data, start)
    return {
        right = s16(word(data, start + 0x2e)), left = s16(word(data, start + 0x30)),
        bottom = s16(word(data, start + 0x32)), top = s16(word(data, start + 0x34)),
    }
end

local function readSlot(data, start, address, group)
    local id = word(data, start)
    if id == 0 or id >= 0x8000 then return end
    local object = {
        address = address, id = id, state = s16(word(data, start + 2)),
        x = fixed(data, start + 6), y = fixed(data, start + 0x0a),
        vx = fixed(data, start + 0x0e), vy = fixed(data, start + 0x12),
        flags = word(data, start + 0x24),
    }
    if group == 4 then
        object.invulnerable = (object.flags & 0x10) == 0
        object.invulnerability = s16(word(data, start + 0x58))
        object.rect = storedRect(data, start)
    elseif group == 1 then
        local kind = healthTypes[id]
        if kind and object.state ~= 0 then
            if kind.accumulated then
                object.hp = kind.limit - word(data, start + 0x70)
            else
                object.hp = s16(word(data, start + 0x36))
            end
            object.limit = kind.limit
            if kind.cooldown then object.cooldown = word(data, start + kind.cooldown) end
        elseif id == 0x0128 then
            object.lifetime = object.state
        end
        if object.flags & 0x80 ~= 0 then object.rect = storedRect(data, start) end
    else
        object.power = data[start + 0x2d + 1]
        if object.flags & 0x20 ~= 0 then object.rect = storedRect(data, start) end
    end
    return object
end

local function readPool(group, destination)
    local pool = pools[group]
    local blockSlots = 4096 // pool.stride
    for first = 0, pool.count - 1, blockSlots do
        local base = pool.base + first * pool.stride
        local count = math.min(blockSlots, pool.count - first)
        local data = bytesAt(base, count * pool.stride)
        if not data then return end
        for i = 0, count - 1 do
            local offset = i * pool.stride
            local object = readSlot(data, offset, base + offset, group)
            if object then
                object.slot = first + i
                destination[#destination + 1] = object
            end
        end
    end
end

local function namedSlot(list, index)
    for _, object in ipairs(list) do
        if object.slot == index then return object end
    end
end

local function pointerSet(address, limit)
    local data, pointers = bytesAt(address, limit * 2), {}
    if not data then return pointers end
    for offset = 0, limit * 2 - 2, 2 do
        local pointer = word(data, offset)
        if pointer == 0 then break end
        pointers[pointer] = true
    end
    return pointers
end

local function readView(frame)
    if not (panel or layers.hud or layers.players or layers.actors or layers.contacts or layers.shots or layers.solids) then
        return
    end
    local players = {}
    readPool(4, players)
    if #players == 0 then return end
    local scroll, shake = bytesAt(0xa0e8, 8), bytesAt(0xa1dc, 2)
    if not scroll or not shake then return end
    local v = {frame = frame, pools = {{}, {}, {}, players}, dy = 128 + s16(word(shake, 0)),
        cameraX = fixed(scroll, 0), cameraY = fixed(scroll, 4)}
    local screen = emu.screen()
    v.width, v.height = screen.width, screen.height
    if panel or layers.actors or layers.solids then
        readPool(1, v.pools[1])
    end
    if panel or layers.shots then
        readPool(2, v.pools[2])
        readPool(3, v.pools[3])
    end
    if layers.shots or (panel and (selectedPool == 2 or selectedPool == 3)) then
        v.shotPointers = pointerSet(0xafc0, 15)
    end
    if layers.solids or (panel and selectedPool == 1) then
        local tail = memory.read(ram, 0xa3a2, 2, "be")
        if tail and tail > 0xaea2 and tail <= 0xaea2 + actorCount * 2 then
            v.solidPointers = pointerSet(0xaea2, (tail - 0xaea2) // 2)
        end
    end
    return v
end

local function paintBox(v, box, stroke)
    local left, right = box.left - 128, box.right - 128
    local top, bottom = box.top - v.dy, box.bottom - v.dy
    if left < 0 then left = 0 end
    if right >= v.width then right = v.width - 1 end
    if top < 0 then top = 0 end
    if bottom >= v.height then bottom = v.height - 1 end
    if right < left or bottom < top then return false end
    draw.rect(left, top, right - left + 1, bottom - top + 1, stroke, (stroke & 0xffffff00) | 0x18)
    return true
end

local function marker(v, object, stroke)
    local x, y = object.x // 1 - 128, object.y // 1 - v.dy
    if x < 2 or x >= v.width - 2 or y < 2 or y >= v.height - 2 then return false end
    draw.line(x - 2, y, x + 2, y, stroke)
    draw.line(x, y - 2, x, y + 2, stroke)
    return true
end

local function caption(v, x, y, label, ink)
    local maxChars = (v.width - x - 6) // 8
    if maxChars < 2 then return end
    local chars = utf8.len(label) or #label
    if chars > maxChars then
        local cut = utf8.offset(label, maxChars)
        label = string.sub(label, 1, cut - 1) .. "…"
        chars = maxChars
    end
    draw.rect(x, y, chars * 8 + 6, 11, 0, color.background)
    draw.text(x + 3, y + 2, label, ink)
end

local function overlay(v)
    if not v then return end
    local actors, players = v.pools[1], v.pools[4]
    local selected = panel and namedSlot(v.pools[selectedPool], selectedSlot)
    if layers.actors then
        local shown = 0
        for _, object in ipairs(actors) do
            if object ~= selected and shown < 20 and marker(v, object, color.actor) then
                shown = shown + 1
            end
        end
    end
    if layers.solids and v.solidPointers then
        local shown = 0
        for _, object in ipairs(actors) do
            if shown < 20 and object.rect and v.solidPointers[object.address]
                and paintBox(v, object.rect, color.solid) then
                shown = shown + 1
            end
        end
    end
    if layers.contacts and hookFrame == v.frame and contacts then
        for _, box in ipairs(contacts) do
            local stroke = box.flags == 0x30 and color.both or (box.flags == 0x20 and color.attack or color.hurt)
            paintBox(v, box, stroke)
        end
    end
    if layers.shots and v.shotPointers then
        for group = 2, 3 do
            local shots = v.pools[group]
            for _, object in ipairs(shots) do
                if object.rect and v.shotPointers[object.address] then paintBox(v, object.rect, color.shot) end
            end
        end
    end
    if layers.players then
        for _, player in ipairs(players) do
            paintBox(v, player.rect, player.invulnerable and color.protected or color.player)
        end
    end
    if selected then marker(v, selected, color.selected) end
    if layers.hud then
        local y = 30
        for _, player in ipairs(players) do
            local label = format("P%d  X %.2f  Y %.2f", player.slot + 1, player.x - 128, player.y - v.dy)
            if player.invulnerability > 0 then
                label = label .. format(T("  I %d", "  Н %d"), player.invulnerability)
            end
            caption(v, 5, y, label, color.player)
            y = y + 11
        end
        for _, actor in ipairs(actors) do
            if actor.hp and (actor.id == 0x002d or actor.id == 0x0185)
                and actor.x >= 128 and actor.x < 128 + v.width then
                local label = actor.id == 0x0185 and T("Boss part %d to threshold", "Часть босса · до порога %d")
                    or T("Boss HP %d/%d", "HP босса %d/%d")
                caption(v, 5, y, format(label, math.max(0, actor.hp), actor.limit), color.hurt)
                y = y + 11
                break
            end
        end
        if truncated then
            caption(v, 5, y, T("Partial collision sample", "Часть коллизий пропущена"), color.hurt)
        end
    end
end

local function resetHistory()
    history, log, previousHealth = {}, {}, {}
    historyHead, historyCount, previousFrame = 0, 0, nil
end

local function sampleDamage(v)
    local current, total = {}, 0
    for _, object in ipairs(v.pools[1]) do
        if object.hp then
            local prior = previousHealth[object.slot]
            local lost = prior and prior.id == object.id and prior.hp - object.hp or 0
            if lost > 0 then
                total = total + lost
                log[#log + 1] = format("%d  #%02d  %04X  −%d", v.frame, object.slot, object.id, lost)
                if #log > 5 then table.remove(log, 1) end
            end
            current[object.slot] = object
        end
    end
    previousHealth = current
    historyHead = historyHead % 96 + 1
    historyCount = math.min(historyCount + 1, 96)
    history[historyHead] = total
end

local function slotDetails(v)
    local object = v and namedSlot(v.pools[selectedPool], selectedSlot)
    if not object then return T("Slot empty", "Слот пуст") end
    local name
    if selectedPool == 1 then
        local kind = healthTypes[object.id]
        name = kind and kind.name or (enemyShots[object.id] and T("Enemy projectile", "Снаряд врага") or T("Actor", "Объект"))
    elseif selectedPool == 4 then
        local c = memory.read(ram, 0xfa0d + object.slot * 0x20)
        name = character[c] or T("Player", "Игрок")
    else
        name = T("Player projectile", "Снаряд игрока")
    end
    local output = format(T("%s  ID %04X · $FF%04X · State %d", "%s  ID %04X · $FF%04X · Состояние %d"),
        name, object.id, object.address, object.state)
        .. format("\nX %.3f  Y %.3f   ΔX %+.3f  ΔY %+.3f", object.x - 128, object.y - v.dy, object.vx, object.vy)
    if object.hp then
        local hpLabel = healthTypes[object.id].accumulated and T("To damage threshold", "До порога урона") or "HP"
        output = output .. format("\n%s %d / %d", hpLabel, object.hp, object.limit)
        if object.cooldown then
            output = output .. format(T("  Hit cooldown %d", "  Пауза между попаданиями %d"), object.cooldown)
        end
    elseif selectedPool == 2 or selectedPool == 3 then
        local active = v.shotPointers and v.shotPointers[object.address]
        output = output .. format(T("\nDamage %d · %s", "\nУрон %d · %s"), object.power,
            active and T("In collision list", "В списке коллизий") or T("Outside collision list", "Вне списка коллизий"))
    elseif object.invulnerability then
        local vulnerable = object.invulnerable and T("Protected", "Защищён") or T("Vulnerable", "Уязвим")
        output = output .. format(T("\n%s · Invulnerability timer %d", "\n%s · Таймер неуязвимости %d"),
            vulnerable, object.invulnerability)
    elseif object.lifetime and object.lifetime > 0 then
        output = output .. format(T("\nLifetime %d updates", "\nВремя жизни %d тактов"), object.lifetime)
    end
    if selectedPool == 1 then
        if object.rect and v.solidPointers and v.solidPointers[object.address] then
            output = output .. T("\nSolid collision surface", "\nТвёрдая поверхность")
        end
        if layers.contacts and hookFrame == v.frame and contacts then
            local attacks, targets = 0, 0
            for _, box in ipairs(contacts) do
                if box.address == 0xff0000 + object.address then
                    if box.flags & 0x20 ~= 0 then attacks = attacks + 1 end
                    if box.flags & 0x10 ~= 0 then targets = targets + 1 end
                end
            end
            if attacks + targets > 0 then
                output = output .. format(T("\nCombat rectangles · Attack %d · Target %d",
                    "\nЗоны боя · Атака %d · Цель %d"), attacks, targets)
            end
        end
    end
    return output
end

local function refreshPanel()
    if not panel then return end
    local status = T("Waiting for gameplay", "Ожидание игрового уровня")
    if view then
        status = format(T("Camera X %.2f  Y %.2f · Actors %d · P1 shots %d · P2 shots %d",
            "Камера X %.2f  Y %.2f · Объекты %d · Снаряды И1 %d · И2 %d"),
            view.cameraX, view.cameraY, #view.pools[1], #view.pools[2], #view.pools[3])
        if layers.contacts then
            local count = hookFrame == view.frame and #contacts or 0
            status = status .. format(T("\nCollision tests %d%s", "\nПроверки коллизий %d%s"), count,
                truncated and T(" · sample truncated", " · выборка неполная") or "")
        end
    end
    if hookReason then status = status .. "\n" .. hookReason end
    ui.set(panel, "status", status)
    ui.set(panel, "details", slotDetails(view))
    ui.set(panel, "practice", practiceNotice or (practiceFrame
        and format(T("Practice point at frame %d", "Точка тренировки на кадре %d"), practiceFrame)
        or T("Capture a practice point during play.", "Запомните точку тренировки во время игры.")))
    ui.set(panel, "log", #log > 0 and table.concat(log, "\n") or T("No observed changes", "Изменений пока нет"))
    local points = {}
    for i = 1, historyCount do
        points[i] = history[(historyHead - historyCount + i - 1) % 96 + 1]
    end
    ui.set(panel, "damage", points)
    ui.enable(panel, "save", view ~= nil)
    ui.enable(panel, "retry", practiceFrame ~= nil)
end

local function capturePractice()
    if not view then return end
    local why
    if not checkpoint then checkpoint, why = savestate.create() end
    if not checkpoint then
        practiceNotice = why
        ui.set(panel, "practice", why or T("Practice capture unavailable", "Захват точки недоступен"))
        return
    end
    local frame = view.frame
    ui.set(panel, "practice", format(T("Practice capture requested for frame %d", "Запрошена точка для кадра %d"), frame))
    ui.enable(panel, "retry", true)
    refreshNeeded = true
    -- Native capture can spend the callback's remaining wall time.
    local saved, reason = savestate.save(checkpoint)
    if saved then practiceFrame = frame end
    practiceNotice = reason
end

local function selectNeighbor(direction)
    if not view then return end
    local list = view.pools[selectedPool]
    if #list == 0 then return end
    if direction > 0 then
        for _, object in ipairs(list) do
            if object.slot > selectedSlot then return object.slot end
        end
        return list[1].slot
    end
    for i = #list, 1, -1 do
        if list[i].slot < selectedSlot then return list[i].slot end
    end
    return list[#list].slot
end

local why
panel, why = ui.window(T("Contra: Hard Corps · Combat Lab", "Contra: Hard Corps · Боевой инспектор"), {
    {type = "label", id = "status", text = ""},
    {type = "group", text = T("Overlay", "Оверлей"), children = {
        {type = "row", children = {
            {type = "checkbox", id = "hud", text = "HUD", value = layers.hud},
            {type = "checkbox", id = "players", text = T("Players", "Игроки"), value = layers.players},
            {type = "checkbox", id = "actors", text = T("Object origins", "Опорные точки"), value = layers.actors},
        }},
        {type = "row", children = {
            {type = "checkbox", id = "contacts", text = T("Combat zones", "Зоны боя"), value = layers.contacts},
            {type = "checkbox", id = "shots", text = T("Player shots", "Снаряды игроков"), value = layers.shots},
            {type = "checkbox", id = "solids", text = T("Solid objects", "Твёрдые объекты"), value = layers.solids},
        }},
        {type = "label", id = "legend", text = T("Blue: shots · Red: attack · Gold: hurt · Pink: both · Green: solid",
            "Синий: снаряд · Красный: атака · Жёлтый: уязвимость · Розовый: обе · Зелёный: преграда")},
    }},
    {type = "group", text = T("Slot inspector", "Инспектор слотов"), children = {
        {type = "select", id = "pool", text = T("Pool", "Группа"), value = 1, items = {
            T("World objects", "Объекты уровня"), T("Player 1 shots", "Снаряды игрока 1"),
            T("Player 2 shots", "Снаряды игрока 2"), T("Players", "Игроки"),
        }},
        {type = "row", children = {
            {type = "number", id = "slot", text = T("Slot", "Слот"), min = 0, max = 77, step = 1},
            {type = "button", id = "prev", text = T("Previous active", "Предыдущий активный")},
            {type = "button", id = "next", text = T("Next active", "Следующий активный")},
        }},
        {type = "label", id = "details", text = ""},
    }},
    {type = "graph", id = "damage", text = T("Observed damage in mapped slots", "Урон в известных слотах")},
    {type = "label", id = "log", text = ""},
    {type = "row", children = {
        {type = "button", id = "save", text = T("Save practice point", "Запомнить точку")},
        {type = "button", id = "retry", text = T("Retry", "Повторить")},
    }},
    {type = "label", id = "practice", text = ""},
})

if panel then
    ui.on(panel, function(event)
        if event.event == "close" then
            panel = nil
            resetHistory()
            if checkpoint then
                savestate.free(checkpoint)
                checkpoint, practiceFrame, practiceNotice = nil, nil, nil
            end
            return
        end
        local control = event.control
        if layers[control] ~= nil then
            layers[control] = event.value
            if control == "contacts" then updateHook() end
        elseif control == "pool" then
            selectedPool, selectedSlot = event.value, 0
            ui.set(panel, "slot", selectedSlot)
        elseif control == "slot" then
            selectedSlot = math.min(math.floor(event.value), pools[selectedPool].count - 1)
            ui.set(panel, "slot", selectedSlot)
        elseif control == "prev" or control == "next" then
            selectedSlot = selectNeighbor(control == "prev" and -1 or 1) or selectedSlot
            ui.set(panel, "slot", selectedSlot)
        elseif control == "save" then
            capturePractice()
            return
        elseif control == "retry" then
            if checkpoint and practiceFrame then
                local ok, reason = savestate.load(checkpoint)
                practiceNotice = reason or (ok and T("Restoring practice point", "Восстановление точки"))
                if reason == "Save object is empty or unknown" then practiceFrame = nil end
            end
        end
        refreshPanel()
    end)
else
    print(why or T("Combat Lab window unavailable.", "Окно боевого инспектора недоступно."))
end

updateHook()
refreshPanel()

emu.on("frame", function(event)
    if previousFrame and event.frame <= previousFrame then
        resetHistory()
        if hookFrame ~= event.frame then clearContacts() end
    end
    previousFrame = event.frame
    local hadGame = view ~= nil
    view = readView(event.frame)
    if hadGame and not view then resetHistory() end
    if hook then
        local stats = memory.hookstats()
        truncated = stats and stats.truncated or false
    end
    if view and panel then sampleDamage(view) end
    overlay(view)
    if refreshNeeded or hadGame ~= (view ~= nil) or event.frame % 8 == 0 then
        refreshPanel()
        refreshNeeded = false
    end
end)

emu.on("draw", function(event)
    view = readView(event.frame)
    overlay(view)
end)

for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function(e)
        clearContacts()
        resetHistory()
        if event == "reset" then
            practiceFrame, practiceNotice = nil, nil
        elseif event == "loadstate" then
            practiceNotice = nil
        end
        refreshNeeded = true
        view = readView(e.frame)
        overlay(view)
    end)
end
