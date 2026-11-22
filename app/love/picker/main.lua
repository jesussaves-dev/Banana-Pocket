-- Game picker: lists *.love files and game folders (with a main.lua) in the games dir and writes the choice to the
-- file named by $LOVE_PICK_OUT, then quits. run-love starts the game and comes back here when it ends.
local dir = os.getenv("LOVE_GAMES_DIR") or "/data/banana/appdata/love/games"
local out = os.getenv("LOVE_PICK_OUT")
local list, sel, top = {}, 1, 0
local font, small

local function exists(p)
    local f = io.open(p, "rb"); if f then f:close(); return true end
    return false
end

local function scan()
    list = {}
    local p = io.popen('ls -1 "' .. dir .. '" 2>/dev/null')
    if p then
        for n in p:lines() do
            local full = dir .. "/" .. n
            if n:match("%.love$") then
                list[#list + 1] = { name = n:gsub("%.love$", ""), path = full }
            elseif exists(full .. "/main.lua") then
                list[#list + 1] = { name = n, path = full }
            end
        end
        p:close()
    end
    table.sort(list, function(a, b) return a.name:lower() < b.name:lower() end)
end

function love.load()
    font = love.graphics.newFont(26)
    small = love.graphics.newFont(16)
    scan()
end

local function pick()
    if #list == 0 or not out then return end
    local f = io.open(out, "w")
    if f then f:write(list[sel].path, "\n"); f:close() end
    love.event.quit()
end

local function move(d)
    if #list == 0 then return end
    sel = math.max(1, math.min(#list, sel + d))
end

function love.keypressed(k)
    if k == "down" then move(1) elseif k == "up" then move(-1)
    elseif k == "pagedown" then move(8) elseif k == "pageup" then move(-8)
    elseif k == "return" then pick() elseif k == "escape" then love.event.quit() end
end

function love.gamepadpressed(_, b)
    if b == "dpdown" then move(1) elseif b == "dpup" then move(-1)
    elseif b == "rightshoulder" then move(8) elseif b == "leftshoulder" then move(-8)
    elseif b == "b" or b == "a" then pick()   -- east = accept on this layout; B/A both start
    elseif b == "back" or b == "start" then love.event.quit() end
end

function love.draw()
    local w, h = love.graphics.getDimensions()
    love.graphics.clear(0.09, 0.07, 0.12)
    love.graphics.setFont(font)
    love.graphics.setColor(0.91, 0.33, 0.6)
    love.graphics.print("LOVE", 24, 16)
    love.graphics.setColor(1, 1, 1)
    local rows = math.floor((h - 110) / 38)
    if sel <= top then top = sel - 1 elseif sel > top + rows then top = sel - rows end
    if #list == 0 then
        love.graphics.setFont(small)
        love.graphics.printf("No games yet.\nPut .love files (or game folders) in:\n" .. dir, 24, 90, w - 48)
        return
    end
    for i = 1, math.min(rows, #list - top) do
        local g = list[top + i]
        local y = 70 + (i - 1) * 38
        if top + i == sel then
            love.graphics.setColor(0.91, 0.33, 0.6, 0.35)
            love.graphics.rectangle("fill", 12, y - 4, w - 24, 36, 6, 6)
            love.graphics.setColor(1, 1, 1)
        else
            love.graphics.setColor(0.8, 0.8, 0.85)
        end
        love.graphics.print(g.name, 24, y)
    end
    love.graphics.setFont(small)
    love.graphics.setColor(0.6, 0.6, 0.7)
    love.graphics.print("A/B start   L/R page   Select/Start exit", 24, h - 28)
end
