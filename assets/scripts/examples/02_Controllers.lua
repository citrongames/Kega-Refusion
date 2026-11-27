-- Display the controls actually used for each completed frame.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local names = {
    up = "↑", down = "↓", left = "←", right = "→",
    button = "•", button1 = "1", button2 = "2", start = "S", pause = "P",
    trigger = "T", reload = "R", mode = "M", auxA = "A", auxB = "B",
    middle = "M", wheelDown = "W"
}
local rows = {}

local function render()
    if #rows == 0 then
        local label = text("Waiting for input", "Ожидание ввода")
        draw.rect(10, 38, utf8.len(label) * 14 + 14, 23, 0x718592c0, 0x09121ddd, "hud")
        draw.text(17, 41, label, 0xffffffff, "hud")
        return
    end
    for i, row in ipairs(rows) do
        local y = 38 + (i - 1) * 23
        draw.rect(10, y, utf8.len(row) * 14 + 14, 22, 0x718592c0, 0x09121ddd, "hud")
        draw.text(17, y + 3, row, 0xffffffff, "hud")
    end
end

emu.on("frame", function()
    -- Device descriptors include adapter players and the second Game Gear.
    local connected = input.devices()
    for i, device in ipairs(connected) do
        local controls = input.get(device.player)
        local active, axes = {}, ""
        if controls then
            for _, button in ipairs(device.buttons) do
                if controls[button] == true then
                    local name = names[button]
                    if device.kind == "mouse" and (button == "left" or button == "right") then
                        name = string.upper(string.sub(button, 1, 1))
                    end
                    active[#active + 1] = name or string.upper(button)
                end
            end
            -- MD6 uses x/y for boolean buttons; analog devices use numeric axes.
            if type(controls.x) == "number" then
                local positions = type(controls.y) == "number" and string.format("%d,%d", controls.x, controls.y)
                    or tostring(controls.x)
                axes = " " .. (controls.offscreen and text("offscreen", "вне кадра") or positions)
            end
        end
        rows[i] = string.format("P%d %s %s%s", device.player, device.kind,
            #active > 0 and table.concat(active) or "—", axes)
    end
    for i = #rows, #connected + 1, -1 do rows[i] = nil end
    render()
end)
emu.on("draw", render)
for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function()
        rows = {}
        render()
    end)
end
