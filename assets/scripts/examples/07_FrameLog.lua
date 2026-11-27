-- Record native inputs and save a small sidecar aligned by movie ID and frame.
-- Choose a fresh name, record and play for a while, then stop and save.
-- Replay with the same name. Polling measures input use, not how often the game updates.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local sampleFrames, maxSamples = 30, 120
local maxFrames = sampleFrames * maxSamples
local ready = text("Choose a fresh name to record. If paused, step or resume the game.",
    "Для записи выберите новое имя. На паузе прокручивайте кадры или продолжите игру.")
local panel, why = ui.window(text("Recording and input polls", "Запись и опрос ввода"), {
    {type = "text", id = "name", text = text("Movie name", "Имя записи"), value = "lua-frame-log"},
    {type = "row", children = {
        {type = "button", id = "record", text = text("Record from power-on", "Записать с включения")},
        {type = "button", id = "stop", text = text("Stop and save", "Остановить и сохранить")},
        {type = "button", id = "play", text = text("Play with samples", "Воспроизвести с графиком")}
    }},
    {type = "row", children = {
        {type = "button", id = "check", text = text("Check status", "Проверить состояние")},
        {type = "checkbox", id = "hud", text = text("Show in-game progress", "Ход записи в игре"), value = true}
    }},
    {type = "graph", id = "activity", text = text("Frames with input poll (%)", "Кадры с опросом ввода (%)")},
    {type = "label", id = "status", text = ready}
})
if not panel then print(why); return end

local phase = "idle"
local showHud = true
local name, identity, activePath, request
local samples = {}
local shown = -1
local lastFrame, seen, polls = 0, 0, 0
local triedAutoStop = false

local function announce(nextPhase, message)
    phase = nextPhase
    if not panel then return end
    local idle = phase == "idle"
    ui.set(panel, "status", message)
    ui.enable(panel, "name", idle)
    ui.enable(panel, "record", idle)
    ui.enable(panel, "play", idle)
    ui.enable(panel, "stop", phase == "recording" or phase == "replay"
        or phase == "retry-movie" or phase == "retry-data")
    local stop = phase == "replay" and text("Stop playback", "Остановить воспроизведение")
        or text("Stop and save", "Остановить и сохранить")
    if phase == "retry-movie" or phase == "retry-data" then
        stop = text("Retry save", "Повторить сохранение")
    end
    ui.set(panel, "stop", stop)
end

local function fail(reason)
    announce("idle", reason)
    print(reason)
end

local function cursor(frame)
    local lo, hi = 1, #samples
    while lo <= hi do
        local middle = (lo + hi) // 2
        if samples[middle].frame <= frame then lo = middle + 1 else hi = middle - 1 end
    end
    return hi
end

local function graph(count)
    if not panel or shown == count then return end
    local points = {}
    for i = 1, count do points[i] = samples[i].polls * 100 / sampleFrames end
    ui.set(panel, "activity", points)
    shown = count
end

local function validName(value)
    if #value == 0 or #value > 64 then return false end
    for i = 1, #value do
        local b = string.byte(value, i)
        local letter = (b >= 65 and b <= 90) or (b >= 97 and b <= 122)
        if not letter and not (b >= 48 and b <= 57) and (i == 1 or b ~= 45 and b ~= 95) then
            return false
        end
    end
    return true
end

local function fileName()
    return "poll-" .. name .. ".txt"
end

