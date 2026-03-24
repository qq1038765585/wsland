#include <unistd.h>
#include <linux/input-event-codes.h>

#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/edges.h>
#include <wlr/xcursor.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


static void dispatch_window_focus(wsland_window *window) {
    if (!window || !window->tree) {
        return;
    }

    wsland_server *server = window->server;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *cur_surface = window->handle->fetch_surface(window);
    if (prev_surface == cur_surface) {
        return;
    }

    if (prev_surface) {
        window->handle->surface_activate(prev_surface, false);
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&window->tree->node);

    wl_list_remove(&window->server_link);
    wl_list_insert(&server->windows, &window->server_link);
    window->handle->window_activate(window, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(
            seat, cur_surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers
        );
    }

    wsland_window *temp;
    wl_list_for_each(temp, &window->children, parent_link) {
        dispatch_window_focus(temp);
    }
}

static void output_frame(struct wl_listener *listener, void *data) {
    wsland_output *output = wl_container_of(listener, output, events.frame);

    pixman_region32_copy(&output->pending_commit_damage, &output->scene_output->pending_commit_damage);
    pixman_region32_translate(&output->pending_commit_damage, output->scene_output->x, output->scene_output->y);
    if (pixman_region32_not_empty(&output->pending_commit_damage)) {
        wl_signal_emit(&output->server->events.wsland_window_frame, output);
        pixman_region32_clear(&output->scene_output->pending_commit_damage);
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    wsland_output *output = wl_container_of(listener, output, events.destroy);

    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->events.destroy.link);
    wl_list_remove(&output->events.frame.link);
}

static void server_new_output(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_output);
    wsland_output *output = data;
    output->server = server;

    wlr_output_init_render(&output->output, server->allocator, server->renderer);

    LISTEN(&output->output.events.frame, &output->events.frame, output_frame);
    LISTEN(&output->output.events.destroy, &output->events.destroy, output_destroy);
    wl_list_insert(&server->outputs, &output->server_link);

    output->scene_output = wlr_scene_output_create(server->scene, &output->output);
    struct wlr_output_layout_output *layout_output = wlr_output_layout_add_auto(server->output_layout, &output->output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, output->scene_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_commit_state(&output->output, &state);
    wlr_output_state_finish(&state);

    if (server->config->command) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", server->config->command, (void*)NULL);
        }
    }
}

static bool alt_keybinding(wsland_server *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_F1:
        {
            if (wl_list_length(&server->windows) < 2) {
                break;
            }
            wsland_window *next_window = wl_container_of(server->windows.prev, next_window, server_link);
            dispatch_window_focus(next_window);
        }
        break;
    default:
        return false;
    }
    return true;
}

static bool alt_shift_keybinding(wsland_server *server, xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Q: {
            struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

            if (ws && ws->data && server->move.mode == WSLAND_CURSOR_PASSTHROUGH) {
                wsland_window *window = ws->data;
                window->handle->window_shutdown(window);
            }
        }
        break;
        case XKB_KEY_C: {
            struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

            if (ws && ws->data) {
                wsland_window *window = ws->data;
                window->handle->window_center(window);
            }
        }
        break;
        case XKB_KEY_F: {
            struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

            if (ws && ws->data) {
                wsland_window *window = ws->data;
                window->handle->window_maximize(window);
            }
        }
        break;
    default:
        return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.key);
    wsland_server *server = keyboard->server;

    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server->seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->keyboard.xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(&keyboard->keyboard);
    if (modifiers & WLR_MODIFIER_ALT && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = alt_keybinding(server, syms[i]);
        }
    }
    if (modifiers & WLR_MODIFIER_ALT && modifiers & WLR_MODIFIER_SHIFT && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = alt_shift_keybinding(server, syms[i]);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, &keyboard->keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.modifiers);

    wlr_seat_set_keyboard(keyboard->server->seat, &keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard.modifiers);
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.destroy);
    wl_list_remove(&keyboard->events.modifiers.link);
    wl_list_remove(&keyboard->events.destroy.link);
    wl_list_remove(&keyboard->events.key.link);
    wl_list_remove(&keyboard->server_link);
}

static void server_new_keyboard(wsland_server *server, struct wlr_input_device *device) {
    wsland_keyboard *keyboard = device->data;
    if (!keyboard) {
        return;
    }

    keyboard->server = server;
    wlr_seat_set_keyboard(server->seat, &keyboard->keyboard);

    LISTEN(&keyboard->keyboard.events.key, &keyboard->events.key, keyboard_handle_key);
    LISTEN(&keyboard->keyboard.events.modifiers, &keyboard->events.modifiers, keyboard_handle_modifiers);
    LISTEN(&device->events.destroy, &keyboard->events.destroy, keyboard_handle_destroy);
    wl_list_insert(&server->keyboards, &keyboard->server_link);
}

