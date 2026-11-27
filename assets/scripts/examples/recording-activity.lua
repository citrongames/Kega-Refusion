-- Native input recording + a saved input-activity graph.
-- Change mode to "replay" to replay the SAME .krm with its measurements.
-- Pick a new recordingName for another recording; existing movies are preserved.
-- This estimates game activity from controller polls, NOT universal game FPS.
local mode = "record"
local recordingName = "lua-activity"
local seconds = 30 -- 0: keep recording until Input > Save and finish recording
local window, maxPoints, visible = 60, 4096, 48

local positions, values = {}, {}
local count, lastFrame, elapsed, active = 0, 0, 0, 0
local rate, identity
local filename = "activity-" .. recordingName .. ".txt"
local phase, message = "starting", "Preparing input recording"
local request, payload, encodeIndex, parseIndex
local loadedCount, cursor = 0, 0

local function findPoint(frame)
    local lo, hi = 1, count
    while lo <= hi do
        local mid = (lo + hi) // 2
        if positions[mid] <= frame then lo = mid + 1 else hi = mid - 1 end
    end
    return hi
end

local function render()
    draw.text(12, 12, message, 0xffffffff, "hud")
    if count == 0 or not rate then return end
    local current = mode == "replay" and findPoint(movie.status().frame) or count
    local first = math.max(1, current - visible + 1)
    draw.rect(12, 34, (visible - 1) * 4, 40, 0x779598aa, 0x101c24c0, "hud")
    for i = first + 1, current do
        local x = 12 + (i - first - 1) * 4
        draw.line(x, 74 - values[i - 1] / rate * 40,
                  x + 4, 74 - values[i] / rate * 40, 0x7fffd4ff, "hud")
    end
end

local function changedTimeline()
    if phase == "recording" then
        local frame = movie.status().frame
        count = findPoint(frame)
        lastFrame, elapsed, active = frame, 0, 0
    end
    render()
end

assert(mode == "record" or mode == "replay", "Choose record or replay")
assert(movie.status().mode == "off", "Finish the active input recording first")
if mode == "replay" then
    -- Parsing is cooperative. Pause explicitly until the saved graph is ready.
    assert(emu.pause(true))
    request = assert(storage.read(filename))
    phase, message = "reading", "Loading recorded measurements"
else
    assert(movie.record(recordingName))
end

emu.on("frame", function()
    local status = movie.status()
    if status.error and status.error ~= "" and not status.pending then
        if phase ~= "error" then print(status.error) end
        phase, message = "error", status.error
        render()
        return
    end

    if phase == "starting" and not status.pending then
        if status.mode == (mode == "record" and "recording" or "playback") or
           (mode == "replay" and status.mode == "finished") then
            if mode == "record" then
                identity, rate = status.id, status.fps
                assert(#identity == 32, "Unexpected recording identity")
                phase, message = "recording", "Measuring input activity"
                assert(emu.pause(false))
            else
                assert(status.id == identity, "Measurements belong to another recording")
                phase, message = "replay", "Recorded input-activity graph"
                if status.mode ~= "finished" then assert(emu.pause(false)) end
            end
        end
    end

    if phase == "recording" then
        if status.mode == "recording" and not status.pending then
            if status.frame ~= lastFrame + 1 then
                count = findPoint(status.frame)
                elapsed, active = 0, 0
            end
            lastFrame = status.frame
            local lagged = emu.lagged()
            if lagged ~= nil then
                elapsed = elapsed + 1
                if not lagged then active = active + 1 end
                if elapsed == window then
                    if count < maxPoints then
                        count = count + 1
                        positions[count] = status.frame
                        values[count] = active / elapsed * rate
                    end
                    elapsed, active = 0, 0
                end
            end
            if (seconds > 0 and status.frame >= seconds * rate) or count == maxPoints then
                assert(movie.stop())
            end
        elseif status.mode == "off" and not status.pending and (status.error == nil or status.error == "") then
            payload = "KRFPS1\n" .. identity .. "\n" .. string.format("%010.5f\n", rate)
            encodeIndex, phase, message = 1, "encoding", "Saving recorded measurements"
        end
    elseif phase == "encoding" then
        local block, size = {}, 0
        while encodeIndex <= count and size < 128 do
            size = size + 1
            block[size] = string.format("%010d\t%06.2f\n", positions[encodeIndex], values[encodeIndex])
            encodeIndex = encodeIndex + 1
        end
        payload = payload .. table.concat(block)
        if encodeIndex > count then
            request = assert(storage.write(filename, payload))
            payload, phase = nil, "writing"
        end
    elseif phase == "writing" or phase == "reading" then
        local result, reason = storage.result(request)
        if result ~= nil then
            request = nil
            if phase == "writing" then
                phase, message = "done", "Recording and measurements saved"
                print(message)
            else
                assert(string.sub(result, 1, 7) == "KRFPS1\n", "Unknown measurement format")
                identity = string.sub(result, 8, 39)
                assert(#identity == 32, "Invalid recording identity")
                rate = assert(tonumber(string.sub(result, 41, 50)))
                assert(rate > 0 and rate < 1000, "Invalid measurement refresh rate")
                assert((#result - 51) % 18 == 0, "Truncated measurement file")
                loadedCount = (#result - 51) // 18
                assert(loadedCount <= maxPoints, "Too many measurement points")
                payload, parseIndex, phase = result, 1, "parsing"
            end
        elseif reason ~= "pending" then
            error(reason)
        end
    elseif phase == "parsing" then
        for i = 1, 16 do
            if parseIndex > loadedCount then break end
            local start = 52 + (parseIndex - 1) * 18
            local frame = assert(tonumber(string.sub(payload, start, start + 9)))
            local value = assert(tonumber(string.sub(payload, start + 11, start + 16)))
            assert(frame >= 0 and (parseIndex == 1 or frame > positions[parseIndex - 1]),
                   "Invalid measurement order")
            assert(value >= 0 and value <= rate + 0.01, "Invalid measurement value")
            positions[parseIndex], values[parseIndex] = frame, value
            parseIndex = parseIndex + 1
        end
        if parseIndex > loadedCount then
            count, payload, phase = loadedCount, nil, "starting"
            message = "Preparing input playback"
            assert(movie.play(recordingName))
        end
    end
    render()
end)

emu.on("draw", render)
emu.on("loadstate", changedTimeline)
emu.on("rewind", changedTimeline)
emu.on("reset", changedTimeline)
render()

-- A paused asynchronous preparation needs actual frames for callbacks.
-- Advance only during preparation/parsing. Measurement files are read BEFORE
-- starting playback, so the recorded graph is available from its first frame.
while phase == "starting" or phase == "reading" or phase == "parsing" do
    assert(emu.frameadvance())
end
