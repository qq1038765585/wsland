#include <linux/input-event-codes.h>

#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/transform.h>
#include <wlr/util/region.h>
#include <wlr/util/edges.h>
#include <wlr/xcursor.h>

#include "common/buffer.h"
#include "common/config.h"
#include "common/logger.h"
#include "adapter.h"
#include "server.h"


static void backend_output_frame(struct wl_listener *listener, void *user_data) {
    struct wsland_output *output = wl_container_of(listener, output, events.frame);
    struct wlr_output *wo = &output->output;
    struct timespec now;

    if (output->primary) {
        wo->impl->commit(wo, NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void backend_output_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_output *output = wl_container_of(listener, output, events.destroy);

    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->events.destroy.link);
    wl_list_remove(&output->events.frame.link);
    wl_list_remove(&output->link);
}

static void backend_new_output(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.backend_new_output);

    struct wsland_output *output = user_data;
    output->server = server;

    wlr_output_init_render(&output->output, server->allocator, server->renderer);
    LISTEN(&output->output.events.frame, &output->events.frame, backend_output_frame);
    LISTEN(&output->output.events.destroy, &output->events.destroy, backend_output_destroy);
    wl_list_insert(&server->outputs, &output->link);

    output->scene_output = wlr_scene_output_create(server->scene, &output->output);
    struct wlr_output_layout_output *layout_output = wlr_output_layout_add(server->output_layout, &output->output, output->full_area.x, output->full_area.y);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, output->scene_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_commit_state(&output->output, &state);
    wlr_output_state_finish(&state);
}

static void window_kill_action(struct wsland_server *server) {
    struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

    if (ws && ws->data && server->grab.mode == WSLAND_CURSOR_PASSTHROUGH) {
        struct wlr_scene_tree *tree = ws->data;

        if (tree->node.data) {
            struct wsland_adapter *adapter = server->adapter;
            struct wsland_window *window = tree->node.data;

            if (!window->parent) { adapter->handle->backend_input_synchronize(adapter, 0); }
            window->handle->window_shutdown(window);
        }
    }
}

static void window_center_action(struct wsland_server *server) {
    struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

    if (ws && ws->data) {
        struct wlr_scene_tree *tree = ws->data;

        if (tree->node.data) {
            struct wsland_window *window = tree->node.data;
            window->handle->window_center(window);
        }
    }
}

static void window_maximize_action(struct wsland_server *server) {
    struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

    if (ws && ws->data) {
        struct wlr_scene_tree *tree = ws->data;

        if (tree->node.data) {
            struct wsland_window *window = tree->node.data;
            window->handle->window_maximize(window);
        }
    }
}

static void window_fullscreen_action(struct wsland_server *server) {
    struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

    if (ws && ws->data) {
        struct wlr_scene_tree *tree = ws->data;

        if (tree->node.data) {
            struct wsland_window *window = tree->node.data;
            window->handle->window_fullscreen(window);
        }
    }
}

static void window_column_maximize_action(struct wsland_server *server) {
    struct wlr_surface *ws = server->seat->keyboard_state.focused_surface;

    if (ws && ws->data) {
        struct wlr_scene_tree *tree = ws->data;

        if (tree->node.data) {
            struct wsland_window *window = tree->node.data;
            window->handle->window_column(window);
        }
    }
}

static bool keyboard_binding_handler(struct wsland_server *server, uint32_t modifiers, const xkb_keysym_t sym) {
    for (int idx = 0; idx < server->config->binding_count; ++idx) {
        struct keybinding *binding = &server->config->bindings[idx];

        if (binding->modifiers == modifiers && binding->keysym == sym && binding->action) {
            binding->action(server);
            return true;
        }
    }
    return false;
}

static void process_keyboard_key(struct wsland_keyboard *keyboard, struct wlr_keyboard_key_event *event) {
    struct wsland_server *server = keyboard->server;
    struct wlr_seat *seat = server->seat;

    xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->keyboard.xkb_state, event->keycode + 8);
    sym = xkb_keysym_to_lower(sym);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(&keyboard->keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && modifiers > 0) {
        handled = keyboard_binding_handler(server, modifiers, sym);
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, &keyboard->keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void backend_keyboard_key(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.key);
    struct wsland_server *server = keyboard->server;
    struct wlr_keyboard_key_event *event = data;

    server->service->process_keyboard_key(keyboard, event);
}

static void backend_keyboard_modifiers(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.modifiers);

    wlr_seat_set_keyboard(keyboard->server->seat, &keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard.modifiers);
}

