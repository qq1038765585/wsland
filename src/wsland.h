#ifndef WSLAND_H
#define WSLAND_H

#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/backend.h>

#include <freerdp/listener.h>
#include <rdpapplist/rdpapplist_server.h>
#include <freerdp/server/gfxredir.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/disp.h>
#include <wayland-server-core.h>

#define RDP_MAX_MONITOR 16
#define MAX_FREERDP_FDS 32
#define RDP_DEFAULT_REFRESH (60 * 1000) // 60 Hz

#define RDP_RAIL_MARKER_WINDOW_ID  0xFFFFFFFE
#define RDP_RAIL_DESKTOP_WINDOW_ID 0xFFFFFFFF

#define WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, arg_type, arg, callback) { \
    struct wsland_peer_context *peer_ctx = (context)->custom; \
    struct wsland_rail_dispatch_data *dispatch_data; \
    dispatch_data = malloc(sizeof(*dispatch_data)); \
    dispatch_data->peer_ctx = peer_ctx; \
    dispatch_data->arg_type = *(arg); \
    wsland_freerdp_dispatch_to_display_loop(peer_ctx, callback, &dispatch_data->task_base); \
}

typedef void (*wsland_loop_task_func_t)(bool free_only, void *data);
typedef void (*hash_table_iterator_func_t)(void *element, void *data);

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

struct wsland_freerdp {
    struct rdp_freerdp_listener *listener;
    struct wl_event_source *listener_events[MAX_FREERDP_FDS];
    struct wsland_peer_context *peer_ctx;

    int port;
    char *address;
    char *tls_key_content;
    char *tls_cert_content;

    bool enable_window_snap_arrange;
    bool enable_window_shadow_remoting;

    bool enable_hi_dpi_support;
    bool enable_fractional_hi_dpi_support;
    bool enable_fractional_hi_dpi_roundup;
    uint32_t debug_desktop_scaling_factor; /* must be between 100 to 500 */

    const struct wsland_freerdp_shell_api *freerdp_shell_api;
    void *rail_shell_context;
    char *rail_shell_name;
    bool use_gfxredir;
};

struct wsland_server {
    char *command;
    const char *socket_name;
    pid_t compositor_tid;

    struct wl_display *wl_display;
    struct wl_event_loop *event_loop;

    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;
    struct wlr_compositor *compositor;
    struct wlr_subcompositor *subcompositor;
    struct wlr_data_device_manager *data_device_manager;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *scene_background;
    struct wsland_output *primary_output;
    struct wlr_scene *scene;

    struct wlr_xdg_shell *xdg_shell;
    struct wl_listener new_xdg_toplevel;
    struct wl_listener new_xdg_popup;

    struct wlr_cursor *cursor;
    struct wlr_buffer *cursor_buffer;
    struct wlr_xcursor_manager *cursor_mgr;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_motion;
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

    struct wsland_freerdp freerdp;
};

struct wsland_pointer {
    struct wlr_pointer pointer;

    struct wl_listener destroy;
};

struct wsland_keyboard {
    struct wl_list link;

    struct wlr_keyboard keyboard;

    struct wl_listener modifiers;
    struct wl_listener destroy;
    struct wl_listener key;
};

struct wsland_loop_task {
    struct wl_list link;
    struct wsland_peer_context *client_context;
    wsland_loop_task_func_t func;
};

struct wsland_rail_dispatch_data {
    struct wsland_loop_task task_base;
    struct wsland_peer_context *peer_ctx;

    union {
        RAIL_EXEC_ORDER exec;
        RAIL_ACTIVATE_ORDER activate;
        RAIL_SNAP_ARRANGE snap_arrange;
        RAIL_SYSCOMMAND_ORDER sys_command;
        RAIL_GET_APPID_REQ_ORDER get_appid_req;
        RAIL_LANGUAGEIME_INFO_ORDER language_ime_info;
        RDPAPPLIST_CLIENT_CAPS_PDU app_list_caps;
        RAIL_WINDOW_MOVE_ORDER window_move;
        RAIL_SYSPARAM_ORDER sys_param;
        RAIL_SYSMENU_ORDER sys_menu;
    };
};

struct wsland_peer_context {
    struct rdp_context rdp_ctx;

    struct wl_event_source *events[MAX_FREERDP_FDS];
    struct rdp_freerdp_peer *peer;

    struct wsland_output *output;
    struct wsland_pointer *pointer;
    struct wsland_keyboard *keyboard;

    HANDLE vcm;
    uint32_t flags; // wlr_rdp_peer_flags

    RailServerContext *ctx_server_rail;
    DrdynvcServerContext *ctx_server_drdynvc;
    DispServerContext *ctx_server_disp;
    RdpgfxServerContext *ctx_server_rdpgfx;
    GfxRedirServerContext *ctx_server_gfxredir;
    RdpAppListServerContext *ctx_server_applist;

    int loop_task_event_source_fd;
    pthread_mutex_t loop_task_list_mutex;
    struct wl_event_source *loop_task_event_source;
    struct wl_list loop_task_list; // struct wsland_loop_task::link

