gs = require('src.gamesync')
tab = gs.open('gs://127.0.0.1:8000/foo/bar', 'w')
tab.x = 100
tab.y = {}
tab.y.z = 99

gs.poll(true)

--[[
while true do
    gs.poll(true)
    gs.atomic(function() 
        tab.x = 100
        tab.y = 20 
        tab.z = 100 => can block processing
    end)
end

server:
/foo/bar
/bar/baz/bat


]]
