dependencies = [lib_common_dep]

lpm_test = executable(
  'lpm_test',
  ['lpm_test.c',],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('lpm', lpm_test, suite: ['common'])

radix_test = executable(
  'radix_test',
  ['radix_test.c',],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('radix', radix_test, suite: ['common'])

table_test = executable(
  'table_test',
  ['value_table.c'],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies
)
test('value_table', table_test, suite: ['common'])

ttlmap_test_basic = executable(
  'ttlmap_basic_test',
  ['ttlmap_basic_test.c'],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('ttlmap_basic', ttlmap_test_basic, suite: ['common'])

ttlmap_bench_test = executable(
  'ttlmap_bench_test',
  ['ttlmap_bench_test.c'],
  c_args: yanet_c_args,  # not testing args
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('ttlmap_bench-st', ttlmap_bench_test, suite: ['common', 'large'])

ttlmap_heavy_bench_test = executable(
  'ttlmap_heavy_bench_test',
  ['ttlmap_heavy_bench_test.c'],
  c_args: yanet_c_args,  # not testing args
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('ttlmap_heavy_bench-mt', ttlmap_heavy_bench_test, suite: ['common', 'large'])

layermap_test = executable(
  'layermap_test',
  ['layermap_test.c'],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('layermap_basic', layermap_test, suite: ['common'])
