#ifndef WSLAND_ADAPTER_H
#define WSLAND_ADAPTER_H

#include "wsland/server.h"
#include "wsland/freerdp.h"

#define RAIL_MARKER_WINDOW_ID  0xFFFFFFFE
#define RAIL_DESKTOP_WINDOW_ID 0xFFFFFFFF

#define WSLAND_BORDER_WIDTH 2
#define WSLAND_BORDER_ACTIVE (struct wlr_render_color) { 0.2, 0.6, 1.0, 1.0 }
#define WSLAND_BORDER_DEACTIVE (struct wlr_render_color) { 0.7, 0.7, 0.7, 1.0 }

enum adapter_type {
    TOPLEVEL, POPUP, XWAYLAND
};

typedef struct wsland_window {
    enum adapter_type type;
    struct wlr_scene_tree *tree;
    struct wsland_window *parent;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;

    uint32_t window_id;
    uint32_t parent_id;
    uint32_t surface_id;
    struct wlr_box before;
    struct wlr_box current;
    struct wlr_render_color border_color;
    int scale_w, scale_h;
    char *title;

    uint32_t pool_id;
    uint32_t buffer_id;
    void *window_buffer;
    int buffer_width, buffer_height;
    wsland_shared_memory shared_memory;
    bool update_pending;

    struct wlr_box damage;
    struct wlr_buffer *buffer;
    struct wlr_texture *texture;
    uint32_t resize_serial;
    bool buffer_opaque;

    union {
        struct wlr_xdg_surface *wayland;
        struct wlr_xwayland_surface *xwayland;
    };

    struct {
        struct wl_listener map;
        struct wl_listener unmap;
        struct wl_listener commit;
        struct wl_listener destroy;
        struct wl_listener associate;
        struct wl_listener dissociate;
        struct wl_listener reposition;
        struct wl_listener new_popup;

        struct wl_listener set_hints;
        struct wl_listener set_geometry;
        struct wl_listener request_move;
        struct wl_listener request_resize;
        struct wl_listener request_maximize;
        struct wl_listener request_fullscreen;
        struct wl_listener request_configure;
        struct wl_listener request_activate;

        struct wl_listener request_decoration_mode;
        struct wl_listener decoration_destroy;
    } events;

    struct wl_list server_link;
    struct wl_list parent_link;
    struct wl_list children;

    wsland_server *server;
    wsland_window_handle *handle;
} wsland_window;


void wsland_free_shared_memory(wsland_freerdp *freerdp, wsland_shared_memory *shared_memory);
bool wsland_allocate_shared_memory(wsland_freerdp *freerdp, wsland_shared_memory *shared_memory);
void wsland_adapter_frame_for_peer(wsland_peer *peer, RDPGFX_FRAME_ACKNOWLEDGE_PDU frame_acknowledge);

void wsland_adapter_work_area_for_peer(wsland_peer *peer, struct wlr_box area);
void wsland_adapter_taskbar_area_for_peer(wsland_peer *peer, struct wlr_box area);
void wsland_adapter_create_keyboard_for_peer(wsland_peer *peer, rdpSettings *settings);
void wsland_adapter_create_output_for_peer(wsland_peer *peer, rdpMonitor *monitor);


typedef struct wsland_adapter_handle {
    void (*wsland_cursor_frame)(struct wl_listener *listener, void *data);
    void (*wsland_window_frame)(struct wl_listener *listener, void *data);
    void (*wsland_window_motion)(struct wl_listener *listener, void *data);
    void (*wsland_window_destroy)(struct wl_listener *listener, void *data);
} wsland_adapter_handle;

typedef struct wsland_adapter {

    struct {
        struct wl_listener wsland_cursor_frame;
        struct wl_listener wsland_window_frame;
        struct wl_listener wsland_window_motion;
        struct wl_listener wsland_window_destroy;

        struct wl_listener set_selection;
    } events;

    wsland_server *server;
    wsland_freerdp *freerdp;
    wsland_adapter_handle *handle;
} wsland_adapter;

struct wl_event_loop *wsland_adapter_fetch_event_loop(wsland_adapter *adapter);
wsland_adapter_handle *wsland_adapter_handle_init(wsland_adapter *adapter);

void wsland_adapter_destroy(wsland_adapter *adapter);
wsland_adapter *wsland_adapter_create(wsland_server *server);
#endif
