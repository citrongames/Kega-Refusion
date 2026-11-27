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

-- Damage/defeat logic for the USA/Korea dispatcher at $1F32. Values are
-- big-endian words. Some bosses accumulate damage, and several multipart
-- encounters route hits into a controller or a shared work-RAM counter.
local healthTypes = {
    [0x0009] = {limit = 0x40}, -- $9AFC / $9B66
    [0x000f] = {limit = 0x14}, -- $16C90 / $16CE6
    [0x002a] = {limit = 0x14}, -- $16B52 / $16BA8
    [0x002d] = {limit = 0x118, name = T("Boss", "Босс")}, -- $19BB4 / $19C3A
    [0x002e] = {limit = 0x14, name = T("Breakable", "Разрушаемый объект")},
    [0x005b] = {limit = 0x14}, -- $1C216 / $1C274
    [0x0101] = {limit = 0x5dc}, -- $47298 / $47938
    [0x0107] = {limit = 0x320}, -- $48716 / $48938
    [0x0108] = {limit = 0x3e8}, -- $48A2E / $48BC6
    [0x0109] = {limit = 0x4b0}, -- $48C72 / $48F9E, hit input from $4900C
    [0x010e] = {limit = 1}, -- $49442 / $49570
    [0x0112] = {limit = 0x15e}, -- $49864 / $49A14
    [0x0116] = {limit = 0x708}, -- $49ECC / $4A2FC
    [0x0120] = {limit = 1, name = T("Walker", "Пехотинец")}, -- $4AE02 / $58722
    [0x0121] = {limit = 1, name = T("Walker", "Пехотинец")},
    [0x0124] = {limit = 0x28, name = T("Turret", "Турель")}, -- $4B2BA / $58722
    [0x0125] = {limit = 0x28}, -- $4B4AE / $58722
    [0x012f] = {limit = 0x32}, -- $4C476 / $58722
    [0x0131] = {limit = 0x320}, -- $4CF5A/$4D108 / $4D2F4
    [0x013c] = {limit = 0x0a}, -- $4E330 / $58722
    [0x0144] = {limit = 0xbb8}, -- $4F178 / $4F33C
    [0x0148] = {limit = 0xc8}, -- $4FC08 / $4FC2A
    [0x0149] = {limit = 0x320}, -- $4FE30 / $510B8
    [0x014a] = {limit = 0x96}, -- $50068 / $50092
    [0x014b] = {limit = 0x64}, -- $50200 / $50224
    [0x014c] = {limit = 0xc8}, -- $5037C / $50356
    [0x014d] = {limit = 0x28}, -- shares $0124 handler
    [0x014e] = {limit = 1}, -- $50634 / $58722
    [0x0150] = {}, -- shared $510B8, activation uses parent state
    [0x0157] = {limit = 0x64}, -- $522E0 / $52644
    [0x015e] = {limit = 3}, -- $528B2 / $52E40
    [0x0167] = {limit = 0x28, name = T("Turret", "Турель")},
    [0x016e] = {limit = 0x32}, -- $504C0 / $58722
    [0x0280] = {limit = 0x50}, -- $540C4 / $541AA
    [0x0281] = {limit = 0x0a}, -- $54210 / $5434A
    [0x0282] = {limit = 0x14}, -- $5446E / $54424
    [0x0283] = {limit = 0x16}, -- $5465C / $58722
    [0x0286] = {limit = 0x3e8}, -- $54ABA / $54D98
    [0x0289] = {limit = 8}, -- $55008 / $55146
    [0x028a] = {limit = 0x3e8}, -- $5527A / $55692 / $556C6
    [0x028f] = {field = 0xa240, limit = 0xfa0}, -- $55BFE / $55C68
    [0x0290] = {field = 0xa240}, -- $56156, shared with $028F or $0294
    [0x0293] = {limit = 0x400}, -- $566DC / $568C4
    [0x0294] = {field = 0xa240, limit = 0x244, minState = 2}, -- $56902 / $56978
    [0x0295] = {limit = 0x3e8}, -- $56E86 / $58722
    [0x0296] = {limit = 0x320}, -- $56C7A / $56CBE
    [0x029e] = {limit = 0x258}, -- $57610 / $578E2
    [0x02a1] = {limit = 0x320}, -- $5752E / $574E2
    [0x02a8] = {limit = 0x50}, -- $54066 shares $0280 damage
    [0x02b9] = {limit = 0x28, name = T("Turret", "Турель")},
    [0x02ba] = {limit = 1}, -- shares $0120 handler
    [0x02bb] = {limit = 1},
    [0x038e] = {limit = 9}, -- $2212A / $220CA
    [0x0399] = {limit = 0x30}, -- $22B1A / $22E74
    [0x03a5] = {limit = 0x280, minState = 2}, -- $2480A / $247BE
    [0x03a8] = {current = function(data, start)
        if word(data, start + 0x36) > 0 then return 0x8002, 0x480 end
    end}, -- $2378E / $24C48 / $24C68
    [0x03ad] = {limit = 0x280, minState = 2}, -- $25196 / $2514A
    [0x03b2] = {limit = 0x300, minState = 2}, -- $2585C / $25810
    [0x03b4] = {current = function(data, start)
        if word(data, start + 0x36) > 0 then return 0x8004, 0x580 end
    end}, -- $23794 / $25B7A / $25B9A
    [0x03b7] = {limit = 0x280, minState = 2}, -- $26124 / $260D8
}

