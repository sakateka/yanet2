test_env = environment(
  {
    'MALLOC_PERTURB_': '0',
    'CGO_CFLAGS': '--coverage',
    'CGO_LDFLAGS': '--coverage',
    'ASAN_OPTIONS': '',
    'UBSAN_OPTIONS': '',
    'MSAN_OPTIONS': '',
  },
)

sources = files(
  'decap.go',
  'decap_test.go',
)

test(
  'decap',
  find_program('go'),
  args: ['test', './...'] + (get_option('b_coverage') ? ['-cover', '-args', '-test.gocoverdir=' + meson.current_build_dir()]: []),
  workdir: meson.current_source_dir(),
  timeout: 120,
  env: test_env,
)
