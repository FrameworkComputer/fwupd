cargs = ['-DG_LOG_DOMAIN="FuPluginIlitekTouch"']

plugins += {meson.current_source_dir().split('/')[-1]: true}
plugin_quirks += files('ilitek-touch.quirk')
plugin_builtins += static_library('fu_plugin_ilitek_touch',
  rustgen.process('fu-ilitek-touch.rs'),
  sources: [
    'fu-ilitek-touch-device.c',
    'fu-ilitek-touch-plugin.c',
  ],
  include_directories: plugin_incdirs,
  link_with: plugin_libs,
  c_args: cargs,
  dependencies: plugin_deps,
)
enumeration_data += files(
  'tests/framework16-macropad-setup.json',
  'tests/framework16-ansi-setup.json',
)
device_tests += files('tests/ilitek-touch.json')