-- This earlier engine family uses A0 to point at the actor. Its active
-- flags at +$40 replace the +$02 initialization check, and its shared
-- routines $391E8–$393FC subtract damage using unsigned comparisons.
local actorHealthLimits = {
    [0x080] = 0x050, [0x081] = 0x12c, [0x082] = 0x00a, [0x083] = 0x00a,
    [0x086] = 0x00a, [0x08a] = 0x0c8, [0x08c] = 0x8ca, [0x08e] = 0x2ee, [0x08f] = 0x8ca,
    [0x091] = 0x8ca, [0x093] = 0x032, [0x095] = 0x8ca, [0x097] = 0x41a,
    [0x09a] = 0x2ee, [0x09c] = 0x960, [0x0a8] = 0x032, [0x0a9] = 0x096,
    [0x0ab] = 0x258, [0x0ac] = 0x384, [0x0ad] = 0x4b0, [0x0b3] = 0x01e,
    [0x0b5] = 0x1c2, [0x0b6] = 0x1c2, [0x0b7] = 0x834, [0x0ba] = 0x014,
    [0x0bb] = 0x014, [0x0bc] = 0x014, [0x0bf] = 0x5dc, [0x0e1] = 0x00a,
    [0x0e3] = 0x00a, [0x0e5] = 0xbb8, [0x0f1] = 0x9c4, [0x0f8] = 0x1c2,
    [0x0f9] = 0x2ee,
}
local activeMasks = {[0x097] = 0x08, [0x09c] = 0x20,
    [0x0ab] = 0x40, [0x0b5] = 0x40, [0x0ba] = 0x10}
for id, limit in pairs(actorHealthLimits) do
    healthTypes[id] = {limit = limit, active = 0x40, activeMask = activeMasks[id], unsigned = id ~= 0x0ad}
end

healthTypes[0x087] = {active = 0x40, unsigned = true, current = function(data, start)
    if data[start + 0x6d] < 0xfe then return 0x36, 0x0a end
end}
healthTypes[0x088] = {active = 0x40, unsigned = true, current = function(data, start)
    if data[start + 0x6d] < 0xfe then
        if data[start + 0x43] == 8 then return 0x36 end
        return 0x36, 0x32
    end
end}
local limits089 = {[1] = 0x32, [2] = 0x64, [3] = 0xc8}
healthTypes[0x089] = {active = 0x40, unsigned = true, current = function(data, start)
    local model = data[start + 0x6d]
    if model < 0xfe then
        local limit = limits089[(model & 6) >> 1]
        if limit then return 0x36, limit end
    end
end} -- $2D5C4 indexed initialization and $392A4 subtraction

