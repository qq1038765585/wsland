#ifndef WSLAND_ADAPTER_H
#define WSLAND_ADAPTER_H

#include <stdatomic.h>
#include <freerdp/channels/rdpgfx.h>
#include <freerdp/channels/gfxredir.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_scene.h>
#include <freerdp/settings.h>
#include <wlr/util/box.h>

#define BACKEND_WINDOW_MARKER_ID  0xFFFFFFFE
#define BACKEND_WINDOW_DESKTOP_ID 0xFFFFFFFF
#define BACKEND_SHARED_MEMORY_NAME_SIZE (32 + 4 + 2)

#define BACKEND_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define BACKEND_WINDOW_NORMAL_STYLE (BACKEND_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

struct wsland_server;
struct wsland_output;
struct wsland_window;
struct wsland_backend;
struct wsland_adapter;
struct wsland_pointer;
struct wsland_keyboard;
struct wsland_shared_memory;
struct backend_client;

enum window_type {
    ICON, LAYER, XDG, X11
};

enum window_role {
    TOPLEVEL, POPUP
};

struct wsland_shared_memory {
    int fd;
    void *addr;
    size_t size;
    char name[BACKEND_SHARED_MEMORY_NAME_SIZE + 1];
};

struct wsland_adapter_handle {
    struct wlr_seat *(*server_seat_fetch)(struct wsland_server *server);
    void (*server_seat_source_config)(struct wsland_server *server, struct wlr_data_source *source);
    void (*backend_selection_source)(struct wsland_adapter *adapter, struct wlr_seat *seat);
    BOOL (*backend_adjust_monitor_layout)(struct wsland_adapter *adapter, int count, rdpMonitor *monitors);
    void (*backend_source_release)(struct wsland_adapter *adapter, struct wsland_pointer *pointer, struct wsland_keyboard *keyboard);

    struct wsland_window *(*find_bottom_window)(struct wsland_window *window, struct wl_list *children);
    struct wsland_output *(*server_output_from_point)(struct wsland_adapter *adapter, uint16_t x, uint16_t y);
    struct wl_event_source *(*server_dispatch_event_source)(struct wsland_adapter *adapter, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data);
    void (*server_remove_event_source)(struct wl_event_source *source);

    struct wsland_pointer *(*backend_pointer_init)(struct wsland_adapter *adapter);
    struct wsland_keyboard *(*backend_keyboard_init)(struct wsland_adapter *adapter);
    void (*backend_input_synchronize)(struct wsland_adapter *adapter, uint32_t flags);

    void (*backend_rdpgfx_frame_acknowledge)(struct backend_client *client, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge);
    void (*backend_gfxredir_frame_acknowledge)(struct backend_client *client, const GFXREDIR_PRESENT_BUFFER_ACK_PDU *present_buffer_ack);
    void (*backend_rail_client_activate)(bool free_only, void *user_data);
    void (*backend_rail_client_sysparam)(bool free_only, void *user_data);
    void (*backend_rail_client_syscommand)(bool free_only, void *user_data);
    void (*backend_rail_client_appid_response)(bool free_only, void *user_data);

    void (*backend_keyboard_key)(struct wsland_keyboard *keyboard, struct wlr_keyboard_key_event *event);
    void (*backend_mouse_button)(struct wlr_pointer_button_event *event);
    void (*backend_mouse_frame)(struct wsland_adapter *adapter);
    void (*backend_mouse_move)(struct wsland_adapter *adapter, struct wlr_pointer_motion_absolute_event *event);
    void (*backend_mouse_axis)(struct wsland_adapter *adapter, struct wlr_pointer_axis_event *event);

    void (*backend_cursor_update)(struct wsland_output *output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y);