static void backend_keyboard_destroy(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, events.destroy);
    wl_list_remove(&keyboard->events.modifiers.link);
    wl_list_remove(&keyboard->events.destroy.link);
    wl_list_remove(&keyboard->events.key.link);
    wl_list_remove(&keyboard->link);
}

static void backend_new_keyboard(struct wsland_server *server, struct wlr_input_device *device) {
    struct wsland_keyboard *keyboard = device->data;
    if (!keyboard) {
        return;
    }

    keyboard->server = server;
    wlr_seat_set_keyboard(server->seat, &keyboard->keyboard);
    LISTEN(&keyboard->keyboard.events.key, &keyboard->events.key, backend_keyboard_key);
    LISTEN(&keyboard->keyboard.events.modifiers, &keyboard->events.modifiers, backend_keyboard_modifiers);
    LISTEN(&device->events.destroy, &keyboard->events.destroy, backend_keyboard_destroy);
}

static void backend_new_pointer(struct wsland_server *server, struct wlr_input_device *device) {
    struct wsland_pointer *pointer = device->data;
    if (!pointer) {
        return;
    }

    pointer->server = server;
    wlr_cursor_attach_input_device(server->cursor, device);
}

static void backend_new_input(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.backend_new_input);
    struct wlr_input_device *device = user_data;

    switch (device->type) {
        case WLR_INPUT_DEVICE_KEYBOARD:
            backend_new_keyboard(server, device);
            break;
        case WLR_INPUT_DEVICE_POINTER:
            backend_new_pointer(server, device);
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

static void cursor_axis(struct wl_listener *listener, void *data) {
    struct wsland_server *server = wl_container_of(listener, server, events.cursor_axis);
    struct wlr_pointer_axis_event *event = data;

    server->service->process_cursor_axis(server, event);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
    struct wsland_server *server = wl_container_of(listener, server, events.cursor_frame);

    wlr_seat_pointer_notify_frame(server->seat);
}

static void cursor_button(struct wl_listener *listener, void *data) {
    struct wsland_server *server = wl_container_of(listener, server, events.cursor_button);
    struct wlr_pointer_button_event *event = data;

    server->service->process_cursor_button(server, event);
}

static void cursor_motion(struct wl_listener *listener, void *data) {
    server_log(ERROR, "cursor motion need");
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct wsland_server *server = wl_container_of(listener, server, events.cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_closest(server->cursor, &event->pointer->base, event->x, event->y);
    server->service->process_cursor_motion(server, event);
}

static void cursor_set_shape(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.request_set_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = user_data;

    if (server->grab.mode != WSLAND_CURSOR_PASSTHROUGH) {
        return;
    }

    if (event->seat_client == server->seat->pointer_state.focused_client) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, wlr_cursor_shape_v1_name(event->shape));
    }
}

static void seat_set_selection(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.request_set_selection);
    struct wlr_seat_request_set_selection_event *event = user_data;

    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_start_drag(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.request_start_drag);
    struct wlr_seat_request_start_drag_event *event = user_data;

    if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    } else {
        wlr_data_source_destroy(event->drag->source);
    }
}

static void set_backend_selection(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.backend_set_selection);
    struct wsland_adapter *adapter = server->adapter;

    adapter->handle->backend_selection_source(adapter, user_data);
}

static void new_server_decoration(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.new_server_decoration);
    struct wlr_server_decoration *decoration = user_data;

    wlr_server_decoration_manager_set_default_mode(
        server->server_decoration_manager,
        decoration->mode
    );
}

static void request_decoration_mode(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_decoration_mode);

    if (window->wayland->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(
            window->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
        );
    }
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.decoration_destroy);

    wl_list_remove(&window->events.decoration_destroy.link);
    wl_list_remove(&window->events.request_decoration_mode.link);
}

static void new_toplevel_decoration(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.new_toplevel_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *decoration = user_data;

     struct wsland_window *window;
    if ((window = decoration->toplevel->base->data) != NULL) {
        window->decoration = decoration;

        LISTEN(&decoration->events.request_mode, &window->events.request_decoration_mode, request_decoration_mode);
        LISTEN(&decoration->events.destroy, &window->events.decoration_destroy, decoration_destroy);
    }
}

static void cursor_warp_to_hint(struct wsland_server *server) {
    double sx = server->active_constraint->current.cursor_hint.x;
    double sy = server->active_constraint->current.cursor_hint.y;

    struct wsland_window *window = server->active_constraint->surface->data;
    if (window && server->active_constraint->current.cursor_hint.enabled) {
        wlr_cursor_warp(server->cursor, NULL, sx + window->metadata.target.x, sy + window->metadata.target.y);
        wlr_seat_pointer_warp(server->active_constraint->seat, sx, sy);
    }
}