local function matchesName(path)
    local suffix = "/" .. name .. ".krm" -- Native movie paths use Qt's separators.
    return path and string.sub(path, -#suffix) == suffix
end

local function ownsMovie(status)
    return status.id == identity and status.path == activePath
end

local function hexId(value)
    if #value ~= 32 then return false end
    for i = 1, #value do
        local b = string.byte(value, i)
        if not (b >= 48 and b <= 57 or b >= 97 and b <= 102) then return false end
    end
    return true
end

local header = "KRPOLL1\n"
local rowBytes = 8  -- 4 frame digits, space, 2 poll digits, newline.

local function writeSidecar()
    local lines = {header, identity .. "\n"}
    for _, sample in ipairs(samples) do
        lines[#lines + 1] = string.format("%04d %02d\n", sample.frame, sample.polls)
    end
    local reason
    request, reason = storage.write(fileName(), table.concat(lines))
    if request then
        announce("writing", text("Saving measurements…", "Сохранение замеров…"))
    else
        announce("retry-data", reason)
        print(reason)
    end
end

local function digit(b) return b >= 48 and b <= 57 end

local function decode(data)
    local start = #header + 33
    local size = #data - start
    local count = size // rowBytes
    local id = string.sub(data, #header + 1, start - 1)
    if string.sub(data, 1, #header) ~= header or string.byte(data, start) ~= 10
        or not hexId(id) or size < 0 or size % rowBytes ~= 0 or count > maxSamples then
        return false
    end
    local loaded, previous = {}, 0
    for pos = start + 1, #data, rowBytes do
        local a, b, c, d, space, tens, ones, newline = string.byte(data, pos, pos + rowBytes - 1)
        if not (digit(a) and digit(b) and digit(c) and digit(d) and digit(tens) and digit(ones))
            or space ~= 32 or newline ~= 10 then
            return false
        end
        local frame = (a - 48) * 1000 + (b - 48) * 100 + (c - 48) * 10 + (d - 48)
        local hits = (tens - 48) * 10 + (ones - 48)
        if frame <= previous or frame % sampleFrames ~= 0 or frame > maxFrames or hits > sampleFrames then
            return false
        end
        loaded[#loaded + 1], previous = {frame = frame, polls = hits}, frame
    end
    samples, identity = loaded, id
    return true
end

local function stopMovie()
    local status = movie.status()
    if status.mode ~= "off" and not ownsMovie(status) then
        fail(text("The active recording changed.", "Активная запись изменилась."))
        return
    end
    local ok, reason = movie.stop()
    if not ok then
        announce(phase, reason)
        print(reason)
    elseif phase == "replay" then
        announce("idle", text("Playback stopped.", "Воспроизведение остановлено."))
    else
        announce("saving", text("Saving input recording…", "Сохранение записи ввода…"))
    end
end

local function start(action)
    local status = movie.status()
    if status.pending or status.mode ~= "off" then
        fail(text("Finish the current movie first.", "Сначала завершите текущую запись или воспроизведение."))
        return
    end
    name = ui.get(panel, "name")
    if not validName(name) then
        fail(text("Use 1–64 ASCII letters or digits. Dashes and underscores may follow.",
            "От 1 до 64 латинских букв или цифр. Далее допустимы дефис и подчёркивание."))
        return
    end
    samples, identity, activePath = {}, nil, nil
    shown, lastFrame, seen, polls = -1, 0, 0, 0
    triedAutoStop = false
    graph(0)
    if action == "record" then
        local ok, reason = movie.record(name .. ".krm")
        if not ok then fail(reason); return end
        announce("starting-record", text("Preparing input recording…", "Подготовка записи ввода…"))
    else
        local reason
        local limit = #header + 33 + maxSamples * rowBytes
        request, reason = storage.read(fileName(), 0, limit + 1)
        if not request then fail(reason); return end
        announce("reading", text("Loading measurements…", "Загрузка замеров…"))
    end
end

local function trim(frame)
    -- A rewind or load can branch a recording. Keep points through that position.
    local keep = cursor(frame)
    for i = #samples, keep + 1, -1 do samples[i] = nil end
    lastFrame, seen, polls = frame, 0, 0
end

local function measure(frame)
    if frame ~= lastFrame + 1 then
        trim(frame - 1)
        graph(#samples)
    end
    lastFrame = frame
    local lagged = emu.lagged()
    if lagged ~= nil then
        seen = seen + 1
        if not lagged then polls = polls + 1 end
    end
    if frame % sampleFrames == 0 then
        if seen == sampleFrames then
            samples[#samples + 1] = {frame = frame, polls = polls}
            graph(#samples)
        end
        seen, polls = 0, 0
    end
end

local function poll(completed)
    if phase == "idle" or phase == "retry-data" or phase == "retry-movie" then return end
    if request then
        local result, reason = storage.result(request)
        if reason == "pending" then return end
        request = nil
        if phase == "reading" then
            if not result then fail(reason); return end
            if not decode(result) then
                fail(text("Invalid input-poll measurements.", "Неверные данные опроса ввода."))
                return
            end
            local ok
            ok, reason = movie.play(name .. ".krm")
            if not ok then fail(reason); return end
            announce("starting-play", text("Preparing input playback…", "Подготовка воспроизведения…"))
        elseif not result then
            announce("retry-data", reason)
            print(reason)
        else
            local message = text("Recording and measurements saved", "Запись и замеры сохранены")
            announce("idle", message)
            print(message)
        end
        return
    end

    local status = movie.status()
    if status.pending then return end
    if phase == "starting-record" then
        if status.mode ~= "recording" or not status.id or not matchesName(status.path) then
            fail(status.error or text("Recording did not start.", "Запись не началась."))
            return
        end
        identity, activePath = status.id, status.path
        announce("recording", text("Recording input. Samples every 30 frames; auto-stop at 3,600.",
            "Идёт запись ввода. Замеры каждые 30 кадров, автостоп на 3600."))
        if not panel then stopMovie(); return end
    elseif phase == "starting-play" then
        if (status.mode ~= "playback" and status.mode ~= "finished") or not matchesName(status.path) then
            fail(status.error or text("Playback did not start.", "Воспроизведение не началось."))
            return
        end
        if status.id ~= identity or (#samples > 0 and samples[#samples].frame > status.length) then
            movie.stop()
            fail(text("Measurements belong to a different recording.", "Замеры относятся к другой записи."))
            return
        end
        activePath = status.path
        announce("replay", text("Recorded input-activity graph", "График опроса из записи"))
        if not panel then stopMovie(); return end
    end

    if phase == "recording" then
        if status.mode == "recording" and ownsMovie(status) then
            graph(#samples)
            if completed and status.frame > 0 and status.frame <= maxFrames then measure(status.frame) end
            if status.frame >= maxFrames and not triedAutoStop then
                triedAutoStop = true
                stopMovie()
            end
        elseif status.error then
            announce("retry-movie", status.error)
        elseif status.mode == "off" then
            writeSidecar()
        else
            fail(text("The active recording changed.", "Активная запись изменилась."))
        end
    elseif phase == "saving" then
        if status.error then
            announce("retry-movie", status.error)
        elseif status.mode == "off" then
            writeSidecar()
        end
    elseif phase == "replay" then
        if ownsMovie(status) then
            graph(cursor(status.frame))
        else
            announce("idle", text("Playback stopped.", "Воспроизведение остановлено."))
        end
    end
end

local function render()
    if not panel or not showHud or (phase ~= "recording" and phase ~= "replay") then return end
    local status = movie.status()
    if not ownsMovie(status) then return end
    local label = phase == "replay" and string.format(text("Play %d/%d", "Кадр %d/%d"), status.frame, status.length)
        or string.format(text("REC %d/%d", "Запись %d/%d"), status.frame, maxFrames)
    draw.rect(10, 10, utf8.len(label) * 14 + 14, 23, 0x51bcafdf, 0x09121ddd, "hud")
    draw.text(17, 13, label, 0xffffffff, "hud")
end

announce("idle", ready)
ui.on(panel, function(e)
    if e.event == "close" then
        panel = nil
        if phase == "reading" then
            phase, request = "idle", nil
        elseif phase == "recording" or phase == "replay" then
            stopMovie()
        elseif phase == "starting-record" or phase == "starting-play" then
            print(text("Movie preparation is pending. Advance the game or control the movie from Input.",
                "Подготовка ещё идёт. Продолжите игру или используйте меню «Ввод»."))
        end
        return
    end
    if e.control == "record" or e.control == "play" then
        start(e.control)
    elseif e.control == "stop" then
        if phase == "retry-data" then writeSidecar() else stopMovie() end
    elseif e.control == "check" then
        -- Tool events can poll an asynchronous result at a paused boundary.
        poll(false)
    elseif e.control == "hud" then
        showHud = e.value
    end
end)
emu.on("frame", function()
    poll(true)
    render()
end)
emu.on("draw", render)
for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function()
        if phase == "recording" then
            local status = movie.status()
            if ownsMovie(status) then
                trim(status.frame)
                shown = -1
                triedAutoStop = false
            end
        end
        render()
    end)
end
