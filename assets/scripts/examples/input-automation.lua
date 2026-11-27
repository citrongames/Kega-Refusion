-- Run in a loaded game with a directional pad. Pause first to observe one step
-- from the restored checkpoint after the trial.
local player
for _, device in ipairs(input.devices()) do
    for _, button in ipairs(device.buttons) do
        if button == "up" then
            player = device.player
            break
        end
    end
    if player then break end
end
assert(player, "Connect a controller with a right direction")

local checkpoint = assert(savestate.create())
assert(savestate.save(checkpoint))
assert(input.set(player, {left = false, right = true}))

local frames, pressed = 30, 0
for _ = 1, frames do
    assert(emu.frameadvance())
    local controls = assert(input.get(player))
    if controls.right and not controls.left then pressed = pressed + 1 end
end
assert(input.clear(player))
print(string.format("Player %d: right held for %d/%d effective frames", player, pressed, frames))

assert(savestate.load(checkpoint))
assert(emu.frameadvance())
assert(savestate.free(checkpoint))
print("Checkpoint restored and advanced one frame with live input.")