static void pointer_constraint_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_pointer_constraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

    if (pointer_constraint->server->active_constraint == pointer_constraint->constraint) {
        cursor_warp_to_hint(pointer_constraint->server);
        pointer_constraint->server->active_constraint = NULL;
    }

    wl_list_remove(&pointer_constraint->destroy.link);
    free(pointer_constraint);
}

static void seat_set_cursor(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *event = user_data;

    if (server->grab.mode != WSLAND_CURSOR_PASSTHROUGH) {
        return;
    }

    if (event->seat_client == server->seat->pointer_state.focused_client) {
        if (event->surface) {
            wlr_scene_surface_create(&server->scene->tree, event->surface);
        }
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void pointer_new_constraint(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.new_constraint);

    struct wsland_pointer_constraint *pointer_constraint = calloc(1, sizeof(*pointer_constraint));
    pointer_constraint->constraint = user_data;
    pointer_constraint->server = server;

    LISTEN(&pointer_constraint->constraint->events.destroy, &pointer_constraint->destroy, pointer_constraint_destroy);
}

static void virtual_new_pointer(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.new_virtual_pointer);

    struct wlr_virtual_pointer_v1_new_pointer_event *event = user_data;
    struct wlr_input_device *device = &event->new_pointer->pointer.base;

    wlr_cursor_attach_input_device(server->cursor, device);

    if (event->suggested_output) {
        wlr_cursor_map_input_to_output(server->cursor, device, event->suggested_output);
    }
}

static void dispatch_window_focus(struct wsland_window *window) {
    if (!window || !window->metadata.mapped) {
        return;
    }

    if (window->role == POPUP) {
        struct wsland_window *parent;
        while ((parent = window->parent) != NULL) {
            if (parent->role == TOPLEVEL) {
                window = parent;
                break;
            }
        }
    }

    struct wsland_adapter *adapter = window->server->adapter;
    window = adapter->handle->find_bottom_window(window, &window->scene->children);

    if (window->role != TOPLEVEL) {
        return;
    }

    struct wlr_seat *seat = window->server->seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *current_surface = window->handle->fetch_surface(window);
    if (prev_surface == current_surface) {
        return;
    }

    if (prev_surface) {
        struct wlr_scene_tree *prev_tree = prev_surface->data;
        if (prev_tree && prev_tree->node.data) {
            struct wsland_window *prev_window = prev_tree->node.data;
            prev_window->handle->window_activate(prev_window, false);

            if (prev_window->buffer_border) {
                struct wlr_render_color BX = window->server->config->border.inactive_color;
                prev_window->server->service->window_border_color(prev_window, &BX);

                pixman_region32_union_rect(
                    &prev_window->render.damage, &prev_window->render.damage, 0, 0,
                    prev_window->metadata.target.width, prev_window->metadata.target.height
                );
            }
        }
    }

    if (window->scene_border) {
        struct wlr_render_color BY = window->server->config->border.active_color;
        window->server->service->window_border_color(window, &BY);

        pixman_region32_union_rect(
            &window->render.damage, &window->render.damage, 0, 0,
            window->metadata.target.width, window->metadata.target.height
        );
    }

    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&window->scene->node);

    wl_list_remove(&window->link);
    wl_list_insert(&window->server->windows, &window->link);
    window->handle->window_activate(window, true);

    if (keyboard != NULL) {
        wlr_seat_keyboard_enter(
            seat, current_surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers
        );
    }
}

static void begin_window_interactive(struct wsland_window *window, enum wsland_cursor_mode mode, uint32_t edges) {
    struct wsland_server *server = window->server;
    server->grab.window = window;
    server->grab.mode = mode;

    if (mode == WSLAND_CURSOR_MOVE) {
        server->grab.x = server->cursor->x - window->scene->node.x;
        server->grab.y = server->cursor->y - window->scene->node.y;

        server->grab.restore = true;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "move");
    }
    else {
        if (edges == WLR_EDGE_NONE) {
            if ((int) server->cursor->x < window->scene->node.x + window->metadata.source.width / 2) {
                edges |= WLR_EDGE_LEFT;
            }
            else {
                edges |= WLR_EDGE_RIGHT;
            }
            if ((int) server->cursor->y < window->scene->node.y + window->metadata.source.height / 2) {
                edges |= WLR_EDGE_TOP;
            }
            else {
                edges |= WLR_EDGE_BOTTOM;
            }
        }

        server->grab.restore = true;
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, wlr_xcursor_get_resize_name(edges));

        struct wlr_box geo_box = window->handle->fetch_geometry(window);
        double border_x = window->scene->node.x + geo_box.x + (edges & WLR_EDGE_RIGHT ? geo_box.width : 0);
        double border_y = window->scene->node.y + geo_box.y + (edges & WLR_EDGE_BOTTOM ? geo_box.height : 0);
        server->grab.x = server->cursor->x - border_x;
        server->grab.y = server->cursor->y - border_y;

        server->grab.geobox = geo_box;
        server->grab.geobox.x += window->scene->node.x;
        server->grab.geobox.y += window->scene->node.y;
        server->grab.resize_edges = edges;
    }
}

