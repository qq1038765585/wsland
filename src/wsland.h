#ifndef WSLAND_H
#define WSLAND_H

#include <wlr/backend.h>
#include <freerdp/listener.h>
#include <freerdp/server/rdpgfx.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wayland-server-core.h>

#define MAX_FREERDP_FDS 32
#define RDP_DEFAULT_REFRESH (60 * 1000) // 60 Hz

enum wsland_cursor_mode {
    WSLAND_CURSOR_PASSTHROUGH,
    WSLAND_CURSOR_MOVE,
    WSLAND_CURSOR_RESIZE,
};

enum wsland_peer_flags {
    WSLAND_RDP_PEER_OUTPUT_ENABLED = 1 << 0,
    WSLAND_RDP_PEER_POINTER_ENABLE = 1 << 1,
    WSLAND_RDP_PEER_KEYBOARD_ENABLE = 1 << 2,
    WSLAND_RDP_PEER_ACTIVATED = 1 << 3,
};

struct wsland_server {
    char *start_command;

    struct wl_display *wl_display;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *scene_background;
    struct wsland_output *primary_output;
    struct wlr_scene *scene;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;
    struct wl_list toplevels;

    struct wlr_cursor *cursor;
    struct wl_listener cursor_motion;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_frame;
    struct wl_listener cursor_axis;

    struct wlr_seat *seat;
    struct wl_list keyboards;
    struct wl_listener new_input;
    struct wl_listener request_cursor;
    struct wl_listener request_set_selection;
    struct wsland_toplevel *grabbed_toplevel;
    enum wsland_cursor_mode cursor_mode;
    double grab_x, grab_y;
    struct wlr_box grab_geobox;
    uint32_t resize_edges;

    struct wlr_output_layout *output_layout;
    struct wl_listener new_surface;
    struct wl_listener new_output;
    struct wl_list outputs;

    struct wsland_freerdp *freerdp;
};

struct wsland_freerdp {
    struct wl_list clients;
    struct rdp_freerdp_listener *listener;
    struct wl_event_source *listener_events[MAX_FREERDP_FDS];

    int port;
    char* address;
    const char* tls_key_path;
    const char* tls_cert_path;

    void *data;
};

struct wsland_pointer {
    struct wlr_pointer pointer;
    struct wsland_server *server;

    struct wl_listener destroy;
};

struct wsland_keyboard {
    struct wl_list link;

    struct wlr_keyboard keyboard;
    struct wsland_server *server;

    struct wl_listener modifiers;
    struct wl_listener destroy;
    struct wl_listener key;
};

struct wsland_client_context {
    struct rdp_context rdp_ctx;

    struct wsland_server *server;
    struct wl_event_source *events[MAX_FREERDP_FDS];
    struct rdp_freerdp_peer *client;

    struct wsland_output *output;
    struct wsland_pointer *pointer;
    struct wsland_keyboard *keyboard;

    uint32_t flags; // wlr_rdp_peer_flags
    RFX_RECT *rfx_rects;
    RFX_CONTEXT *rfx_context;
    NSC_CONTEXT *nsc_context;
    wStream *encode_stream;

    struct wl_list link;
};

struct wsland_surface {
    struct wlr_surface *wlr_surface;
    struct wsland_server *server;

    struct wl_listener destroy;
    struct wl_listener commit;
};

struct wsland_output {
    struct wl_list link;

    struct wsland_server *server;
    struct wsland_client_context *context;

    struct wlr_output *base;
    struct wlr_scene_output *scene_output;

    pixman_image_t *shadow_surface;
    pixman_region32_t shadow_region;

    struct wl_listener frame;
    struct wl_listener request_state;
    struct wl_listener destroy;

};

struct wsland_toplevel {
    struct wl_list link;
    struct wsland_server *server;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;
    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener commit;
    struct wl_listener destroy;
    struct wl_listener request_move;
    struct wl_listener request_resize;
    struct wl_listener request_maximize;
    struct wl_listener request_fullscreen;
};

struct wsland_popup {
    struct wlr_xdg_popup *xdg_popup;
    struct wl_listener commit;
    struct wl_listener destroy;
};

void commit_buffer(struct wsland_output *output);
struct xkb_rule_names fetch_xkb_rule_names(struct rdp_settings *settings);

struct wlr_backend *wlr_rdp_backend_create(struct wl_event_loop *loop);

void freerdp_init(struct wsland_server *server);
int server_init(int argc, char *argv[]);
#endif
