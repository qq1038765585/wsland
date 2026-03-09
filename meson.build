project('wsland', 'c', version : '1.0.0', default_options : ['warning_level=3', 'c_std=c11'])

sources = files(
    'src/main.c',
    'src/utils/log.c',
    'src/utils/box.c',
    'src/utils/time.c',
    'src/utils/config.c',
    'src/server/server.c',
    'src/server/handle.c',
    'src/server/output.c',
    'src/adapter/adapter.c',
    'src/adapter/handle.c',
    'src/freerdp/freerdp.c',
    'src/freerdp/openssl.c',
    'src/freerdp/handle.c',
    'src/freerdp/peer.c',
    'src/freerdp/context/rail.c',
    'src/freerdp/context/disp.c',
    'src/freerdp/context/rdpgfx.c',
    'src/freerdp/context/drdynvc.c',
    'src/freerdp/context/applist.c',
    'src/freerdp/context/gfxredir.c',
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

wlroots_project = subproject(
    'wlroots',
    default_options: [
        'xwayland=enabled'
    ]
)

dep_wlroots = wlroots_project.get_variable('wlroots') # dependency('wlroots-0.18', version : '>=0.18.3', fallback: 'wlroots')
dep_winpr = dependency('winpr2', version : '>=2.4.0')
dep_freerdp = dependency('freerdp2', version : '>=2.4.0')
dep_freerdp_server = dependency('freerdp-server2', version : '>=2.4.0')
dep_rdpapplist = dependency('rdpapplist', version: '>= 2.0.0')
dep_wayland_server = dependency('wayland-server', version : '>= 1.24.0')
dep_xkbcommon = dependency('xkbcommon', version : '>=1.11.0')
dep_openssl = dependency('openssl', version: '>= 1.1.1')
dep_pixman = dependency('pixman-1', version : '>=0.46.4')
dep_xcb = dependency('xcb', version : '>=1.17.0')
dep_xwayland = dependency('xwayland', version : '>=1.17.0')
def_cairo = dependency('cairo', required: false, disabler: true)

wayland_kwargs = {
    'version': '>=1.23.1',
    'fallback': 'wayland',
    'default_options': [
        'tests=false',
        'documentation=false',
    ],
}

include_dir = include_directories('include')
subdir('protocol')

executable('wsland', [sources, protocols_server_header['xdg-shell']], include_directories : [include_dir], dependencies : [
    dep_wlroots, dep_winpr, dep_freerdp, dep_rdpapplist, dep_freerdp_server, dep_wayland_server,
    dep_pixman, def_cairo, dep_openssl, dep_xkbcommon, dep_xcb, dep_xwayland
])