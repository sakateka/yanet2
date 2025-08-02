dependencies = [lib_common_dep]

lpm_test = executable(
  'lpm_test',
  ['lpm_test.c',],
  dependencies: dependencies,
)
test('lpm', lpm_test)

# Swissmap tests
test_large_swissmap = executable(
  'test_large_swissmap',
  ['test_large_swissmap.c',],
  dependencies: dependencies,
)
test('large_swissmap', test_large_swissmap, suite: ['yanet', 'large'])

test_multiprocess_swissmap = executable(
  'test_multiprocess_swissmap',
  ['test_multiprocess_swissmap.c',],
  dependencies: dependencies,
)
test('multiprocess_swissmap', test_multiprocess_swissmap)

test_swissmap = executable(
  'test_swissmap',
  ['test_swissmap.c',],
  dependencies: dependencies,
)
test('swissmap', test_swissmap)
