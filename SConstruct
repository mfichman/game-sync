
import pkgboot

class Gamesync(pkgboot.Package):
    defines = {}
    includes = [
        'C:\\WinBrew\\include\\luajit',
    ]
    libs = [
        pkgboot.Lib('ws2_32', 'win32'),
		pkgboot.Lib('lua51', 'win32'),
        pkgboot.Lib('luajit-5.1', ('linux', 'darwin')),
    ]
    major_version = '0'
    minor_version = '0'
    patch = '0'

Gamesync()
