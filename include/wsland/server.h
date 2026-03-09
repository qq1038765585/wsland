// ReSharper disable All
#ifndef WSLAND_SERVER_H
#define WSLAND_SERVER_H

#include <wlr/util/box.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/backend.h>

#include "wsland/utils/config.h"

#define WSLAND_DEFAULT_REFRESH (60 * 1000) // 60 Hz

struct wsland_window;
struct wsland_toplevel;

extern const struct wlr_keyboard_impl wsland_keyboard_impl;
extern const struct wlr_pointer_impl wsland_pointer_impl;

typedef enum wsland_cursor_mode {
    WSLAND_CURSOR_PASSTHROUGH,
    WSLAND_CURSOR_RESIZE,
    WSLAND_CURSOR_MOVE,
} wsland_cursor_mode;

typedef struct wsland_server_handle {
    void (*server_new_output)(struct wl_listener *listener, void *data);
    void (*server_new_input)(struct wl_listener *listener, void *data);
    void (*server_new_xdg_toplevel)(struct wl_listener *listener, void *data);
    void (*server_new_xdg_popup)(struct wl_listener *listener, void *data);
    void (*server_cursor_motion)(struct wl_listener *listener, void *data);
    void (*server_cursor_motion_absolute)(struct wl_listener *listener, void *data);
    void (*server_cursor_button)(struct wl_listener *listener, void *data);
    void (*server_cursor_axis)(struct wl_listener *listener, void *data);
    void (*server_cursor_frame)(struct wl_listener *listener, void *data);
    void (*seat_request_cursor)(struct wl_listener *listener, void *data);
    void (*seat_request_set_selection)(struct wl_listener *listener, void *data);

    void (*wsland_xwayland_new_surface)(struct wl_listener *listener, void *data);

    void (*server_focus_toplevel)(struct wsland_toplevel *toplevel);
} wsland_server_handle;

typedef struct wsland_toplevel {
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_scene_tree *tree;
    struct wlr_box before;

    struct {
        struct wl_listener map;
        struct wl_listener unmap;
        struct wl_listener commit;
        struct wl_listener destroy;
        struct wl_listener request_move;
        struct wl_listener request_resize;
        struct wl_listener request_maximize;
        struct wl_listener request_fullscreen;
    } events;


    struct wl_list server_link;
    struct wl_list parent_link;
    struct wl_list children;

    bool window_dirty;
    struct wsland_window *window_data;
    struct wsland_server *server;
} wsland_toplevel;

typedef struct wsland_popup {
    struct wlr_xdg_popup *popup;

    struct {
        struct wl_listener map;
        struct wl_listener commit;
        struct wl_listener destroy;
    } events;
} wsland_popup;

typedef struct wsland_scene_buffer {
    struct wlr_scene_buffer *buffer;

    struct {
        struct wl_listener scene_surface_commit;
        struct wl_listener scene_surface_destroy;
    } events;

    wsland_toplevel *toplevel;
} wsland_scene_buffer;

typedef struct wsland_keyboard {
    struct wlr_keyboard keyboard;

    struct {
        struct wl_listener key;
        struct wl_listener modifiers;
        struct wl_listener destroy;
    } events;

    struct wl_list server_link;

    struct wsland_server *server;
} wsland_keyboard;

typedef struct wsland_output {
    struct wlr_box monitor;
    struct wlr_output output;
    struct wlr_pointer pointer;
    struct wsland_server *server;
    struct wlr_scene_output *scene_output;
    pixman_region32_t pending_commit_damage;
    struct wlr_box taskbar_area;
    struct wlr_box work_area;

    struct {
        struct wl_listener frame;
        struct wl_listener destroy;
    } events;

    struct wl_event_source *frame_timer;
    int frame_delay; // ms
    bool primary;

    struct wl_list peer_link;
    struct wl_list server_link;
} wsland_output;

typedef struct wsland_server {
    const char *socket_name;
    struct wl_display *display;
    struct wl_event_loop *event_loop;

    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_subcompositor *subcompositor;
    struct wlr_data_device_manager *data_device_manager;
    struct wlr_server_decoration_manager *server_decoration_manager;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_viewporter *viewporter;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_cursor *cursor;
    struct wlr_scene *scene;
    struct wlr_seat *seat;

    struct wlr_xwayland *xwayland;

    struct wlr_output_layout *output_layout;
    struct wlr_scene_output_layout *scene_layout;

    struct wl_list outputs;
    struct wl_list keyboards;
    struct wl_list toplevels;

    struct {
        double x, y;
        struct wlr_box geobox;
        wsland_toplevel *toplevel;
    } grab;

    struct {
        uint32_t resize_edges;
        wsland_cursor_mode mode;
    } move;

    struct {
        struct wlr_surface *surface;
        struct wlr_buffer *buffer;
        int s_hotspot_x, s_hotspot_y;
        int hotspot_x, hotspot_y;
        bool restore;
        bool dirty;
    } cache_cursor;

    struct {
        struct wl_listener new_output;
        struct wl_listener new_input;
        struct wl_listener new_xdg_toplevel;
        struct wl_listener new_xdg_popup;
        struct wl_listener cursor_motion_absolute;
        struct wl_listener cursor_motion;
        struct wl_listener cursor_button;
        struct wl_listener cursor_frame;
        struct wl_listener cursor_axis;
        struct wl_listener request_cursor;
        struct wl_listener request_set_selection;

        struct wl_listener wsland_xwayland_new_surface;

        struct wl_listener wsland_cursor_destroy;

        struct wl_signal wsland_window_motion;
        struct wl_signal wsland_window_destroy;

        struct wl_signal wsland_cursor_frame;
        struct wl_signal wsland_output_frame;
    } events;

    wsland_config *config;
    wsland_server_handle *handle;
} wsland_server;

wsland_server_handle *wsland_server_handle_init(wsland_server *server);

wsland_output *wsland_output_create(wsland_server *server, int width, int height);
bool wlr_output_is_wsland(struct wlr_output *wlr_output);

wsland_server *wsland_server_create(wsland_config *config);
void wsland_server_running(wsland_server *server);
void wsland_server_destroy(wsland_server *server);
#endif
