ring_test = executable(
  'ring_test',
  ['ring_test.c'],
  c_args: yanet_c_args,
  link_args: yanet_link_args,
  dependencies: [
    lib_logging_dep,
  ],
  include_directories: yanet_rootdir,
)

test('ring', ring_test, suite: 'ring')

ring_object_test = executable(
  'ring_object_test',
  ['object_test.c'],
  c_args: yanet_c_args,
  link_args: yanet_link_args + [
    # Exports the test's stub module constructor to the harness's dlsym.
    '-Wl,-E',
  ],
  dependencies: [
    lib_dataplane_ut_dep,
    lib_logging_dep,
    lib_errors_dep,
    lib_ring_objects_dep,
    libdpdk_dep,
  ],
  include_directories: yanet_rootdir,
)

test('ring_object', ring_object_test, suite: 'ring')

ring_bench_thread_dep = dependency('threads')

ring_bench = executable(
  'ring_bench',
  ['ring_bench.c'],
  c_args: yanet_c_args,
  link_args: yanet_link_args,
  dependencies: [
    ring_bench_thread_dep,
  ],
  include_directories: yanet_rootdir,
)

benchmark('ring_bench', ring_bench, timeout: 120)
