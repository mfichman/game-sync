gs = require('src.gamesync')
tab = gs.open('gs://127.0.0.1:8000/foo/bar')

    tab.x = 100
    tab.y = {}
    tab.y.z = 99
while true do
    gs.poll(true)
--
end
