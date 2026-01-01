project('wsland', 'c', version : '1.0.0', default_options : ['warning_level=3', 'c_std=c11'])

sources = files(
    'src/main.c',
    'src/wsland.c',
    'src/freerdp.c',
)

config_h = configuration_data()
config_h.set_quoted('VERSION', meson.project_version())
configure_file(output: 'config.h', configuration: config_h)

little_endian = target_machine.endian() == 'little'
big_endian = target_machine.endian() == 'big'

add_project_arguments([
  '-D_POSIX_C_SOURCE=200809L',
  '-DWLR_USE_UNSTABLE',
  '-DWLR_PRIVATE=',
  '-DWLR_LITTLE_ENDIAN=@0@'.format(little_endian.to_int()),
  '-DWLR_BIG_ENDIAN=@0@'.format(big_endian.to_int()),
], language: 'c')

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
 '-Wundef',
 '-Wlogical-op',
 '-Wmissing-include-dirs',
 '-Wold-style-definition',
 '-Wpointer-arith',
 '-Winit-self',
 '-Wstrict-prototypes',
 '-Wimplicit-fallthrough=2',
 '-Wendif-labels',
 '-Wstrict-aliasing=2',
 '-Woverflow',
 '-Wmissing-prototypes',
 '-Walloca',

 '-Wno-deprecated',
 '-Wno-missing-braces',
 '-Wno-missing-field-initializers',
 '-Wno-unused-parameter',
]), language: 'c')

dep_wlroots = dependency('wlroots-0.19', version : '>=0.19.2')
dep_winpr = dependency('winpr2', version : '>=2.2.0')
dep_freerdp = dependency('freerdp2', version : '>=2.4.0')
dep_wayland_server = dependency('wayland-server', version : '>= 1.24.0')
dep_xkbcommon = dependency('xkbcommon', version : '>=1.11.0')
dep_pixman = dependency('pixman-1', version : '>=0.46.4')
def_cairo = dependency('cairo', required: false, disabler: true)

subdir('protocol')


executable('wsland', sources, include_directories : [protocol_dir], dependencies : [
    dep_wlroots, dep_winpr, dep_freerdp, dep_wayland_server, dep_pixman, def_cairo,
    dep_xkbcommon, dep_wayland_protocols, dep_wayland_scanner,
])