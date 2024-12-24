dependencies = []
dependencies += dependency('threads')
dependencies += dependency('libpcap', static: false)

sources = files('main.c')

executable('sock-send',
           sources,
           dependencies: dependencies)