static struct wsland_window* desktop_toplevel_at(
    struct wsland_server *server, double lx, double ly, struct wlr_surface **surface, double *sx, double *sy
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
    return server->service->find_toplevel_window(node);
}

static void process_cursor_move(struct wsland_server *server) {
    struct wsland_window *window = server->grab.window;

    if (window->handle->window_grab_cannot(window)) {
        return;
    }

    int pos_x = (int)(server->cursor->x - server->grab.x);
    int pos_y = (int)(server->cursor->y - server->grab.y);

    window->handle->window_motion(window, pos_x, pos_y);
}

static void process_cursor_resize(struct wsland_server *server) {
    struct wsland_window *window = server->grab.window;

    if (window->handle->window_grab_cannot(window)) {
        return;
    }

    int border_x = (int)(server->cursor->x - server->grab.x);
    int border_y = (int)(server->cursor->y - server->grab.y);
    int new_left = server->grab.geobox.x;
    int new_right = server->grab.geobox.x + server->grab.geobox.width;
    int new_top = server->grab.geobox.y;
    int new_bottom = server->grab.geobox.y + server->grab.geobox.height;

    if (server->grab.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    }
    else if (server->grab.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server->grab.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    }
    else if (server->grab.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box geo_box = window->handle->fetch_geometry(window);
    int pos_x = new_left - geo_box.x;
    int pos_y = new_top - geo_box.y;

    int size_w = new_right - new_left;
    int size_h = new_bottom - new_top;
    {
        size_w = MAX(18 - geo_box.x, size_w);
        size_h = MAX(18 - geo_box.y, size_h);

        if (window->handle) {
            struct wsland_output *output = window->handle->fetch_output(window);
            if (output) {
                size_w = MIN(output->full_area.width, size_w);
                size_h = MIN(output->full_area.height, size_h);
            }
        }
    }

    window->handle->window_resize(window, (struct wlr_box) { .x = pos_x, .y = pos_y, .width = size_w, .height = size_h });
}

static void process_cursor_motion(struct wsland_server *server, struct wlr_pointer_motion_absolute_event *event) {
    if (server->grab.window) {
        if (server->grab.mode == WSLAND_CURSOR_MOVE) {
            process_cursor_move(server);
            return;
        }

        if (server->grab.mode == WSLAND_CURSOR_RESIZE) {
            process_cursor_resize(server);
            return;
        }
    }

    double sx, sy;
    struct wlr_seat *seat = server->seat;
    struct wlr_surface *surface = NULL;
    struct wsland_window *window = desktop_toplevel_at(
        server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
    );

    if (window) { window->handle->window_surface_position(window, &sx, &sy); }
    if (server->grab.mode == WSLAND_CURSOR_PRESSED && server->seat->pointer_state.focused_surface) {
        surface = server->seat->pointer_state.focused_surface;

        wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(seat, event->time_msec, sx, sy);
        return;
    }

    if (!window) {
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
    }

    if (!surface) {
        wlr_seat_pointer_notify_clear_focus(seat);
        return;
    }

    wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(seat, event->time_msec, sx, sy);
    // wlr_seat_pointer_notify_frame(seat);
}

static void reset_server_cursor(struct wsland_server *server) {
    if (server->grab.restore) {
        struct wsland_window *window = server->grab.window;
        if (window) { window->handle->window_grab_reset(window); }

        wlr_seat_pointer_notify_clear_focus(server->seat);
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
        server->grab.restore = false;
    }

    server->grab.mode = WSLAND_CURSOR_PASSTHROUGH;
    server->grab.window = NULL;
}

static void process_cursor_button(struct wsland_server *server, struct wlr_pointer_button_event *event) {
    bool handled = false;
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_server_cursor(server);
    } else {
        server->grab.mode = WSLAND_CURSOR_PRESSED;

        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct wsland_window *window = desktop_toplevel_at(
            server, server->cursor->x, server->cursor->y, &surface, &sx, &sy
        );

        server->service->dispatch_window_focus(window);
        if (window && window->handle->window_click_cannot(window)) {
            wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
            return;
        }

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
        // wlr_seat_pointer_notify_frame(server->seat);
    }
}

