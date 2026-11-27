-- Draw native-pixel shapes on the game and a fixed-size label in the video view.
-- @image overlay-marker.png
local russian = emu.language == "ru"
local function text(en, ru) return russian and ru or en end

local function render(e)
    local screen = emu.screen()
    local w, h = screen.width, screen.height
    if w < 40 or h < 48 then return end

    -- The animation follows emulated time, including after loads and rewind.
    local x = w // 2 + math.floor(math.sin(e.frame / 24) * w / 5)
    local y = h // 2
    draw.rect(x - 16, y - 12, 32, 24, 0x7fffd4ff, 0x0b151c99)
    draw.circle(x, y, 10, 0x7fffd4ff)
    draw.line(x - 24, y, x + 24, y, 0x7fffd4ff)
    if not draw.image("overlay-marker.png", x - 6, y - 6) then
        draw.circle(x, y, 3, 0xffffffff, 0xffffffff)
    end

    local label = string.format(text("Game %d × %d px", "Кадр %d × %d пкс"), w, h)
    draw.rect(10, 10, utf8.len(label) * 14 + 14, 23, 0x718592c0, 0x09121ddd, "hud")
    draw.text(17, 13, label, 0xffffffff, "hud")
end

for _, event in ipairs({"frame", "draw", "reset", "loadstate", "rewind"}) do
    emu.on(event, render)
end
