-- Try a 30-frame Right hold, then return to the temporary checkpoint.
-- A paused game needs normal frame steps or Resume to run the trial.
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local duration = 30
local waiting = text("Ready. Advance frames or resume after starting.", "После запуска продолжите игру или прокручивайте кадры.")
local panel, why = ui.window(text("Input sequence", "Пробный ввод"), {
    {type = "row", children = {
        {type = "button", id = "run", text = text("Try Right for 30 frames", "Удерживать «вправо» 30 кадров")},
        {type = "button", id = "cancel", text = text("Cancel and restore", "Отменить и восстановить")}
    }},
    {type = "label", id = "status", text = waiting}
})
if not panel then print(why); return end

local checkpoint, player
local elapsed, held = 0, 0
local phase, status = "idle", waiting
local dirty = false

local function publish()
    if not panel then return end
    ui.set(panel, "status", status)
    ui.enable(panel, "cancel", phase == "armed" or phase == "running")
    dirty = false
end

local function freeCheckpoint()
    if checkpoint then
        savestate.free(checkpoint)
        checkpoint = nil
    end
end

local function findPad()
    for _, device in ipairs(input.devices()) do
        for _, button in ipairs(device.buttons) do
            if button == "up" then return device.player end
        end
    end
end

local function begin()
    freeCheckpoint()
    local tape = movie.status()
    if tape.mode ~= "off" or tape.pending then
        status = text("Finish the input recording or playback first.", "Сначала завершите запись или воспроизведение.")
        publish()
        return
    end
    player = findPad()
    if not player then
        status = text("Connect a controller with a directional pad.", "Подключите контроллер с крестовиной.")
        publish()
        return
    end
    local reason
    checkpoint, reason = savestate.create()
    if not checkpoint then
        status = reason
        publish()
        return
    end
    elapsed, held = 0, 0
    phase, status = "armed", text("Trial ready. Advance frames or resume.", "Проба готова. Продолжите игру или прокручивайте кадры.")
    publish()
    -- Capture is the final native call here. Start the override at the next
    -- frame boundary, so capturing a large state does not delay input work.
    local ok
    ok, reason = savestate.save(checkpoint)
    if not ok then
        phase, status, dirty = "idle", reason, true
    end
end

local function restore(message)
    input.clear(player)
    local ok, reason = savestate.load(checkpoint)
    freeCheckpoint() -- The queued command retains the snapshot until it runs.
    phase, status = ok and "restoring" or "idle", reason or message
end

publish()
ui.on(panel, function(e)
    if e.event == "close" then
        panel = nil
        if phase == "running" then restore(text("Trial cancelled.", "Проба отменена.")) end
        freeCheckpoint()
        return
    end
    if e.control == "run" and phase == "idle" then
        begin()
        return
    elseif e.control == "cancel" and phase == "running" then
        restore(text("Trial cancelled. Checkpoint restored.", "Проба отменена. Состояние восстановлено."))
    elseif e.control == "cancel" and phase == "armed" then
        freeCheckpoint()
        phase, status = "idle", text("Trial cancelled before input.", "Проба отменена до ввода.")
    end
    publish()
end)

emu.on("beforeframe", function()
    if phase ~= "armed" then return end
    local tape = movie.status()
    if tape.mode ~= "off" or tape.pending then return end
    local ok, reason = input.set(player, {left = false, right = true})
    if ok then
        phase = "running"
    else
        phase, status, dirty = "idle", reason, true
    end
end)

emu.on("frame", function()
    if phase == "armed" then
        -- Read-only modes may skip beforeframe entirely.
        phase, status, dirty = "idle", text("Input override unavailable.", "Подмена ввода недоступна."), true
    end
    if phase ~= "running" then
        freeCheckpoint()
        if dirty then publish() end
        return
    end
    local tape = movie.status()
    if tape.mode ~= "off" or tape.pending then
        input.clear(player)
        freeCheckpoint()
        phase, status = "idle", text("Trial interrupted by an input movie.", "Проба прервана изменением записи.")
        publish()
        return
    end
    elapsed = elapsed + 1
    local controls = input.get(player)
    if controls and controls.right and not controls.left then held = held + 1 end
    status = string.format(text("P%d: Right applied in %d/%d elapsed frames (%d total)",
        "P%d: «вправо» в %d из %d пройденных кадров (всего %d)"), player, held, elapsed, duration)
    if elapsed == duration then
        restore(string.format(text("Right held: %d/%d. Checkpoint restored.",
            "Удержание «вправо»: %d/%d. Состояние восстановлено."), held, duration))
    end
    publish()
end)

for _, event in ipairs({"reset", "loadstate", "rewind"}) do
    emu.on(event, function(e)
        if phase == "restoring" and e.event == "loadstate" then
            phase = "idle"
        elseif phase == "armed" or phase == "running" then
            -- The emulator already released our overrides on this transition.
            dirty = true
            phase, status = "idle", text("Trial interrupted by a timeline change.", "Проба прервана изменением времени игры.")
        end
    end)
end
