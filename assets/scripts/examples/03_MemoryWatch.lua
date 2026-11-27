-- Read a physical domain and optionally add that address to native RAM Watch.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local domains, names, firstWritable = {}, {}, nil
for side = 0, emu.instances() - 1 do
    for _, domain in ipairs(memory.domains(side)) do
        if #names == 32 then break end -- Maximum options in a native selector.
        domain.side = side
        domains[#domains + 1] = domain
        names[#names + 1] = string.format("%s · %s", side == 0 and "A" or "B", domain.name)
        if not firstWritable and domain.writable then firstWritable = #domains end
    end
end
if #names == 0 then
    names[1] = text("No domains available", "Нет доступных областей")
end

local panel, why = ui.window(text("Memory watch", "Наблюдение за памятью"), {
    {type = "select", id = "domain", text = text("Physical domain", "Область памяти"),
        items = names, value = firstWritable or 1},
    {type = "text", id = "offset", text = text("Offset (e.g. 0x10)", "Смещение (например, 0x10)"), value = "0x0"},
    {type = "select", id = "width", text = text("Bytes", "Байты"), items = {"1", "2", "4"}, value = 1},
    {type = "checkbox", id = "hud", text = text("Show in-game value", "Показывать значение в игре"), value = true},
    {type = "row", children = {
        {type = "button", id = "pin", text = text("Add to RAM Watch", "Добавить в RAM Watch")},
        {type = "button", id = "remove", text = text("Remove watch", "Удалить наблюдение")}
    }},
    {type = "label", id = "value", text = "—"},
    {type = "label", id = "status", text = ""}
})
if not panel then print(why); return end

local domain, offset, width, watch
local showHud, refreshAt = true, 0
local valueLabel, hudLabel

local function selection()
    domain = domains[ui.get(panel, "domain")]
    offset = math.tointeger(tonumber(ui.get(panel, "offset")) or -1)
    if offset and (offset < 0 or offset > 0xffffffff) then offset = nil end
    width = 1 << (ui.get(panel, "width") - 1)
    showHud = ui.get(panel, "hud")
end

local function sample()
    if not domain or not offset then
        hudLabel = text("RAM: no valid address", "Память: неверный адрес")
        valueLabel = domain and text("Enter a nonnegative integer offset.", "Введите целое неотрицательное смещение.")
            or names[1]
        return
    end
    local value, reason = memory.read(domain.id, offset, width, "native", false, domain.side)
    hudLabel = value and string.format("RAM [0x%X] = 0x%0" .. width * 2 .. "X", offset, value)
        or text("RAM: unavailable", "Память: недоступна")
    valueLabel = value and hudLabel or reason
end

local function render()
    if not panel or not showHud then return end
    draw.rect(10, 66, utf8.len(hudLabel) * 14 + 14, 23, 0x718592c0, 0x09121ddd, "hud")
    draw.text(17, 69, hudLabel, 0xffffffff, "hud")
end

selection()
sample()
ui.set(panel, "value", valueLabel)
ui.enable(panel, "pin", domain ~= nil)
ui.enable(panel, "remove", false)
ui.on(panel, function(e)
    if e.event == "close" then
        if watch then memory.unwatch(watch) end
        panel = nil
        return
    end
    if e.event == "change" then
        selection()
        sample()
        ui.set(panel, "value", valueLabel)
        ui.enable(panel, "pin", domain ~= nil and offset ~= nil)
    elseif e.control == "pin" then
        local nextWatch, reason = memory.watch(domain.id, offset, width, "Lua sample", domain.side)
        if nextWatch then
            if watch then memory.unwatch(watch) end
            watch = nextWatch
            ui.enable(panel, "remove", true)
        end
        ui.set(panel, "status", reason or text("Watch added to RAM Watch.", "Запись добавлена в RAM Watch."))
    elseif e.control == "remove" then
        local ok, reason = memory.unwatch(watch)
        if ok then
            watch = nil
            ui.enable(panel, "remove", false)
        end
        ui.set(panel, "status", reason or text("Watch removed.", "Запись удалена."))
    end
end)

emu.on("frame", function(e)
    if not panel then return end
    if e.frame >= refreshAt then
        sample()
        ui.set(panel, "value", valueLabel)
        refreshAt = e.frame + 8
    end
    render()
end)
emu.on("draw", render)
for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function()
        if not panel then return end
        refreshAt = 0
        sample()
        render()
    end)
end