static void process_cursor_axis(struct wsland_server *server, struct wlr_pointer_axis_event *event) {
    wlr_seat_pointer_notify_axis(
        server->seat, event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction
    );
}

static void window_surface_unmap(struct wl_listener *listener, void *data) {
    struct window_surface *ws = wl_container_of(listener, ws, events.unmap);

    ws->surface->data = NULL;
    ws->window = NULL;
}

static void window_surface_commit(struct wl_listener *listener, void *data) {
    struct window_surface *ws = wl_container_of(listener, ws, events.commit);

    if (ws->window && ws->window->metadata.mapped) {
        pixman_region32_t damage;
        pixman_region32_init(&damage);
        wlr_surface_get_effective_damage(ws->surface, &damage);
        if (pixman_region32_empty(&damage)) {
            pixman_region32_fini(&damage);
            return;
        }

        struct wlr_surface *rs;
        if ((rs = wlr_surface_get_root_surface(ws->surface)) != NULL) {
            if (rs != ws->surface) {
                wl_signal_emit(&rs->events.commit, rs);
            } else {
                pixman_region32_translate(&damage, -ws->window->metadata.geometry.x, -ws->window->metadata.geometry.y);
            }
        }

        if (ws->window->buffer_border) {
            int BW = ws->window->server->config->border.width;
            pixman_region32_translate(&damage, BW, BW);
        }

        pixman_region32_union(&ws->window->render.damage, &ws->window->render.damage, &damage);
        pixman_region32_fini(&damage);

        ws->window->render.dirty = true;
    }
}

static void window_surface_new_subsurface(struct wl_listener *listener, void *user_data) {
    struct window_surface *ws = wl_container_of(listener, ws, events.new_subsurface);
    struct wlr_subsurface *subsurface = user_data;

    ws->window->server->service->window_surface_register(subsurface->surface, ws->window);
}

static void window_surface_destroy(struct wl_listener *listener, void *data) {
    struct window_surface *ws = wl_container_of(listener, ws, events.destroy);

    wl_list_remove(&ws->events.unmap.link);
    wl_list_remove(&ws->events.commit.link);
    wl_list_remove(&ws->events.new_subsurface.link);
    wl_list_remove(&ws->events.destroy.link);
    free(ws);
}

static void window_surface_register(struct wlr_surface *surface, struct wsland_window *window) {
    struct window_surface *ws = calloc(1, sizeof(*ws));
    ws->surface = surface;
    ws->window = window;

    LISTEN(&surface->events.unmap, &ws->events.unmap, window_surface_unmap);
    LISTEN(&surface->events.commit, &ws->events.commit, window_surface_commit);
    LISTEN(&surface->events.new_subsurface, &ws->events.new_subsurface, window_surface_new_subsurface);
    LISTEN(&surface->events.destroy, &ws->events.destroy, window_surface_destroy);
}

static struct wsland_output * fetch_output(struct wsland_server *server, double pos_x, double pos_y) {
    double dest_x, dest_y;
    wlr_box_closest_point(&server->adapter->area, pos_x, pos_y, &dest_x, &dest_y);
    struct wlr_output *wo = wlr_output_layout_output_at(server->output_layout, dest_x, dest_y);