    // Multiple monitor support (monitor topology)
    int32_t desktop_top, desktop_left, desktop_width, desktop_height;

    uint32_t client_status_flags;

    uint32_t current_frame_id;
    uint32_t acknowledged_frame_id;
    bool is_acknowledged_suspended;

    bool handshake_completed;
    bool activation_rail_completed;
    bool activation_graphics_completed;
    bool activation_graphics_redirection_completed;
    bool mouse_button_swap;

    BOOL is_applist_enabled;

    struct wl_client *client_exec;
    struct wl_listener client_exec_destroy_listener;
};

struct wsland_surface {
    struct wlr_surface *wlr_surface;

    bool is_window_snapped;
    bool is_update_pending;
    bool force_recreate_surface;
    bool force_update_window_state;

    uint32_t window_margin_top;
    uint32_t window_margin_left;
    uint32_t window_margin_right;
    uint32_t window_margin_bottom;

    struct wl_listener destroy;
};

struct wsland_output {
    struct wl_list link;

    struct wlr_output *base;
    struct wlr_scene_output *scene_output;
    struct wl_list toplevels; // wsland_toplevel.link

    struct wlr_box desktop_box;
    struct wlr_box work_box;

    struct wl_listener frame;
    struct wl_listener destroy;
};

struct wsland_toplevel {
    struct wl_list link;
    struct wlr_xdg_toplevel *xdg_toplevel;
    struct wlr_scene_tree *scene_tree;

    struct {
        uint32_t window_id, surface_id;
        int scale_width, scale_height;
        struct wlr_box buffer_box;

        pixman_region32_t surface_damage;
        struct wlr_buffer *surface_buffer;

        bool is_cursor;
        bool force_recreate_surface;
    } state;

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

struct wsland_freerdp_shell_api {
    void (*request_window_close)(struct wlr_surface *surface);
    void (*request_window_minimize)(struct wlr_surface *surface);
    void (*request_window_maximize)(struct wlr_surface *surface);
    void (*request_window_move)(struct wlr_surface *surface, int x, int y, int width, int height);
    void (*request_window_restore)(struct wlr_surface *surface);
    void (*request_window_activate)(void *shell_context, struct wlr_seat *seat, struct wlr_surface *surface);
    struct wl_client* (*request_launch_shell_process)(void *shell_context, char *exec_name);
    pid_t (*get_window_app_id)(void *shell_context, struct wlr_surface *surface, char *app_id, size_t app_id_size, char *image_name, size_t image_name_size);
    void (*set_desktop_work_area)(struct wlr_output *output, void *context, pixman_rectangle32_t *work_area);
    void (*request_window_snap)(struct wlr_surface *surface, int x, int y, int width, int height);
    void (*get_window_geometry)(struct wlr_surface *surface, struct wlr_box *geometry);
    bool (*start_app_list_update)(void *shell_context, char *client_language_id);
    void (*stop_app_list_update)(void *shell_context);
};

struct xkb_rule_names fetch_xkb_rule_names(struct rdp_settings *settings);
void wsland_freerdp_rail_client_language_ime_info_callback(bool free_only, void *arg);


bool wsland_freerdp_ctx_server_drdynvc_init(struct wsland_peer_context *peer_ctx);
bool wsland_freerdp_ctx_server_rail_init(struct wsland_peer_context *peer_ctx);
bool wsland_freerdp_ctx_server_disp_init(struct wsland_peer_context *peer_ctx);
bool wsland_freerdp_ctx_server_rdpgfx_init(struct wsland_peer_context *peer_ctx);
bool wsland_freerdp_ctx_server_gfxredir_init(struct wsland_peer_context *peer_ctx);
bool wsland_freerdp_ctx_server_applist_init(struct wsland_peer_context *peer_ctx);

bool wsland_freerdp_surface_cursor_update(struct wlr_cursor *cursor);
bool wsland_freerdp_surface_create(struct wsland_toplevel *toplevel);
bool wsland_freerdp_surface_update(struct wsland_toplevel *toplevel);
bool wsland_freerdp_surface_destroy(struct wsland_toplevel *toplevel);
void wsland_freerdp_surface_output(struct wsland_output *output);

struct wsland_toplevel * wsland_freerdp_instance_loop_wsland_surface(struct wsland_peer_context *peer_ctx, uint32_t window_id);
int wsland_freerdp_instance_new_surface(void);
int wsland_freerdp_instance_new_window(void);


void wsland_freerdp_dispatch_to_display_loop(struct wsland_peer_context *peer_ctx, wsland_loop_task_func_t func, struct wsland_loop_task *task);
bool wsland_freerdp_dispatch_task_init(struct wsland_peer_context *peer_ctx);

const struct wsland_freerdp_shell_api * wsland_freerdp_shell_api_init(void);

bool wsland_freerdp_init(void);
void wsland_freerdp_tls_generate(void);
void wsland_freerdp_destroy(void);

bool wsland_server_init(int argc, char *argv[]);
extern struct wsland_server server;
#endif
