host_machine.system() == 'linux' or subdir_done()

cargs = ['-DG_LOG_DOMAIN="FuPluginMtd"']
plugins += {meson.current_source_dir().split('/')[-1]: true}

plugin_quirks += files('mtd.quirk')
plugin_builtin_mtd = static_library('fu_plugin_mtd',
  sources: [
    'fu-mtd-plugin.c',
    'fu-mtd-device.c',
    'fu-mtd-ifd-device.c',
  ],
  include_directories: plugin_incdirs,
  link_with: plugin_libs,
  c_args: cargs,
  dependencies: plugin_deps,
)
plugin_builtins += plugin_builtin_mtd

if not supported_build
  plugin_quirks += files('ci.quirk')
  device_tests += files('tests/mtdram.json')
endif

if get_option('tests')
  env = environment()
  env.set('G_TEST_SRCDIR', meson.current_source_dir())
  env.set('G_TEST_BUILDDIR', meson.current_build_dir())
  e = executable(
    'mtd-self-test',
    sources: [
      'fu-self-test.c',
    ],
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [
      plugin_libs,
      plugin_builtin_mtd,
    ],
    install: true,
    install_rpath: libdir_pkg,
    install_dir: installed_test_bindir,
    c_args: [
      cargs,
      '-DSRCDIR="' + meson.current_source_dir() + '"',
    ],
  )
  test('mtd-self-test', e, env: env)  # added to installed-tests
endif