local limits084 = {[1] = 0x258, [2] = 0x12c, [3] = 0x258}
healthTypes[0x084] = {unsigned = true, current = function(data, start, state)
    if state == 0x10 and word(data, start + 4) == 8 then
        local limit = limits084[data[start + 0x47]]
        if limit then return 0x36, limit end
    end
end} -- $32FC0/$32FE6/$331DC

-- Counts the remaining successful mechanism strikes. The field starts at
-- five and the fight ends on underflow, so six events are needed initially.
healthTypes[0x01d9] = {field = 0x70, limit = 6, bias = 1,
    name = T("Mechanism", "Механизм")} -- $65CEC/$6612A/$662D6

-- These handlers add registered shot damage to the actor's counter. Strict
-- greater-than transitions include the extra point required by that branch.
local damageLimits = {
    [0x0185] = 0x096, -- $5A370 / $59C18
    [0x01b5] = 0x3e8, -- $63042 / $63066
    [0x01c0] = 0xc80, -- $645CE / $6454C
    [0x01d2] = 0x00f, -- $658C8 / $658F8
    [0x01de] = 0x7d1, -- $666AE / $667B4
    [0x01ed] = 0x7d0, -- $6B612 / $6B6EA
    [0x01fa] = 0x7d1, -- $6D1A0 / $6D286
    [0x030c] = 0x579, -- $6DB8A / $6DB3A
    [0x0314] = 0x2bd, -- $6E606 / $6E5B2
    [0x031c] = 0x259, -- $6EE86 / $6EE32
    [0x0324] = 0x5dd, -- $6F7DA / $6F79A
    [0x0331] = 0x5dd,
    [0x0333] = 0x2bd, -- $7050E / $704BA
    [0x0334] = 0x2bd,
    [0x0339] = 0x4b1, -- $70D40 / $70CEC
    [0x033b] = 0x353, -- $712F2 / $7129E
    [0x033e] = 0x7d1, -- $710F0 forwards damage / $71514 / $71540
    [0x0347] = 0x3e9, -- $71B4C / $71AF2
    [0x034e] = 0x0c9, -- $7284A / $72812
    [0x0367] = 0x4b1, -- $741A6 / $7414A
    [0x036c] = 0x2bd, -- $74AEA / $74A9A
    [0x0400] = 0x2bd, -- $76136 / $760E2
    [0x0405] = 0x2bd,
    [0x0410] = 0xbb8, -- $76DB4 / $76E44
    [0x0447] = 0x01f, -- $7ABC0 / $7AB86
}
for id, limit in pairs(damageLimits) do
    healthTypes[id] = {field = 0x70, limit = limit, accumulated = true}
end
healthTypes[0x0185].name = T("Boss component", "Часть босса")

healthTypes[0x018e] = {accumulated = true, current = function(data, start)
    local phase = data[start + 0x51] -- byte +$50
    if phase & 8 ~= 0 then return end
    local limit = phase & 2 == 0 and 0x578 or (phase & 4 == 0 and 0x3e8 or 0x64)
    return 0x70, limit
end} -- $5CD72/$5CDA4/$5CDCC, $0197 forwards hits via +$6A

local limits198 = {[4] = 0x9c4, [0x0a] = 0x3e9, [0x0c] = 0x708}
healthTypes[0x0198] = {accumulated = true, current = function(_, _, state)
    return 0x70, limits198[state]
end} -- $605B4/$61300/$6161E

healthTypes[0x0341] = {accumulated = true, current = function(data, start)
    return 0x70, data[start + 0x4f] == 0 and 0x320 or 0x8fc
end} -- $731CA/$73440/$73458/$73470

healthTypes[0x0375] = {field = 0x5e, limit = 0x1f40, accumulated = true} -- $74D68/$74D8C
healthTypes[0x037d] = {accumulated = true, current = function(_, _, state)
    if state == 2 then return 0x70, 0x320 end
    if state == 3 then return 0x70, 0x5dc end
end} -- $75C14/$75ED4/$75EEA

