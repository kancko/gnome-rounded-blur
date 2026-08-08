project('gnome-rounded-blur', 'c',
  version: '1.0.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.56.0',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
  ],
)

# Project information
project_name = meson.project_name()
project_version = meson.project_version()

# API version
api_version = '1.0'

# Directories
prefix = get_option('prefix')
libdir = join_paths(prefix, get_option('libdir'))
includedir = join_paths(prefix, get_option('includedir'))
datadir = join_paths(prefix, get_option('datadir'))

mutter_api_versions = ['51', '18']
mutter_req = '>= 50.0'

# Dependencies
mutter_api_version = ''
clutter_dep = disabler()
cogl_dep = disabler()
libmutter_dep = disabler()

foreach ver : mutter_api_versions
  clutter_pc = 'mutter-clutter-' + ver
  cogl_pc = 'mutter-cogl-' + ver
  libmutter_pc = 'libmutter-' + ver

  clutter_try = dependency(clutter_pc, version: mutter_req, required: false)
  cogl_try = dependency(cogl_pc, version: mutter_req, required: false)
  libmutter_try = dependency(libmutter_pc, required: false)

  if clutter_try.found() and cogl_try.found() and libmutter_try.found()
    mutter_api_version = ver
    clutter_dep = clutter_try
    cogl_dep = cogl_try
    libmutter_dep = libmutter_try
    break
  endif
endforeach

if mutter_api_version == ''
  error('The only supported versions of the mutter api are: ' + ', '.join(mutter_api_versions))
endif

glib_dep = dependency('glib-2.0', version: '>= 2.66')
gobject_dep = dependency('gobject-2.0', version: '>= 2.66')

# Required dependencies
deps = [
  glib_dep,
  gobject_dep,
  clutter_dep,
  cogl_dep,
  libmutter_dep
]

# GObject Introspection
gnome = import('gnome')
gir = find_program('g-ir-scanner', required: true)
gir_compiler = find_program('g-ir-compiler', required: true)

# Source subdirectory
subdir('src')
