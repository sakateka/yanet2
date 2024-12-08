dependencies = []
dependencies += dependency('threads')
dependencies += dependency('libpcap', static: true)

sources = files('main.c')

executable('sock-send',
           sources,
           dependencies: dependencies)
