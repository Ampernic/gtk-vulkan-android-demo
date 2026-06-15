project('gtk-vulkan-android-demo', 'c',
  version: '0.1.0',
  meson_version: '>= 0.62.0',
  # no c_std=c11: strict ISO C disables the GNU `asm` keyword, which makes
  # libffi's HAVE_AS_X86_PCREL probe fail and its x86_64 unix64.S fall back to
  # the clang-incompatible `X@rel` PCREL form. Use the toolchain default (gnu*).
  default_options: ['warning_level=2'])

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
