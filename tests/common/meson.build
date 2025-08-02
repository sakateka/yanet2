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

# Swissmap tests
test_large_swissmap = executable(
  'test_large_swissmap',
  ['test_large_swissmap.c',],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('large_swissmap', test_large_swissmap, suite: ['common', 'large'])

test_multiprocess_swissmap = executable(
  'test_multiprocess_swissmap',
  ['test_multiprocess_swissmap.c',],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('multiprocess_swissmap', test_multiprocess_swissmap)

test_swissmap = executable(
  'test_swissmap',
  ['test_swissmap.c',],
  c_args: yanet_test_c_args,
  link_args: yanet_link_args,
  dependencies: dependencies,
)
test('swissmap', test_swissmap)
