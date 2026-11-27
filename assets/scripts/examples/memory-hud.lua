-- A portable read-only HUD. Run it with a game loaded.
local catalog = {}

local function render(e)
    local domains = catalog[e.instance]
    if not domains then
        domains = memory.domains(e.instance) or {}
        catalog[e.instance] = domains
    end

    draw.text(12, 12, string.format("Console %s   Frame %d",
        e.instance == 0 and "A" or "B", e.frame), 0xffffffff, "hud")

    for _, domain in ipairs(domains) do
        if (domain.id == 2 or domain.base == 0xc000) and domain.size >= 2 then
            local value = memory.read(domain.id, 0, 2)
            if value then
                local label = string.format("%s [0x0000] = %04X", domain.name, value)
                draw.text(12, 28, label, 0x7fffd4ff, "hud")
            end
            break
        end
    end
end

for _, event in ipairs({"frame", "reset", "loadstate", "rewind", "draw"}) do
    emu.on(event, render)
end