static void server_new_pointer(wsland_server *server, struct wlr_input_device *device) {
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_input);
    struct wlr_input_device *device = data;

    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            server_new_keyboard(server, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            server_new_pointer(server, device);
            break;
        default:
            break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server->keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server->seat, caps);
}

static void begin_window_interactive(wsland_window *window, wsland_cursor_mode mode, uint32_t edges) {
    wsland_server *server = window->server;
    server->grab.window = window;
    server->move.mode = mode;

    if (mode == WSLAND_CURSOR_MOVE) {
        server->grab.x = server->cursor->x - window->tree->node.x;
        server->grab.y = server->cursor->y - window->tree->node.y;

        server->wsland_cursor.s_hotspot_x = server->wsland_cursor.b_hotspot_x;
        server->wsland_cursor.s_hotspot_y = server->wsland_cursor.b_hotspot_y;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "move");
    }
    else {
        if (edges == WLR_EDGE_NONE) {
            if (server->cursor->x < window->current.x + window->current.width / 2) {
                edges |= WLR_EDGE_LEFT;
            }
            else {
                edges |= WLR_EDGE_RIGHT;
            }
            if (server->cursor->y < window->current.y + window->current.height / 2) {
                edges |= WLR_EDGE_TOP;
            }
            else {
                edges |= WLR_EDGE_BOTTOM;
            }
        }

        server->wsland_cursor.s_hotspot_x = server->wsland_cursor.b_hotspot_x;
        server->wsland_cursor.s_hotspot_y = server->wsland_cursor.b_hotspot_y;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, wlr_xcursor_get_resize_name(edges));

        struct wlr_box *geo_box = window->handle->fetch_geometry(window);

        double border_x = window->tree->node.x + geo_box->x + (edges & WLR_EDGE_RIGHT ? geo_box->width : 0);
        double border_y = window->tree->node.y + geo_box->y + (edges & WLR_EDGE_BOTTOM ? geo_box->height : 0);
        server->grab.x = server->cursor->x - border_x;
        server->grab.y = server->cursor->y - border_y;

        server->grab.geobox = *geo_box;
        server->grab.geobox.x += window->tree->node.x;
        server->grab.geobox.y += window->tree->node.y;
        server->move.resize_edges = edges;
    }
}

static wsland_window* desktop_toplevel_at(
    wsland_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
) {
    struct wlr_scene_node *node = wlr_scene_node_at(&server->scene->tree.node, lx, ly, sx, sy);
    if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return NULL;
    }

    *surface = scene_surface->surface;
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }

    if (tree) {
        return tree->node.data;
    }
    return NULL;
}

static void reset_server_cursor(wsland_server *server) {
    if (server->move.mode != WSLAND_CURSOR_PASSTHROUGH) {
        struct wlr_surface *surface = server->wsland_cursor.surface;
        bool non_xwayland = server->grab.window->type != XWAYLAND;

        if (non_xwayland && surface && surface->mapped) {
            wlr_cursor_set_surface(
                server->cursor, server->wsland_cursor.surface,
                server->wsland_cursor.s_hotspot_x, server->wsland_cursor.s_hotspot_y
            );
        } else {
            wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
        }
    }

    server->move.mode = WSLAND_CURSOR_PASSTHROUGH;
    server->grab.window = NULL;
}

static void process_cursor_move(wsland_server *server) {
    wsland_window *window = server->grab.window;

    if (window->handle->window_grab_cannot(window)) {
        return;
    }

    int pos_x = server->cursor->x - server->grab.x;
    int pos_y = server->cursor->y - server->grab.y;
    window->handle->window_motion(window, pos_x, pos_y);
}

static void process_cursor_resize(wsland_server *server) {
    wsland_window *window = server->grab.window;

    if (window->handle->window_grab_cannot(window)) {
        return;
    }

    double border_x = server->cursor->x - server->grab.x;
    double border_y = server->cursor->y - server->grab.y;
    int new_left = server->grab.geobox.x;
    int new_right = server->grab.geobox.x + server->grab.geobox.width;
    int new_top = server->grab.geobox.y;
    int new_bottom = server->grab.geobox.y + server->grab.geobox.height;

    if (server->move.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    }
    else if (server->move.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->move.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    }
    else if (server->move.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box *geo_box = window->handle->fetch_geometry(window);
    wlr_scene_node_set_position(&window->tree->node, new_left - geo_box->x, new_top - geo_box->y);

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    {
        new_width = MAX(1 - geo_box->x, new_width);
        new_height = MAX(1 - geo_box->y, new_height);

        if (window->handle) {
            wsland_output *output = window->handle->fetch_output(window);
            if (output) {
                new_width = MIN(output->monitor.width, new_width);
                new_height = MIN(output->monitor.height, new_height);
            }
        }
    }

    window->handle->window_resize(window, new_width, new_height);
}

static void process_cursor_motion(wsland_server *server, uint32_t time) {
    if (server->move.mode == WSLAND_CURSOR_MOVE) {
        process_cursor_move(server);
        return;
    }
    if (server->move.mode == WSLAND_CURSOR_RESIZE) {
        process_cursor_resize(server);
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    wsland_window *window = desktop_toplevel_at(
        server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
    );
    if (!window) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    }
    if (surface) {
        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, time, sx, sy);
    }
    else {
        wlr_seat_pointer_clear_focus(seat);
    }
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
    // todo
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_closest(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(server, event->time_msec);
}

static void request_decoration_mode(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_decoration_mode);

    if (window->wayland->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            window->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
    }
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.decoration_destroy);

    wl_list_remove(&window->events.decoration_destroy.link);
    wl_list_remove(&window->events.request_decoration_mode.link);
}