    if (wo) {
        struct wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static struct wsland_window * find_toplevel_window(struct wlr_scene_node *node) {
    struct wlr_scene_tree *tree = node->parent;
    while (tree != NULL && tree->node.data == NULL) {
        tree = tree->node.parent;
    }

    if (tree) {
        return tree->node.data;
    }

    return NULL;
}

static void window_focus_clear(struct wsland_window *window) {
    if (window->server->seat->keyboard_state.focused_surface == window->handle->fetch_surface(window)) {
        wlr_seat_keyboard_notify_clear_focus(window->server->seat);
    }
}

static bool window_title_reset(struct wsland_window *window) {
    char *title = window->handle->fetch_title(window);
    if (!title || (window->metadata.title && strcmp(title, window->metadata.title) == 0)) {
        return false;
    }

    if (window->metadata.title) {
        free(window->metadata.title);
    }

    window->metadata.title = strdup(title);
    return true;
}

static bool window_appid_reset(struct wsland_window *window) {
    char *appid = window->handle->fetch_appid(window);
    if (!appid || (window->metadata.appid && strcmp(appid, window->metadata.appid) == 0)) {
        return false;
    }

    if (window->metadata.appid) {
        free(window->metadata.appid);
    }

    window->metadata.appid = strdup(appid);
    return true;
}

static bool window_parent_reset(struct wsland_window *window) {
    struct wsland_window *pw = window->handle->fetch_parent(window);
    if (!pw || pw == window->parent) {
        return false;
    }

    window->parent = pw;
    return true;
}

static bool window_position_move(struct wsland_window *window) {
    int offset_x, offset_y;
    window->handle->fetch_offset(window, &offset_x, &offset_y);
    if (offset_x == window->metadata.source.x && offset_y == window->metadata.source.y) {
        return false;
    }

    window->metadata.target.x = window->metadata.source.x = offset_x;
    window->metadata.target.y = window->metadata.source.y = offset_y;
    return true;
}

static bool window_geometry_resize(struct wsland_window *window) {
    struct wlr_box geometry = window->handle->fetch_geometry(window);
    if (wlr_box_empty(&geometry)) {
        pixman_region32_clear(&window->render.damage);
        return false;
    }

    if (wlr_box_equal(&geometry, &window->metadata.geometry)) {
        return false;
    }

    window->metadata.geometry = geometry;
    window->metadata.source.width = geometry.width;
    window->metadata.source.height = geometry.height;

    if (window->buffer_border) {
        wlr_scene_buffer_set_buffer(window->scene_border, NULL);
        wlr_buffer_drop(&window->buffer_border->buffer);
        window->buffer_border = NULL;
    }

    if (window->handle->window_have_border(window)) {
        int BW = window->server->config->border.width;
        window->metadata.source.width += BW * 2;
        window->metadata.source.height += BW * 2;

        int width = window->metadata.source.width;
        int height = window->metadata.source.height;
        window->buffer_border = wsland_buffer_create(width, height);
        wlr_scene_buffer_set_buffer(window->scene_border, &window->buffer_border->buffer);
        wlr_scene_buffer_set_source_box(window->scene_border, &(struct wlr_fbox) { .width = width, .height = height });

        struct wlr_box box = { .width = width, .height = height };
        window->server->service->scale_box(window->server, &box, window->render.wo->scale.desktop);
        wlr_scene_buffer_set_dest_size(window->scene_border, box.width, box.height);


        if (window->server->seat->keyboard_state.focused_surface == window->handle->fetch_surface(window)) {
            struct wlr_render_color BY = window->server->config->border.active_color;
            window->server->service->window_border_color(window, &BY);
        }

        window->handle->window_scene_position(window, BW, BW);
    } else {
        window->handle->window_scene_position(window, 0, 0);
    }

    wlr_scene_node_set_position(&window->scene_border->node, 0, 0);
    return true;
}

static bool window_border_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
    return false;
}

static void window_border_color(struct wsland_window *window, struct wlr_render_color *color) {
    int w = window->metadata.source.width, h = window->metadata.source.height;

    if (window->buffer_border) {
        int BW = window->server->config->border.width;
        int RS = window->server->config->border.radius;
        wsland_draw_border(window->buffer_border->surface, w, h, RS, BW, color);
    }
}

static int scale_length(double length, double offset, float scale) {
    return (int)(round((offset + length) * scale) - round(offset * scale));
}

static void scale_region(pixman_region32_t *region, float scale, bool round_up) {
    wlr_region_scale(region, region, scale);

    if (round_up && floorf(scale) != scale) {
        wlr_region_expand(region, region, 1);
    }
}

static void scale_box(struct wsland_server *server, struct wlr_box *box, float scale) {
    if (!server->config->output.scale) {
        return;
    }

    box->width = scale_length(box->width, box->x, scale);
    box->height = scale_length(box->height, box->y, scale);
    box->x = (int)round((double)box->x * scale);
    box->y = (int)round((double)box->y * scale);
}

static void scale_fbox(struct wsland_server *server, struct wlr_fbox *box, float scale) {
    if (!server->config->output.scale) {
        return;
    }

    box->width = scale_length(box->width, box->x, scale);
    box->height = scale_length(box->height, box->y, scale);
    box->x = (int)round(box->x * scale);
    box->y = (int)round(box->y * scale);
}

static void transform_output_box(struct wlr_box *box, struct wsland_output *output) {
    enum wl_output_transform transform = wlr_output_transform_invert(output->output.transform);

    scale_box(output->server, box, output->output.scale);
    wlr_box_transform(box, box, transform, output->max_area.width, output->max_area.height);
}


static void logical_to_buffer_coords(pixman_region32_t *region, struct wsland_output *output, bool round_up) {
    enum wl_output_transform transform = wlr_output_transform_invert(output->output.transform);

    scale_region(region, output->output.scale, round_up);
    wlr_region_transform(region, region, transform, output->max_area.width, output->max_area.height);
}

static void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height) {
	*width = 0;
	*height = 0;

	switch (node->type) {
	case WLR_SCENE_NODE_TREE:
		return;
	case WLR_SCENE_NODE_RECT:;
		struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
		*width = scene_rect->width;
		*height = scene_rect->height;
		break;
	case WLR_SCENE_NODE_BUFFER:;
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
			*width = scene_buffer->dst_width;
			*height = scene_buffer->dst_height;
		} else {
			*width = scene_buffer->buffer_width;
			*height = scene_buffer->buffer_height;
			wlr_output_transform_coords(scene_buffer->transform, width, height);
		}
		break;
	}
}

