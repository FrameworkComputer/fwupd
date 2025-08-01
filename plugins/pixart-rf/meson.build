cc.has_header('linux/hidraw.h', required: false) or subdir_done()

cargs = ['-DG_LOG_DOMAIN="FuPluginPixartRf"']
plugins += {meson.current_source_dir().split('/')[-1]: true}

plugin_quirks += files('pixart-rf.quirk')
plugin_builtin_pxi = static_library('fu_plugin_pxi',
  rustgen.process(
    'fu-pxi.rs', # fuzzing
  ),
  sources: [
    'fu-pxi-plugin.c',
    'fu-pxi-common.c', # fuzzing
    'fu-pxi-ble-device.c',
    'fu-pxi-receiver-device.c',
    'fu-pxi-wireless-device.c',
    'fu-pxi-firmware.c', # fuzzing
  ],
  include_directories: plugin_incdirs,
  c_args: cargs,
  dependencies: plugin_deps,
  link_with: plugin_libs,
)
plugin_builtins += plugin_builtin_pxi

device_tests += files(
  'tests/hp-910-bt-keyboard.json',
  'tests/hp-910-bt-mouse.json',
  'tests/pixart-rf-2862-dongle.json',
  'tests/pixart-rf-GCBBTM25.json',
)

if get_option('tests')
  install_data(['tests/pixart.builder.xml'],
    install_dir: join_paths(installed_test_datadir, 'tests'))
  env = environment()
  env.set('G_TEST_SRCDIR', meson.current_source_dir())
  env.set('G_TEST_BUILDDIR', meson.current_build_dir())
  e = executable(
    'pxi-self-test',
    sources: [
      'fu-self-test.c',
    ],
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [
      plugin_libs,
      plugin_builtin_pxi,
    ],
    install: true,
    install_rpath: libdir_pkg,
    install_dir: installed_test_bindir,
    c_args: [
      '-DSRCDIR="' + meson.current_source_dir() + '"',
    ],
  )
  test('pxi-self-test', e, env: env)
endif
