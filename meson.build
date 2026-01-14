project('wsland', 'c', version : '1.0.0', default_options : ['warning_level=3', 'c_std=c11'])

sources = files(
    'src/main.c',
    'src/wsland.c',
    'src/freerdp.c',
    'src/freerdp_tls.c',
    'src/freerdp_shell.c',
    'src/freerdp_surface.c',
    'src/freerdp_instance.c',
    'src/freerdp_dispatch.c',
    'src/freerdp_keyboard.c',
    'src/freerdp_ctx_server_rail.c',
    'src/freerdp_ctx_server_disp.c',
    'src/freerdp_ctx_server_rdpgfx.c',
    'src/freerdp_ctx_server_drdynvc.c',
    'src/freerdp_ctx_server_applist.c',
    'src/freerdp_ctx_server_gfxredir.c',
    'src/freerdp_ctx_server_rail_language.c',
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
dep_freerdp_server = dependency('freerdp-server2', version : '>=2.4.0')
dep_rdpapplist = dependency('rdpapplist', version: '>= 2.0.0')
dep_wayland_server = dependency('wayland-server', version : '>= 1.24.0')
dep_xkbcommon = dependency('xkbcommon', version : '>=1.11.0')
dep_openssl = dependency('openssl', version: '>= 1.1.1')
dep_pixman = dependency('pixman-1', version : '>=0.46.4')
def_cairo = dependency('cairo', required: false, disabler: true)

wayland_kwargs = {
    'version': '>=1.23.1',
    'fallback': 'wayland',
    'default_options': [
        'tests=false',
        'documentation=false',
    ],
}

subdir('protocol')

executable('wsland', [sources, protocols_server_header['xdg-shell']], include_directories : [], dependencies : [
    dep_wlroots, dep_winpr, dep_freerdp, dep_freerdp_server, dep_rdpapplist, dep_wayland_server,
    dep_pixman, def_cairo, dep_openssl, dep_xkbcommon,
])