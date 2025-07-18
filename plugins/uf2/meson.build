if host_machine.system() != 'linux'
  subdir_done()
endif

cargs = ['-DG_LOG_DOMAIN="FuPluginUf2"']
plugins += {meson.current_source_dir().split('/')[-1]: true}

plugin_quirks += files('uf2.quirk')
plugin_builtin_uf2 = static_library('fu_plugin_uf2',
  rustgen.process(
    'fu-uf2.rs',          # fuzzing
  ),
  sources: [
    'fu-uf2-plugin.c',
    'fu-uf2-device.c',
    'fu-uf2-firmware.c',      # fuzzing
  ],
  include_directories: plugin_incdirs,
  c_args: cargs,
  dependencies: plugin_deps,
  link_with: plugin_libs,
)
plugin_builtins += plugin_builtin_uf2

if get_option('tests')
  install_data(['tests/uf2.builder.xml'],
    install_dir: join_paths(installed_test_datadir, 'tests'))
  env = environment()
  env.set('G_TEST_SRCDIR', meson.current_source_dir())
  env.set('G_TEST_BUILDDIR', meson.current_build_dir())
  e = executable(
    'uf2-self-test',
    sources: [
      'fu-self-test.c',
    ],
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [
      plugin_libs,
      plugin_builtin_uf2,
    ],
    install: true,
    install_rpath: libdir_pkg,
    install_dir: installed_test_bindir,
    c_args: [
      '-DSRCDIR="' + meson.current_source_dir() + '"',
    ],
  )
  test('uf2-self-test', e, env: env)
endif