static void server_new_toplevel_decoration(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.new_toplevel_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

    struct wlr_scene_tree *tree = decoration->toplevel->base->data;
    if (tree && tree->node.data) {
        wsland_window *window = tree->node.data;
        window->decoration = decoration;

        LISTEN(&decoration->events.request_mode, &window->events.request_decoration_mode, request_decoration_mode);
        LISTEN(&decoration->events.destroy, &window->events.decoration_destroy, decoration_destroy);
    }
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_button);

    bool handled = false;
    struct wlr_pointer_button_event *event = data;
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_server_cursor(server);
    } else {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        wsland_window *window = desktop_toplevel_at(
            server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
        );

        if (window && window->handle->window_click_cannot(window)) {
            wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
            return;
        }

        dispatch_window_focus(window);
        if (window && (event->button == BTN_LEFT || event->button == BTN_RIGHT)) {
            uint32_t modifiers = wlr_keyboard_get_modifiers(server->seat->keyboard_state.keyboard);

            if (modifiers & WLR_MODIFIER_ALT) {
                switch (event->button) {
                    case BTN_LEFT: {
                        begin_window_interactive(window, WSLAND_CURSOR_MOVE, 0);
                        handled = true;
                    }
                    break;
                    case BTN_RIGHT: {
                        enum wlr_edges edges = WLR_EDGE_NONE;
                        begin_window_interactive(window, WSLAND_CURSOR_RESIZE, edges);
                        handled = true;
                    }
                    break;
                    default: break;
                }
            }
        }
    }

    if (!handled) {
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    wlr_seat_pointer_notify_axis(
        server->seat, event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction
    );
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.cursor_frame);

    wlr_seat_pointer_notify_frame(server->seat);
    wl_signal_emit(&server->events.wsland_cursor_frame, server);
}

static void cursor_surface_destroy(struct wl_listener *listener, void *user_data) {
    wsland_server *server = wl_container_of(listener, server, wsland_cursor.destroy);

    if (user_data == server->wsland_cursor.surface) {
        wl_list_remove(&server->wsland_cursor.destroy.link);
        wl_list_init(&server->wsland_cursor.destroy.link);
        server->wsland_cursor.surface = NULL;
    }
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    struct wlr_seat_client *focused_client = server->seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        if (server->wsland_cursor.surface) {
            wl_list_remove(&server->wsland_cursor.destroy.link);
            wl_list_init(&server->wsland_cursor.destroy.link);
        }

        if (event->surface && event->surface != server->wsland_cursor.surface) {
            server->wsland_cursor.surface = event->surface;
            server->wsland_cursor.s_hotspot_x = event->hotspot_x;
            server->wsland_cursor.s_hotspot_y = event->hotspot_y;
            LISTEN(&event->surface->events.destroy, &server->wsland_cursor.destroy, cursor_surface_destroy);
        }

        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *user_data) {
    wsland_server *server = wl_container_of(listener, server, events.request_set_selection);
    struct wlr_seat_request_set_selection_event *event = user_data;

    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

wsland_server_handle wsland_server_handle_impl = {
    .new_output = server_new_output,
    .new_input = server_new_input,
    .cursor_axis = server_cursor_axis,
    .cursor_frame = server_cursor_frame,
    .cursor_motion = server_cursor_motion,
    .cursor_button = server_cursor_button,
    .cursor_motion_absolute = server_cursor_motion_absolute,
    .new_toplevel_decoration = server_new_toplevel_decoration,
    .seat_request_selection = seat_request_set_selection,
    .seat_request_cursor = seat_request_cursor,

    .reset_server_cursor = reset_server_cursor,
    .dispatch_window_focus = dispatch_window_focus,
    .begin_window_interactive = begin_window_interactive,
};

wsland_server_handle *wsland_server_handle_init(wsland_server *server) {
    return &wsland_server_handle_impl;
}