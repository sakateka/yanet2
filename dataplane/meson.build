dependencies = []

dependencies += dependency('threads')
dependencies += dependency('yaml-0.1')

dependencies += libdpdk.get_variable('dpdk_dep')

dependencies += lib_common_dep
dependencies += lib_logging_dep
dependencies += lib_pipeline_dp_dep

lib_sources = files(
  'config.c',
  'dataplane.c',
  'device.c',
  'worker.c',
)

lib_dataplane = static_library(
  'dataplane',
  lib_sources,
  c_args: yanet_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
  install: false,
)

lib_dataplane_dep = declare_dependency(
  link_with: lib_dataplane,
  include_directories: include_directories('.'),
)

dependencies += lib_dataplane_dep
dependencies += lib_module_dp_dep
dependencies += lib_pipeline_dp_dep
dependencies += lib_config_dp_dep

dependencies += lib_balancer_dp_dep
dependencies += lib_decap_dp_dep
dependencies += lib_dscp_dp_dep
#dependencies += lib_acl_dp_dep
dependencies += lib_forward_dp_dep
dependencies += lib_route_dp_dep
dependencies += lib_nat64_dp_dep

sources = files(
  'drivers/sock_dev.c',
  'dpdk.c',
  'main.c',
)

if not get_option('fuzzing').enabled()
  executable(
    'yanet-dataplane',
    sources,
    dependencies: dependencies,
    install: true,
  )
endif

#subdir('unittest')
