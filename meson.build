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

# android_exe_type is a Pixiewood meson-fork kwarg; on Android the entry must be
# the shared library RuntimeApplication dlopens (libgtk-vulkan-demo.so), not a
# plain executable. Only pass it when targeting Android so stock meson doesn't
# choke on the unknown kwarg.
exe_kwargs = {}
if host_machine.system() == 'android'
  exe_kwargs = {'android_exe_type': 'application'}
endif

executable('gtk-vulkan-demo',
  'src/main.c',
  dependencies: deps,
  kwargs: exe_kwargs,
  install: true)
