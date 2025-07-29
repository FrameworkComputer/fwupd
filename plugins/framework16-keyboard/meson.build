cargs = ['-DG_LOG_DOMAIN="FuPluginFramework16Keyboard"']

plugins += {meson.current_source_dir().split('/')[-1]: true}
plugin_quirks += files('framework16-keyboard.quirk')
plugin_builtins += static_library('fu_plugin_framework16_keyboard',
  rustgen.process('fu-framework16-keyboard.rs'),
  sources: [
    'fu-framework16-keyboard-device.c',
    'fu-framework16-keyboard-plugin.c',
  ],
  include_directories: plugin_incdirs,
  link_with: plugin_libs,
  c_args: cargs,
  dependencies: plugin_deps,
)
