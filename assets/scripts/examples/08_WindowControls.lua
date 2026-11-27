-- Combine a native tool window with a live overlay and a bounded graph.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local panel, why = ui.window(text("Window controls", "Управление из окна"), {
    {type = "row", children = {
        {type = "button", id = "pause", text = text("Pause", "Пауза")},
        {type = "button", id = "resume", text = text("Resume", "Продолжить")},
        {type = "button", id = "clear", text = text("Clear graph", "Очистить график")}
    }},
    {type = "group", text = text("Overlay", "Оверлей"), children = {
        {type = "checkbox", id = "hud", text = text("Show in-game label", "Надпись в игре"), value = true},
        {type = "select", id = "color", text = text("Text color", "Цвет текста"),
            items = {text("Turquoise", "Бирюзовый"), text("White", "Белый"), text("Yellow", "Жёлтый")}}
    }},
    {type = "number", id = "period", text = text("Frames per sample", "Кадров на замер"), min = 1, max = 120, step = 1, value = 60},
    {type = "text", id = "note", text = text("Note", "Заметка")},
    {type = "graph", id = "activity", text = text("Frames with input poll (%)", "Кадры с опросом ввода (%)")},
    {type = "label", id = "status", text = text("Waiting for completed frames", "Ожидание завершённых кадров")}
})
if not panel then print(why); return end

local points, elapsed, polls = {}, 0, 0
local period, showHud, color = 60, true, 1
local colors = {0x7fffd4ff, 0xffffffff, 0xffdf60ff}
local estimate, dirty, refreshAt = nil, false, 0

local function clear()
    points, elapsed, polls, estimate = {}, 0, 0, nil
    dirty, refreshAt = true, 0
end

local function render()
    if not panel or not showHud then return end
    local label = estimate and string.format(text("Input polls: %.1f%%", "Опрос ввода: %.1f%%"), estimate)
        or text("Waiting for polls", "Ожидание опроса")
    draw.rect(10, 10, utf8.len(label) * 14 + 14, 23, 0x51bcafdf, 0x09121ddd, "hud")
    draw.text(17, 13, label, colors[color], "hud")
end

ui.on(panel, function(e)
    if e.event == "close" then
        panel = nil
        return
    end
    if e.control == "pause" or e.control == "resume" then
        local ok, reason = emu.pause(e.control == "pause")
        ui.set(panel, "status", reason or (e.control == "pause"
            and text("Pause requested", "Запрошена пауза")
            or text("Resume requested", "Запрошено продолжение")))
    elseif e.control == "clear" or e.control == "period" then
        if e.control == "period" then
            period = math.floor(e.value)
            ui.set(panel, "period", period)
        end
        clear()
        ui.set(panel, "activity", points)
    elseif e.control == "hud" then
        showHud = e.value
    elseif e.control == "color" then
        color = e.value
    end
end)

emu.on("frame", function(e)
    if not panel then return end
    local lagged = emu.lagged()
    if lagged ~= nil then
        elapsed = elapsed + 1
        if not lagged then polls = polls + 1 end
        if elapsed == period then
            estimate = 100 * polls / elapsed
            if #points == 96 then table.remove(points, 1) end
            points[#points + 1] = estimate
            elapsed, polls, dirty = 0, 0, true
        end
    end
    if e.frame >= refreshAt then
        if dirty then
            ui.set(panel, "activity", points)
            dirty = false
        end
        local label = lagged == nil and text("Input-poll metrics unavailable", "Данные опроса ввода недоступны")
            or string.format(text("Frame %d · Refresh %.2f Hz", "Кадр %d · Частота %.2f Гц"), e.frame, emu.fps())
        ui.set(panel, "status", label)
        refreshAt = e.frame + 8
    end
    render()
end)

for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function()
        clear()
        render()
    end)
end
emu.on("draw", render)
