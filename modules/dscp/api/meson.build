cp_dependencies =  [
  lib_common_dep,
  lib_config_dp_dep,
  lib_config_cp_dep,
]

includes = include_directories('../dataplane')

api_sources = files(
  'controlplane.c',
)

lib_dscp_cp = static_library(
  'dscp_cp',
  api_sources,
  c_args: yanet_c_args,
  link_args: yanet_link_args,
  dependencies: cp_dependencies,
  include_directories: includes,
  install: false,
)

lib_dscp_cp_dep = declare_dependency(
  link_with: lib_dscp_cp,
)
