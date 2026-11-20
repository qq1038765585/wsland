project('wsland', 'c', version : '1.0.0', default_options: ['warning_level=3', 'c_std=c11'])

sources = files(
    'src/main.c',
    'src/common/time.c',
    'src/common/logger.c',
    'src/common/config.c',
    'src/common/buffer.c',
    'src/server/dnd.c',
    'src/server/layer.c',
    'src/server/server.c',
    'src/server/service.c',
    'src/server/wayland.c',
    'src/server/xwayland.c',
    'src/backend/backend.c',
    'src/backend/crypto.c',
    'src/backend/client.c',
    'src/backend/keyboard.c',
    'src/backend/channel/disp.c',
    'src/backend/channel/rail.c',
    'src/backend/channel/rdpgfx.c',
    'src/backend/channel/audiorx.c',
    'src/backend/channel/audiotx.c',
    'src/backend/channel/drdynvc.c',
    'src/backend/channel/gfxredir.c',
    'src/backend/channel/clipboard.c',
    'src/adapter/adapter.c',
    'src/adapter/output.c',
    'src/adapter/handle.c',
)

cc = meson.get_compiler('c')

add_project_arguments(cc.get_supported_arguments([
 '-D_GNU_SOURCE',
 '-DWLR_USE_UNSTABLE',

 '-DWLR_PRIVATE=',

 '-Wno-unused-parameter',
 '-Werror=false'
]), language: 'c')

dep_wlroots = dependency('wlroots-0.20', version : '>=0.20.0')
dep_xwayland = dependency('xwayland', version : '>=24.1.9')
dep_winpr = dependency('winpr3', version : '>=3.32.0')
dep_freerdp = dependency('freerdp3', version : '>=3.32.0')
dep_freerdp_server = dependency('freerdp-server3', version : '>=3.32.0')
dep_wayland_server = dependency('wayland-server', version : '>= 1.24.0')
dep_xkbcommon = dependency('xkbcommon', version : '>=1.11.0')
dep_openssl = dependency('openssl', version: '>= 3.0.0')
dep_pixman = dependency('pixman-1', version : '>=0.46.4')
dep_xcb_ewmh = dependency('xcb-ewmh', version : '>=0.4.2')
dep_xcb_icccm = dependency('xcb-icccm', version : '>=0.4.2')
def_cairo = dependency('cairo', required: false, disabler: true)
def_inih = dependency('inih')

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

xdg_shell_protocol = protocols_server_header['xdg-shell']
layer_shell_protocol = protocols_server_header['layer-shell']
pointer_constraints_protocol = protocols_server_header['pointer-constraints']
cursor_shape_protocol = protocols_server_header['cursor-shape']

executable('wsland', [sources, xdg_shell_protocol, layer_shell_protocol, pointer_constraints_protocol, cursor_shape_protocol], include_directories : [include_dir], dependencies : [
    dep_wlroots, dep_winpr, dep_freerdp, dep_freerdp_server, dep_wayland_server, dep_pixman, def_cairo, dep_openssl,
    dep_xkbcommon, dep_xcb_ewmh, dep_xcb_icccm, def_inih, dep_xwayland
], install: true)