static void scene_node_opaque_region(struct wlr_scene_node *node, int x, int y, pixman_region32_t *opaque) {
    int width, height;
    scene_node_get_size(node, &width, &height);

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
        if (scene_rect->color[3] != 1) {
            return;
        }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

        if (!scene_buffer->buffer) {
            return;
        }

        if (scene_buffer->opacity != 1) {
            return;
        }

        if (!scene_buffer->buffer_is_opaque) {
            pixman_region32_copy(opaque, &scene_buffer->opaque_region);
            pixman_region32_intersect_rect(opaque, opaque, 0, 0, width, height);
            pixman_region32_translate(opaque, x, y);
            return;
        }
    }

    pixman_region32_fini(opaque);
    pixman_region32_init_rect(opaque, x, y, width, height);
}

static bool dest_scale_point_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) { // NOLINT(*-non-const-parameter)
    return *sx >= 0 && *sx < buffer->dst_width && *sy >= 0 && *sy < buffer->dst_height;
}

static void dest_scale_callback(struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data) {
    struct wsland_window *window = user_data;

    struct wlr_scene_surface *wss;
    if (buffer->buffer && window->render.wo && (wss = wlr_scene_surface_try_from_buffer(buffer)) != NULL) {
        int scale = wss->surface->current.scale;

        struct wlr_fbox box = { .width = buffer->src_box.width * scale, .height = buffer->src_box.height * scale };
        window->server->service->scale_fbox(window->server, &box, window->render.wo->scale.desktop);

        if (!wlr_fbox_empty(&box)) {
            wlr_scene_buffer_set_dest_size(buffer, (int)box.width, (int)box.height);
            buffer->point_accepts_input = dest_scale_point_accepts_input;
        }
    }
}

static void window_dest_scale(struct wsland_window *window) {
    wlr_scene_node_for_each_buffer(&window->scene->node, dest_scale_callback, window);
}

static void window_render_detect_callback(struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data) {
    struct wsland_window *window = user_data;
    struct wsland_output *output = window->render.wo;

    if (buffer->buffer) {
        struct wsland_window *toplevel = window->server->service->find_toplevel_window(&buffer->node);
        if (window != toplevel) { return; }

        struct wlr_texture *texture = wlr_texture_from_buffer(window->server->renderer, buffer->buffer);
        if (!texture) { return; }

        pixman_region32_t rr;
        pixman_region32_init(&rr);
        pixman_region32_copy(&rr, &buffer->node.visible);
        pixman_region32_translate(&rr, -output->max_area.x, -output->max_area.y);
        pixman_region32_translate(&rr, -window->metadata.source.x, -window->metadata.source.y);
        logical_to_buffer_coords(&rr, output, true);
        if (pixman_region32_empty(&rr)) {
            window->render.skip = true;
            wlr_texture_destroy(texture);
            pixman_region32_fini(&rr);
            return;
        }

        int x = 0, y = 0;
        window->handle->fetch_render_position(window, sx, sy, &x, &y);

        struct wlr_scene_surface *surface;
        if ((surface = wlr_scene_surface_try_from_buffer(buffer)) != NULL) {
            pixman_region32_t damage;
            pixman_region32_init(&damage);
            wlr_surface_get_effective_damage(surface->surface, &damage);
            pixman_region32_translate(&damage, x, y);

            pixman_region32_union(&window->render.damage, &window->render.damage, &damage);
            pixman_region32_fini(&damage);
        }


        struct wlr_box dst_box = { .x = x, .y = y, };
        window->server->service->scale_box(window->server, &dst_box, window->render.wo->scale.desktop);

        scene_node_get_size(&buffer->node, &dst_box.width, &dst_box.height);
        transform_output_box(&dst_box, output);

        pixman_region32_t opaque;
        pixman_region32_init(&opaque);
        scene_node_opaque_region(&buffer->node, x, y, &opaque);
        logical_to_buffer_coords(&opaque, output, false);
        pixman_region32_subtract(&opaque, &rr, &opaque);

        enum wl_output_transform transform = wlr_output_transform_invert(buffer->transform);
        transform = wlr_output_transform_compose(transform, window->render.wo->output.transform);

        wlr_render_pass_add_texture(window->render.wrp, &(struct wlr_render_texture_options) {
            .texture = texture,
            .dst_box = dst_box,
            .transform = transform,
            .alpha = &buffer->opacity,
            .src_box = buffer->src_box,
            .wait_point = buffer->wait_point,
            .color_range = buffer->color_range,
            .filter_mode = buffer->filter_mode,
            .wait_timeline = buffer->wait_timeline,
            .color_encoding = buffer->color_encoding,
            .transfer_function = buffer->transfer_function,
            .blend_mode = pixman_region32_not_empty(&opaque) ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED : WLR_RENDER_BLEND_MODE_NONE,
            // .clip = &clip_box,
        });

        pixman_region32_fini(&rr);
        pixman_region32_fini(&opaque);
        wlr_texture_destroy(texture);
    }
}

