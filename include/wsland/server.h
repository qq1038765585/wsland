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

#define WSLAND_DEFAULT_REFRESH (66 * 1000) // 60 Hz
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))
#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define MIN(A, B) ((A) < (B) ? (A) : (B))

struct wsland_server;
struct wsland_window;

extern const struct wlr_keyboard_impl wsland_keyboard_impl;
extern const struct wlr_pointer_impl wsland_pointer_impl;

typedef enum wsland_cursor_mode {
    WSLAND_CURSOR_PASSTHROUGH,
    WSLAND_CURSOR_RESIZE,
    WSLAND_CURSOR_MOVE,
} wsland_cursor_mode;

typedef struct wsland_window_handle {
    char* (*fetch_title)(struct wsland_window *window);
    bool (*fetch_activate)(struct wsland_window *window);
    struct wlr_box (*fetch_region)(struct wsland_window *window);
    struct wlr_box (*fetch_geometry)(struct wsland_window *window);
    struct wlr_surface* (*fetch_surface)(struct wsland_window *window);
    struct wsland_window* (*fetch_parent)(struct wsland_window *window);
    struct wsland_output* (*fetch_output)(struct wsland_window *window);
    struct wlr_scene_tree* (*scene_surface_create)(struct wsland_window *window);
    void (*window_resize)(struct wsland_window *window, int width, int height);
    void (*window_motion)(struct wsland_window *window, int pos_x, int pos_y);
    void (*window_activate)(struct wsland_window *window, bool enabled);
    void (*surface_activate)(struct wlr_surface *surface, bool enabled);
    bool (*window_resize_cannot)(struct wsland_window *window);
    bool (*window_click_cannot)(struct wsland_window *window);
    bool (*window_grab_cannot)(struct wsland_window *window);
    void (*window_maximize)(struct wsland_window *window);
    void (*window_shutdown)(struct wsland_window *window);
    void (*window_center)(struct wsland_window *window);
} wsland_window_handle;

typedef struct wsland_server_handle {
    void (*new_output)(struct wl_listener *listener, void *data);
    void (*new_input)(struct wl_listener *listener, void *data);
    void (*cursor_motion)(struct wl_listener *listener, void *data);
    void (*cursor_motion_absolute)(struct wl_listener *listener, void *data);
    void (*cursor_button)(struct wl_listener *listener, void *data);
    void (*cursor_axis)(struct wl_listener *listener, void *data);
    void (*cursor_frame)(struct wl_listener *listener, void *data);
    void (*new_constraint)(struct wl_listener *listener, void *data);
    void (*request_set_shape)(struct wl_listener *listener, void *data);
    void (*seat_request_cursor)(struct wl_listener *listener, void *data);
    void (*seat_request_selection)(struct wl_listener *listener, void *data);
    void (*seat_keyboard_focus_change)(struct wl_listener *listener, void *data);
    void (*new_toplevel_decoration)(struct wl_listener *listener, void *data);
    void (*new_server_decoration)(struct wl_listener *listener, void *data);
    void (*new_virtual_pointer)(struct wl_listener *listener, void *data);

    void (*reset_server_cursor)(struct wsland_server *server);
    void (*begin_window_interactive)(struct wsland_window *window, wsland_cursor_mode mode, uint32_t edges);
    void (*dispatch_window_focus)(struct wsland_window *window);
} wsland_server_handle;

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

    bool frame_pending;
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
    struct wlr_xcursor_manager *cursor_manager;
    struct wlr_data_device_manager *data_device_manager;
    struct wlr_data_control_manager_v1 *data_control_manager;
    struct wlr_cursor_shape_manager_v1* cursor_shape_manager;
    struct wlr_xdg_decoration_manager_v1 *xdg_decoration_manager;
    struct wlr_virtual_pointer_manager_v1 *virtual_pointer_manager;
    struct wlr_server_decoration_manager *server_decoration_manager;
    struct wlr_relative_pointer_manager_v1 *relative_pointer_manager;
    struct wlr_primary_selection_v1_device_manager *primary_selection_device_manager;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager_v1;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wlr_output_manager_v1 *output_manager_v1;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wlr_viewporter *viewporter;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xwayland *xwayland;
    struct wlr_cursor *cursor;
    struct wlr_scene *scene;
    struct wlr_seat *seat;

    struct wl_list outputs;
    struct wl_list keyboards;
    struct wl_list windows;
    bool zorder, framing;

    struct {
        double x, y;
        struct wlr_box geobox;
        struct wsland_window *window;
    } grab;

    struct {
        uint32_t resize_edges;
        wsland_cursor_mode mode;
    } move;

    struct {
        bool dirty, restore;
        int hotspot_x, hotspot_y;
        struct wlr_texture *texture;
        struct wlr_swapchain *swapchain;
    } wsland_cursor;

    struct {
        struct wl_listener new_output;
        struct wl_listener new_input;
        struct wl_listener cursor_axis;
        struct wl_listener cursor_frame;
        struct wl_listener cursor_motion;
        struct wl_listener cursor_button;
        struct wl_listener new_constraint;
        struct wl_listener request_cursor;
        struct wl_listener request_set_shape;
        struct wl_listener request_set_selection;
        struct wl_listener cursor_motion_absolute;
        struct wl_listener seat_keyboard_focus_change;
        struct wl_listener new_toplevel_decoration;
        struct wl_listener new_server_decoration;
        struct wl_listener new_virtual_pointer;

        struct wl_listener wayland_new_toplevel;

        struct wl_listener xwayland_new_toplevel;
        struct wl_listener xwayland_ready;

        struct wl_signal wsland_cursor_frame;
        struct wl_signal wsland_window_frame;
        struct wl_signal wsland_window_motion;
        struct wl_signal wsland_window_destroy;
    } events;

    wsland_config *config;
    wsland_server_handle *handle;
} wsland_server;

void wayland_event_init(wsland_server *server);
void xwayland_event_init(wsland_server *server);
wsland_server_handle *wsland_server_handle_init(wsland_server *server);

wsland_output *wsland_output_create(wsland_server *server, int width, int height);
void output_update_refresh(wsland_output *output, int32_t refresh);
bool wlr_output_is_wsland(struct wlr_output *wlr_output);

wsland_server *wsland_server_create(wsland_config *config);
void wsland_server_running(wsland_server *server);
void wsland_server_destroy(wsland_server *server);
#endif
