host_machine.system() in ['linux', 'android'] or subdir_done()

cargs = ['-DG_LOG_DOMAIN="FuPluginFrameworkInputmodule"']
plugins += {
  meson.current_source_dir().split('/')[-1]: true,
}
plugin_quirks += files('framework-inputmodule.quirk')
plugin_builtin_framework_inputmodule = static_library(
  'fu_plugin_framework_inputmodule',
  rustgen.process('fu-framework-inputmodule.rs'),
  sources: [
    'fu-framework-inputmodule-device.c',
    'fu-framework-inputmodule-firmware.c',
    'fu-framework-inputmodule-plugin.c',
  ],
  include_directories: plugin_incdirs,
  link_with: plugin_libs,
  c_args: cargs,
  dependencies: plugin_deps,
)
plugin_builtins += plugin_builtin_framework_inputmodule

if get_option('tests')
  e = executable(
    'framework-inputmodule-self-test',
    'fu-self-test.c',
    rustgen.process('fu-framework-inputmodule.rs'),
    include_directories: plugin_incdirs,
    dependencies: plugin_deps,
    link_with: [plugin_libs, plugin_builtin_framework_inputmodule],
    c_args: cargs,
  )
  test('framework-inputmodule-self-test', e)
endif
