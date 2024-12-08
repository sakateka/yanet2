dependencies = []
dependencies += libdpdk.get_variable('dpdk_dep')

dependencies += libfilter_dep

sources = files('main.c',
                'dataplane.c',
                'dpdk.c',
                'pipeline.c',
                'module.c',
                'device.c',
                'worker.c',
                'packet/packet.c',
                'packet/encap.c',
                'drivers/sock_dev.c',
                'modules/kernel.c',
                'modules/acl.c',
                'modules/balancer.c',
                'modules/route.c')

executable('yanet-dataplane',
           sources,
           include_directories: yanet_rootdir,
           dependencies: dependencies,
           install: true)

