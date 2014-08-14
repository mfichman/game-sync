gs = require('src.gamesync')

gs.listen(8000)

while true do
    gs.poll(true)
    tab = gs.table['/foo/bar']
    if tab then
        print(tab)
        for k, v in pairs(tab) do
            print(k, v)
        end
        print('output', #tab._channels.output)
    end
    print()
end