static void window_render_detect(struct wsland_window *window) {
    struct wlr_buffer *buffer = wlr_swapchain_acquire(window->render.swapchain);
    if (!buffer) { return; }

    struct wlr_box *target = &window->metadata.target;
    window->render.wrp = wlr_renderer_begin_buffer_pass(window->server->renderer, buffer, NULL);

    wlr_render_pass_add_rect(window->render.wrp, &(const struct wlr_render_rect_options) {
        .box = { .x = 0, .y = 0, .width = target->width, .height = target->height },
        .color = { .r = 0, .g = 0, .b = 0,  .a = 0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
    });

    wlr_scene_node_for_each_buffer(&window->scene->node, window_render_detect_callback, window);
    if (wlr_render_pass_submit(window->render.wrp)) {
        if (!window->render.skip) {
            if (window->buffer_border) {
                int BW = window->server->config->border.width;
                int RS = window->server->config->border.radius;

                struct wlr_box box = { .x = BW, .y = RS };
                window->server->service->scale_box(window->server, &box, window->render.wo->scale.desktop);
                wsland_mask_border(buffer, target->width, target->height, box.y, box.x);
            }
            window->render.buffer = buffer;
        } else {
            window->render.skip = false;
            wlr_buffer_unlock(buffer);
        }
    }

    window->render.wrp = NULL;
}

static const struct wsland_server_service server_service = {
    .backend_new_output = backend_new_output,
    .backend_new_input = backend_new_input,

    .cursor_axis = cursor_axis,
    .cursor_frame = cursor_frame,
    .cursor_button = cursor_button,
    .cursor_motion = cursor_motion,
    .cursor_motion_absolute = cursor_motion_absolute,

    .seat_set_cursor = seat_set_cursor,
    .cursor_set_shape = cursor_set_shape,
    .seat_set_selection = seat_set_selection,
    .seat_request_start_drag = seat_request_start_drag,
    .set_backend_selection = set_backend_selection,

    .new_server_decoration = new_server_decoration,
    .new_toplevel_decoration = new_toplevel_decoration,
    .pointer_new_constraint = pointer_new_constraint,
    .virtual_new_pointer = virtual_new_pointer,

    .process_keyboard_key = process_keyboard_key,
    .process_cursor_motion = process_cursor_motion,
    .process_cursor_button = process_cursor_button,
    .process_cursor_axis = process_cursor_axis,

    .window_kill_action = window_kill_action,
    .window_center_action = window_center_action,
    .window_maximize_action = window_maximize_action,
    .window_fullscreen_action = window_fullscreen_action,
    .window_column_maximize_action = window_column_maximize_action,

    .scale_box = scale_box,
    .scale_fbox = scale_fbox,
    .fetch_output = fetch_output,
    .dispatch_window_focus = dispatch_window_focus,
    .begin_window_interactive = begin_window_interactive,
    .window_surface_register = window_surface_register,
    .find_toplevel_window = find_toplevel_window,
    .window_focus_clear = window_focus_clear,
    .window_title_reset = window_title_reset,
    .window_appid_reset = window_appid_reset,
    .window_parent_reset = window_parent_reset,
    .window_position_move = window_position_move,
    .window_geometry_resize = window_geometry_resize,
    .window_border_accepts_input = window_border_accepts_input,
    .window_render_detect = window_render_detect,
    .window_border_color = window_border_color,
    .window_dest_scale = window_dest_scale,
};

void wsland_server_service_init(struct wsland_server *server) {
    server->service = &server_service;
}