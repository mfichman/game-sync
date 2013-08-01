gs = require('gamesync')

gs.listen(8000)
while true do
    print('poll')
    gs.poll()
end
