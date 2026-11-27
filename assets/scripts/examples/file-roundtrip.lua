-- General asynchronous file storage example: write a small report once,
-- then read it back. This does not record controls or change game memory.
-- Results belong to this script, below data/scripts-data.
local filename = "example.txt"
local payload = "Kega Lua file example\nNominal refresh: " .. tostring(emu.fps()) .. "\n"
local request, reason = storage.write(filename, payload)
assert(request, reason)
local stage, message = "write", "Saving example.txt"

local function render()
    draw.text(12, 12, message, 0xffffffff, "hud")
end

emu.on("frame", function()
    if request then
        local result, error = storage.result(request)
        if result ~= nil then
            request = nil
            if stage == "write" then
                stage, message = "read", "Reading example.txt"
                request, error = storage.read(filename, 0, 4096)
                if not request then message = error; print(error) end
            else
                assert(result == payload, "The file contents did not match")
                message = "File saved and read successfully"
                print(message)
            end
        elseif error ~= "pending" then
            request, message = nil, error
            print(error)
        end
    end
    render()
end)
emu.on("draw", render)
render()
