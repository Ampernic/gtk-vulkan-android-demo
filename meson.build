project('gtk-vulkan-android-demo', 'c',
  version: '0.1.0',
  meson_version: '>= 0.62.0',
  default_options: ['c_std=c11', 'warning_level=2'])

cc = meson.get_compiler('c')

deps = [
  dependency('gtk4'),
  dependency('libadwaita-1', version: '>= 1.4'),
  cc.find_library('m', required: false),
]

executable('gtk-vulkan-demo',
  'src/main.c',
  dependencies: deps,
  install: true)
