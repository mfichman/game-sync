import os

VariantDir('build', 'src', duplicate=0)

env = Environment() #$MSVC_VERSION='11.0', TARGET_ARCH='x86')
env.Append(ENV = os.environ)
if env['PLATFORM'] == 'win32':
    includes = ['D:\\Tools\\LuaJIT-2.0.2\\src']
    libs = ['ws2_32.lib', 'D:\\Tools\\LuaJIT-2.0.2\\src\\lua51.lib']
    env.Append(CFLAGS = '/MT /Zi')
    env.Append(LINKFLAGS = '/DEBUG')

    for inc in includes:
        env.Append(CFLAGS='/I%s' % inc)

src = env.Glob('build/**.c')
env.SharedLibrary('bin/gamesyncnative', src, LIBS=libs)
