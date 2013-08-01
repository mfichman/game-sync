gs = require('gamesync')

tab = gs.open('gs://127.0.0.1:8000/foo/bar')
gs.poll()
