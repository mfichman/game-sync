
gs = require('src.gamesync')
gs.listen(8000)

while true do
    gs.poll(true)
    tab = gs.table['/foo/bar']
    if tab then
        for k, v in pairs(tab) do
            if not k:match('_.*') then
                print(k, v)
            end
        end
    end
end

