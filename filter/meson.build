dependencies = []
dependencies += libdpdk.get_variable('dpdk_dep')

sources = files('ipfw.c',
                'classify.c')

libfilter = static_library('filter',
                           sources,
                           include_directories: yanet_rootdir,
                           dependencies: dependencies,
                           install: false)

libfilter_dep = declare_dependency(link_with: libfilter)