    void (*backend_output_frame)(struct wsland_adapter *adapter);
    void (*backend_window_maximize)(struct wsland_window *window, bool display);
    void (*backend_window_minimize)(struct wsland_window *window, bool display);
    void (*backend_window_title)(struct wsland_window *window);
    void (*backend_window_appid)(struct wsland_window *window);
    void (*backend_window_create)(struct wsland_window *window);
    void (*backend_window_destroy)(struct wsland_window *window);
    void (*backend_window_reconnect)(struct wsland_adapter *adapter);
    void (*backend_window_icon_sync)(struct wsland_window *window);
    void (*backend_window_buffer_resize)(struct wsland_window *window);
    void (*backend_window_buffer_destroy)(struct wsland_window *window);
    void (*backend_window_resize_detect)(struct wsland_window *window);
    void (*backend_window_metadata_detect)(struct wsland_window *window);
    bool (*backend_shared_memory_new)(struct wsland_adapter *adapter, struct wsland_shared_memory *shared_memory);
    void (*backend_shared_memory_free)(struct wsland_adapter *adapter, struct wsland_shared_memory *shared_memory);
    void (*backend_shared_memory_config)(struct wsland_adapter *adapter);

};

struct wsland_adapter {
    const struct wsland_adapter_handle *handle;
    struct wlr_box area;
    bool reconnect;

    struct wsland_server *server;
    struct wsland_backend *backend;
};

struct wsland_keyboard {
    struct wlr_keyboard keyboard;

    struct {
        struct wl_listener key;
        struct wl_listener modifiers;
        struct wl_listener destroy;
    } events;

    struct wl_list link;
    struct wsland_server *server;
};

struct wsland_pointer {
    struct wlr_pointer pointer;

    struct wsland_server *server;
};

struct wsland_window {
    enum window_type type;
    enum window_role role;
    struct wlr_scene_tree *scene;
    struct wlr_scene_tree *scene_surface;
    struct wlr_scene_layer_surface_v1 *scene_layer;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    struct wlr_scene_buffer *scene_border;
    struct wsland_buffer *buffer_border;
    struct wsland_window *parent;

    struct {
        struct wlr_box geometry, source, target, restore;

        char *title, *appid;
        struct { uint32_t window, surface, buffer, pool; } ids;
        bool mapped, column, above, icon;
        float scale;
    } metadata;

    struct {
        uint32_t resizing;
        atomic_bool center, pending, skip, dirty;
        struct wsland_shared_memory gfx_memory;
        struct wlr_swapchain *swapchain;
        struct wlr_buffer *buffer;
        pixman_region32_t damage;
        void *gfx_buffer;

        struct wlr_render_pass *wrp;
        struct wsland_output *wo;
    } render;

    union {
        struct wlr_xdg_surface *wayland;
        struct wlr_xwayland_surface *xwayland;
        struct wlr_layer_surface_v1 *layer;
        struct wlr_drag_icon *icon;
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
        struct wl_listener set_title;
        struct wl_listener set_appid;
        struct wl_listener set_geometry;
        struct wl_listener request_move;
        struct wl_listener request_resize;
        struct wl_listener request_minimize;
        struct wl_listener request_maximize;
        struct wl_listener request_fullscreen;
        struct wl_listener request_configure;
        struct wl_listener request_activate;

        struct wl_listener request_decoration_mode;
        struct wl_listener decoration_destroy;
    } events;

    struct wl_list link;
    struct wsland_server *server;
    const struct wsland_window_handle *handle;
};

struct window_surface {
    struct wsland_window *window;
    struct wlr_surface *surface;

    struct {
        struct wl_listener unmap;
        struct wl_listener commit;
        struct wl_listener new_subsurface;
        struct wl_listener destroy;
    } events;
};

void wsland_client_event_handle(struct backend_client *client);
void wsland_adapter_handle_init(struct wsland_adapter *adapter);

void wsland_output_reset(void);
bool wlr_output_is_wsland(struct wlr_output *wlr_output);
struct wsland_output *wsland_output_create(struct wsland_server *server, struct wlr_box output_area);

struct wsland_adapter* wsland_adapter_create(void);
void wsland_adapter_destroy(struct wsland_adapter *adapter);
#endif //WSLAND_ADAPTER_H
