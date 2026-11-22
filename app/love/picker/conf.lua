function love.conf(t)
    t.identity = "banana-love-picker"
    t.window.title = "LOVE"
    t.window.fullscreen = true
    t.window.fullscreentype = "desktop"
    t.window.vsync = 1
    t.modules.physics = false
    t.modules.video = false
    t.modules.thread = false
end
