#ifndef WSLAND_SERVER_H
#define WSLAND_SERVER_H

#include <wlr/backend.h>

#define WSLAND_DEFAULT_REFRESH (66 * 1000) // 60 Hz
#define LISTEN(E, L, H) wl_signal_add((E), ((L)->notify = (H), (L)))

struct wsland_config;
struct wsland_server;
struct wsland_adapter;

enum wsland_cursor_mode {
    WSLAND_CURSOR_PASSTHROUGH,
    WSLAND_CURSOR_PRESSED,
    WSLAND_CURSOR_RESIZE,
    WSLAND_CURSOR_MOVE,
};

struct wsland_server_service {
    void (*backend_new_output)(struct wl_listener *listener, void *data);
    void (*backend_new_input)(struct wl_listener *listener, void *data);

    void (*cursor_axis)(struct wl_listener *listener, void *data);
    void (*cursor_motion)(struct wl_listener *listener, void *data);
    void (*cursor_motion_absolute)(struct wl_listener *listener, void *data);
    void (*cursor_button)(struct wl_listener *listener, void *data);
    void (*cursor_frame)(struct wl_listener *listener, void *data);
    void (*pointer_new_constraint)(struct wl_listener *listener, void *data);
    void (*cursor_set_shape)(struct wl_listener *listener, void *data);
    void (*seat_set_cursor)(struct wl_listener *listener, void *data);
    void (*seat_set_selection)(struct wl_listener *listener, void *data);
    void (*seat_request_start_drag)(struct wl_listener *listener, void *data);
    void (*set_backend_selection)(struct wl_listener *listener, void *data);
    void (*new_toplevel_decoration)(struct wl_listener *listener, void *data);
    void (*new_server_decoration)(struct wl_listener *listener, void *data);
    void (*virtual_new_pointer)(struct wl_listener *listener, void *data);
    struct wsland_output *(*fetch_output)(struct wsland_server *server, double pos_x, double pos_y);
    struct wsland_window *(*find_toplevel_window)(struct wlr_scene_node *node);
    void (*window_focus_clear)(struct wsland_window *window);
    bool (*window_title_reset)(struct wsland_window *window);
    bool (*window_appid_reset)(struct wsland_window *window);
    bool (*window_parent_reset)(struct wsland_window *window);
    bool (*window_position_move)(struct wsland_window *window);
    bool (*window_geometry_resize)(struct wsland_window *window);
    bool (*window_border_accepts_input)(struct wlr_scene_buffer *buffer, double *sx, double *sy);
    void (*window_border_color)(struct wsland_window *window, struct wlr_render_color *color);
    void (*window_render_detect)(struct wsland_window *window);
    void (*window_dest_scale)(struct wsland_window *window);

    void (*scale_box)(struct wsland_server *server, struct wlr_box *box, float scale);
    void (*scale_fbox)(struct wsland_server *server, struct wlr_fbox *fbox, float scale);
    void (*begin_window_interactive)(struct wsland_window *window, enum wsland_cursor_mode mode, uint32_t edges);
    void (*dispatch_window_focus)(struct wsland_window *window);

    void (*process_keyboard_key)(struct wsland_keyboard *keyboard, struct wlr_keyboard_key_event *event);
    void (*process_cursor_motion)(struct wsland_server *server, struct wlr_pointer_motion_absolute_event *event);
    void (*process_cursor_button)(struct wsland_server *server, struct wlr_pointer_button_event *event);
    void (*process_cursor_axis)(struct wsland_server *server, struct wlr_pointer_axis_event *event);

    void (*window_surface_register)(struct wlr_surface *surface, struct wsland_window *window);

    void (*window_kill_action)(struct wsland_server *server);
    void (*window_center_action)(struct wsland_server *server);
    void (*window_maximize_action)(struct wsland_server *server);
    void (*window_fullscreen_action)(struct wsland_server *server);
    void (*window_column_maximize_action)(struct wsland_server *server);
};

struct wsland_window_handle {
    char* (*fetch_title)(struct wsland_window *window);
    char* (*fetch_appid)(struct wsland_window *window);
    struct wlr_box (*fetch_geometry)(struct wsland_window *window);