healthTypes[0x03a0] = {current = function(_, _, state)
    if state <= 0x0a then return 0x8000, 0x0d00 end
    if state <= 0x0c then return 0x8002, 0x0480 end
    if state <= 0x0e then return 0x8004, 0x0580 end
end} -- $23788/$238FA/$23982/$239D2

local sharedDamage = {field = 0x983c, limit = 0xbb8, accumulated = true}
healthTypes[0x0430] = sharedDamage
healthTypes[0x0433] = sharedDamage
healthTypes[0x0436] = sharedDamage -- $788F8 forwards hits, $78258 checks threshold

healthTypes[0x0440] = {accumulated = true, current = function(_, _, state)
    if state == 2 then return 0x70, 0x1f4 end
    if state >= 4 and state <= 9 then return 0x70, 0xb55 end
end} -- $7902A/$792A6/$792CA/$792EE/$79944
-- Collision proxies and parts whose callers forward the registered hits.
local proxyFields = {[0x097] = 0x7c, [0x09c] = 0x48}
local damageLinks = {
    [0x08d] = {0x54, 0x08c}, [0x090] = {0x54, 0x08f},
    [0x092] = {0x54, 0x091}, [0x096] = {0x54, 0x095},
    [0x0b4] = {0x54, 0x0b3}, [0x0197] = {0x6a, 0x018e},
    [0x033c] = {0x78, 0x033e}, [0x033d] = {0x78, 0x033e},
}
local enemyShots = {[0x0127] = true, [0x0128] = true, [0x0187] = true}
local character = {[0] = "Ray", "Sheena", "Fang", "Browny"}
local color = {
    text = 0xf1f6ffff, player = 0x64e6f5ff, protected = 0xd6e7ffa8,
    actor = 0xe6edf0c0, selected = 0xffffffff, solid = 0x85ecb6ed,
    shot = 0x62b9ffff, hurt = 0xffd16aff, attack = 0xff686dff,
    both = 0xff8fcfff, background = 0x09121de4, healthBackground = 0x09121dbf,
}
local layers = {hud = true, players = true, actors = false, contacts = true, shots = true, solids = false}
local panel, hook, hookReason, hookFrame, contacts, truncated
local view, selectedPool, selectedSlot = nil, 1, 0
local checkpoint, practiceFrame, practiceNotice
local previousFrame
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
    contacts[#contacts + 1] = {address = address, a0 = r.A0 & 0xffffff, flags = flags,
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

local function readHealth(data, start, object, shared)
    local kind = healthTypes[object.id]
    if not kind then return end
    if kind.active then
        local flags = data[start + kind.active + 1]
        if flags & (kind.activeMask or 0x80) == 0 or flags & 1 ~= 0 then return end
    elseif object.state < (kind.minState or 1) then
        return
    end
    local field, limit = kind.field or 0x36, kind.limit
    if kind.current then field, limit = kind.current(data, start, object.state) end
    if not field or (kind.accumulated and not limit) then return end
    local value
    if field >= actorStride then
        value = shared[field]
        if value == nil then
            value = memory.read(ram, field, 2, "be") or false
            shared[field] = value
        end
        object.healthKey = field
    else
        value = word(data, start + field)
        object.healthKey = object.address + field
    end
    if value == false then return end
    if kind.accumulated then
        value = limit - value
    elseif not kind.unsigned then
        value = s16(value)
    end
    value = value + (kind.bias or 0)
    if kind.unsigned and limit and value > limit then return end
    object.hp = value >= 0 and value or 0
    object.limit = limit
end

local function readSlot(data, start, address, group, sharedHealth)
    local id = word(data, start)
    if id == 0 or id >= 0x8000 then return end
    local object = {
        address = address, id = id, state = s16(word(data, start + 2)),
        x = fixed(data, start + 6), y = fixed(data, start + 0x0a),
        flags = word(data, start + 0x24),
    }
    -- Velocities are only shown for the selected slot.
    if panel and group == selectedPool and address == pools[group].base + selectedSlot * pools[group].stride then
        object.vx, object.vy = fixed(data, start + 0x0e), fixed(data, start + 0x12)
    end
    if group == 4 then
        object.invulnerable = (object.flags & 0x10) == 0
        object.invulnerability = s16(word(data, start + 0x58))
        object.rect = storedRect(data, start)
    elseif group == 1 then
        if sharedHealth then readHealth(data, start, object, sharedHealth) end
        if layers.contacts then
            local proxy = proxyFields[id]
            if proxy and object.hp ~= nil then
                object.proxy = (word(data, start + proxy) & 0xff) * 65536 + word(data, start + proxy + 2)
            end
            local link = damageLinks[id]
            if link and (id ~= 0x0b4 or data[start + 0x6d] ~= 0)
                and (id ~= 0x0197 or data[start + 0x51] & 8 == 0) then
                object.damageOwner = (word(data, start + link[1]) & 0xff) * 65536 + word(data, start + link[1] + 2)
            end
        end
        if id == 0x0128 then object.lifetime = object.state end
        if object.flags & 0x80 ~= 0 then object.rect = storedRect(data, start) end
    else
        object.power = data[start + 0x2d + 1]
        if object.flags & 0x20 ~= 0 then object.rect = storedRect(data, start) end
    end
    return object
end

local function readPool(group, destination, actorAt)
    local pool = pools[group]
    local sharedHealth = group == 1 and (layers.contacts or layers.solids) and {} or nil
    local blockSlots = 4096 // pool.stride
    for first = 0, pool.count - 1, blockSlots do
        local base = pool.base + first * pool.stride
        local count = pool.count - first
        if count > blockSlots then count = blockSlots end
        local data = bytesAt(base, count * pool.stride)
        if not data then return end
        for i = 0, count - 1 do
            local offset = i * pool.stride
            local object = readSlot(data, offset, base + offset, group, sharedHealth)
            if object then
                object.slot = first + i
                destination[#destination + 1] = object
                if actorAt then actorAt[0xff0000 + object.address] = object end
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
    if panel or layers.actors or layers.contacts or layers.solids then
        if layers.contacts then v.actorAt = {} end
        readPool(1, v.pools[1], v.actorAt)
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
    return left, right, top
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

local function targetHealth(actorAt, box)
    local actor = actorAt[box.address]
    if actor then
        if actor.hp ~= nil then return actor end
        if actor.damageOwner then
            local owner = actorAt[actor.damageOwner]
            if owner and owner.id == damageLinks[actor.id][2] then return owner end
        end
    end
    local owner = actorAt[box.a0]
    if owner and owner.proxy == box.address then return owner end
end

local function overlay(v)
    if not v then return end
    local actors, players = v.pools[1], v.pools[4]
    local selected = panel and namedSlot(v.pools[selectedPool], selectedSlot)
    -- Reserve commands for HUD and selection. Under load the observed combat
    -- rectangles and their health take priority over solids and origin marks.
    local remaining = 128 - (selected and 2 or 0)
    local hudRows = layers.hud and (#players + (truncated and 1 or 0)) or 0
    remaining = remaining - 2 * hudRows
    if layers.players then remaining = remaining - #players end
    local labels, seen = {}, {}
    local function queueHealth(actor, left, right, top)
        if not actor or actor.hp == nil or seen[actor.healthKey] then return end
        seen[actor.healthKey] = true
        local label = actor.limit and (actor.hp .. "/" .. actor.limit) or ("" .. actor.hp)
        local width = #label * 8 + 4
        local x, y = (left + right - width + 1) // 2, top - 11
        if x > v.width - width then x = v.width - width end
        if x < 0 then x = 0 end
        if y < 0 then y = 0 end
        labels[#labels + 1] = {text = label, x = x, y = y, width = width}
        remaining = remaining - 2
    end

    if layers.contacts and hookFrame == v.frame and contacts then
        for _, box in ipairs(contacts) do
            if remaining < 3 then break end
            local stroke = box.flags == 0x30 and color.both or (box.flags == 0x20 and color.attack or color.hurt)
            local left, right, top = paintBox(v, box, stroke)
            if left then
                remaining = remaining - 1
                if box.flags & 0x10 ~= 0 then queueHealth(targetHealth(v.actorAt, box), left, right, top) end
            end
        end
    end
    -- Text uses a second native call. Keep space for RAM sampling and panel
    -- updates within the host's 192-call budget even with all layers enabled.
    local nativeSpare = 141 - (128 - remaining) - #labels - hudRows
    if nativeSpare < remaining then remaining = nativeSpare end
    if layers.shots and v.shotPointers then
        for group = 2, 3 do
            for _, object in ipairs(v.pools[group]) do
                if remaining < 1 then break end
                if object.rect and v.shotPointers[object.address] and paintBox(v, object.rect, color.shot) then
                    remaining = remaining - 1
                end
            end
        end
    end
    if layers.players then
        for _, player in ipairs(players) do
            paintBox(v, player.rect, player.invulnerable and color.protected or color.player)
        end
    end
    if layers.solids and v.solidPointers then
        local shown = 0
        for _, object in ipairs(actors) do
            if remaining < 1 or shown == 14 then break end
            if object.rect and v.solidPointers[object.address] then
                local needsLabel = object.hp ~= nil and not seen[object.healthKey]
                if not needsLabel or remaining >= 4 then
                    local left, right, top = paintBox(v, object.rect, color.solid)
                    if left then
                        remaining, shown = remaining - 1, shown + 1
                        if needsLabel then
                            queueHealth(object, left, right, top)
                            remaining = remaining - 1
                        end
                    end
                end
            end
        end
    end
    if layers.actors then
        local shown = 0
        for _, object in ipairs(actors) do
            if remaining < 2 or shown == 10 then break end
            if object ~= selected and marker(v, object, color.actor) then
                remaining, shown = remaining - 2, shown + 1
            end
        end
    end
    if selected then marker(v, selected, color.selected) end
    for _, label in ipairs(labels) do
        draw.rect(label.x, label.y, label.width, 10, 0, color.healthBackground)
        draw.text(label.x + 2, label.y + 1, label.text, color.text)
    end
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
        if truncated then
            caption(v, 5, y, T("Partial collision sample", "Часть коллизий пропущена"), color.hurt)
        end
    end
end

local function slotDetails(v)
    local object = v and namedSlot(v.pools[selectedPool], selectedSlot)
    if not object then return T("Slot empty", "Слот пуст") end
    if object.vx == nil then
        local motion = bytesAt(object.address + 0x0e, 8)
        if motion then object.vx, object.vy = fixed(motion, 0), fixed(motion, 4) end
    end
    local name
    if selectedPool == 1 then
        local kind = healthTypes[object.id]
        name = (kind and kind.name) or (enemyShots[object.id] and T("Enemy projectile", "Снаряд врага") or T("Actor", "Объект"))
    elseif selectedPool == 4 then
        local c = memory.read(ram, 0xfa0d + object.slot * 0x20)
        name = character[c] or T("Player", "Игрок")
    else
        name = T("Player projectile", "Снаряд игрока")
    end
    local output = format(T("%s  ID %04X · $FF%04X · State %d", "%s  ID %04X · $FF%04X · Состояние %d"),
        name, object.id, object.address, object.state)
        .. format("\nX %.3f  Y %.3f   ΔX %+.3f  ΔY %+.3f", object.x - 128, object.y - v.dy, (object.vx or 0), (object.vy or 0))
    if selectedPool == 2 or selectedPool == 3 then
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
        previousFrame = nil
        if hookFrame ~= event.frame then clearContacts() end
    end
    previousFrame = event.frame
    local hadGame = view ~= nil
    view = readView(event.frame)
    if hook then
        local stats = memory.hookstats()
        truncated = stats and stats.truncated or false
    end
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
        previousFrame = nil
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
