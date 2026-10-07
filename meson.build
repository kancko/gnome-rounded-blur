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

# mutter renamed its pkg-config files: the ABI number up to GNOME 50
# (libmutter-18), then the release number from GNOME 51 on (libmutter-51).
# Detect whichever is installed instead of hardcoding it.
pkg_config_bin = find_program('pkg-config', 'pkgconf')
libmutter_pc = ''
foreach line : run_command(pkg_config_bin, '--list-all', check: true).stdout().split('\n')
  tokens = line.strip().split(' ')
  if tokens.length() > 0 and tokens[0].startswith('libmutter-')
    libmutter_pc = tokens[0]
  endif
endforeach
if libmutter_pc == ''
  error('No libmutter-*.pc found, is mutter installed with its development files?')
endif

mutter_api_version = libmutter_pc.split('libmutter-')[1]
message('Detected mutter API version: ' + mutter_api_version)

mutter_req = '>= 50.0'

clutter_pc = 'mutter-clutter-' + mutter_api_version
cogl_pc = 'mutter-cogl-' + mutter_api_version

# Dependencies
glib_dep = dependency('glib-2.0', version: '>= 2.66')
gobject_dep = dependency('gobject-2.0', version: '>= 2.66')
clutter_dep = dependency(clutter_pc, version: mutter_req,
  required: false,
)

cogl_dep = dependency(cogl_pc, version: mutter_req,
  required: false,
)
libmutter_dep = dependency(libmutter_pc)

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