    bool (*fetch_activate)(struct wsland_window *window);
    struct wlr_box (*fetch_bounds)(struct wsland_window *window);
    struct wlr_surface* (*fetch_surface)(struct wsland_window *window);
    struct wsland_window* (*fetch_parent)(struct wsland_window *window);
    struct wsland_output* (*fetch_output)(struct wsland_window *window);
    void (*fetch_render_position)(struct wsland_window *window, int sx, int sy, int *dx, int *dy);
    void (*fetch_offset)(struct wsland_window *window, int *offset_x, int *offset_y);
    void (*window_resize)(struct wsland_window *window, struct wlr_box geometry);
    void (*window_motion)(struct wsland_window *window, int pos_x, int pos_y);
    void (*window_activate)(struct wsland_window *window, bool enabled);
    void (*window_surface_position)(struct wsland_window *window, double *sx, double *sy);
    void (*window_scene_position)(struct wsland_window *window, int px, int py);
    bool (*window_have_above)(struct wsland_window *window);
    bool (*window_have_border)(struct wsland_window *window);
    bool (*window_click_cannot)(struct wsland_window *window);
    bool (*window_grab_cannot)(struct wsland_window *window);
    void (*window_half_output)(struct wsland_window *window);
    void (*window_grab_reset)(struct wsland_window *window);
    void (*window_fullscreen)(struct wsland_window *window);
    void (*window_maximize)(struct wsland_window *window);
    void (*window_minimize)(struct wsland_window *window);
    void (*window_shutdown)(struct wsland_window *window);
    void (*window_column)(struct wsland_window *window);
    void (*window_center)(struct wsland_window *window);
};

struct wsland_server {
    const char *socket_name;
    struct wl_display *display;
    struct wl_event_loop *dispatch;

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
    struct wlr_fractional_scale_manager_v1 *fractional_scale_manager;
    struct wlr_xdg_output_manager_v1 *xdg_output_manager_v1;
    struct wlr_pointer_constraints_v1 *pointer_constraints;
    struct wlr_pointer_constraint_v1 *active_constraint;
    struct wlr_output_manager_v1 *output_manager_v1;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_output_layout *output_layout;
    struct wlr_layer_shell_v1 *layer_shell;
    struct wlr_viewporter *viewporter;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xwayland *xwayland;
    struct wlr_cursor *cursor;
    struct wlr_seat *seat;

    struct wlr_scene *scene;
    struct wlr_scene_tree *scene_top;
    struct wlr_scene_tree *scene_apps;
    struct wlr_scene_tree *scene_float;
    struct wlr_scene_tree *scene_bottom;
    struct wlr_scene_tree *scene_overlay;
    struct wlr_scene_tree *scene_grab;

    struct wl_list outputs;
    struct wl_list keyboards;
    struct wl_list windows;

    struct {
        double x, y;
        struct wlr_box geobox;
        struct wsland_window *window;

        uint32_t resize_edges;
        enum wsland_cursor_mode mode;
        bool restore;
    } grab;

    struct {
        struct wl_listener backend_new_output;
        struct wl_listener backend_new_input;

        struct wl_listener cursor_axis;
        struct wl_listener cursor_frame;
        struct wl_listener cursor_motion;
        struct wl_listener cursor_button;
        struct wl_listener new_constraint;
        struct wl_listener request_cursor;
        struct wl_listener request_set_shape;
        struct wl_listener backend_set_selection;
        struct wl_listener request_set_selection;
        struct wl_listener cursor_motion_absolute;
        struct wl_listener new_toplevel_decoration;
        struct wl_listener new_server_decoration;
        struct wl_listener new_virtual_pointer;
        struct wl_listener request_start_drag;
        struct wl_listener start_drag;

        struct wl_listener layer_new_surface;

        struct wl_listener wayland_new_toplevel;
        struct wl_listener wayland_new_popup;

        struct wl_listener xwayland_new_toplevel;
        struct wl_listener xwayland_ready;
    } events;

    struct wsland_config *config;
    struct wsland_adapter *adapter;
    const struct wsland_server_service *service;
};

struct wsland_output {
    struct wlr_output output;
    struct wsland_server *server;
    struct wlr_scene_output *scene_output;
    struct wlr_box full_area;
    struct wlr_box max_area;

    struct {
        float desktop;
        float device;
    } scale;

    struct {
        struct wl_listener frame;
        struct wl_listener destroy;
    } events;

    bool frame_pending;
    struct wl_event_source *frame_timer;
    int frame_delay; // ms
    bool primary;

    struct wl_list link;
};

struct wsland_pointer_constraint {
    struct wlr_pointer_constraint_v1 *constraint;
    struct wl_listener destroy;

    struct wsland_server *server;
};

void dnd_event_init(struct wsland_server *server);
void layer_event_init(struct wsland_server *server);
void wayland_event_init(struct wsland_server *server);
void xwayland_event_init(struct wsland_server *server);
void wsland_server_service_init(struct wsland_server *server);

struct wsland_server *wsland_server_create(struct wsland_config *config, struct wsland_adapter *adapter);
void wsland_server_running(struct wsland_server *server);
void wsland_server_destroy(struct wsland_server *server);
#endif //WSLAND_SERVER_H
