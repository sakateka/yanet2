sources = files(
  'decap.go',
  'decap_test.go',
)

test('decap',
  find_program('go'),
  args: ['test', './...'],
  workdir: meson.current_source_dir(),
)
