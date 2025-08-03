fuzz_targets = {
  'swissmap_operations': [
    lib_common_dep,
  ],
  'swissmap_memory': [
    lib_common_dep,
  ],
  'swissmap_internals': [
    lib_common_dep,
  ],
}

nofuzz_sources = []
fuzz_args = []
if get_option('fuzzing').enabled()
  fuzz_args = ['-fsanitize=fuzzer,address']
else
  nofuzz_sources += 'reproducer.c'
endif

foreach target_name, deps : fuzz_targets
  executable(
    target_name,
    c_args: yanet_c_args + fuzz_args,
    link_args: yanet_link_args + fuzz_args,
    sources: [target_name + '.c'] + nofuzz_sources,
    dependencies: deps,
    include_directories: [yanet_rootdir, yanet_libdir],
    link_language: 'c',
  )
endforeach
