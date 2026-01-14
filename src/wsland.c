// ReSharper disable ALl
#include <getopt.h>
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>

#include <wlr/backend.h>
#include <wlr/util/log.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_drm.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <cairo/cairo.h>

#include "wsland.h"


struct wsland_server server = {0};

static struct wlr_pointer_impl pointer_impl = {
    .name = "wsland-pointer-impl",
};

static struct wlr_keyboard_impl keyboard_impl = {
    .name = "wsland-keyboard-impl"
};

static void output_frame(struct wl_listener *listener, void *data) {
    struct wsland_output *output = wl_container_of(listener, output, frame);

    { // assert
        if (!server.freerdp.peer_ctx) {
            return;
        }
        if (!(server.freerdp.peer_ctx->flags & WSLAND_RDP_PEER_OUTPUT_ENABLED)) {
            return;
        }
    }

    wsland_freerdp_surface_output(output);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(output->scene_output, &now);
}

static void output_destroy(struct wl_listener *listener, void *data) {
    struct wsland_output *output = wl_container_of(listener, output, destroy);

    wlr_scene_output_destroy(output->scene_output);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->link);
    free(output);
}

static void focus_toplevel(struct wsland_toplevel *toplevel) {
    if (toplevel == NULL) {
        return;
    }
    struct wlr_seat *seat = server.seat;
    struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
    struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
    if (toplevel->xdg_toplevel->current.activated && prev_surface == surface) {
        return;
    }

    if (prev_surface) {
        struct wlr_xdg_toplevel *prev_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
        if (prev_toplevel != NULL) {
            wlr_xdg_toplevel_set_activated(prev_toplevel, false);
        }
    }
    struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    struct wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);
    if (wlr_output) {
        struct wsland_output *w_output = wlr_output->data;
        wl_list_remove(&toplevel->link);
        wl_list_insert(&w_output->toplevels, &toplevel->link);
        wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    }

    if (keyboard != NULL) {
        wlr_seat_keyboard_notify_enter(
            seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers
        );
    }
}

static void server_new_output(struct wl_listener *listener, void *data) {
    struct wlr_output *wlr_output = data;

    struct wsland_peer_context *peer_ctx = server.freerdp.peer_ctx;
    wlr_output_init_render(wlr_output, server.allocator, server.renderer);

    struct wsland_output *w_output = calloc(1, sizeof(*w_output));
    w_output->base = wlr_output;
    wlr_output->data = w_output;

    w_output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &w_output->frame);
    w_output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &w_output->destroy);

    wl_list_init(&w_output->toplevels);
    wl_list_insert(&server.outputs, &w_output->link);

    w_output->scene_output = wlr_scene_output_create(server.scene, wlr_output);
    struct wlr_output_layout_output *layout_output = wlr_output_layout_add_auto(server.output_layout, wlr_output);
    wlr_scene_output_layout_add_output(server.scene_layout, layout_output, w_output->scene_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
}

static void wsland_surface_handle_destroy(struct wl_listener *listener, void *data) {
    struct wsland_surface *wsland_surface = wl_container_of(listener, wsland_surface, destroy);

    wl_list_remove(&wsland_surface->destroy.link);
    free(wsland_surface);
}

static void reset_cursor_mode(void) {
    server.cursor_mode = WSLAND_CURSOR_PASSTHROUGH;
    server.grabbed_toplevel = NULL;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, map);
    struct wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout, toplevel->scene_tree->node.x, toplevel->scene_tree->node.y);

    if (wlr_output) {
        struct wsland_output *w_output = wlr_output->data;

        wl_list_insert(&w_output->toplevels, &toplevel->link);
        focus_toplevel(toplevel);

        wsland_freerdp_surface_create(toplevel);
    }
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);

    if (toplevel == server.grabbed_toplevel) {
        reset_cursor_mode();
    }

    wl_list_remove(&toplevel->link);

    wsland_freerdp_surface_destroy(toplevel);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, commit);

    if (toplevel->xdg_toplevel->base->initial_commit) {
        pixman_region32_init(&toplevel->state.surface_damage);

        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    } else {
        wlr_surface_get_effective_damage(data, &toplevel->state.surface_damage);

        wsland_freerdp_surface_update(toplevel);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);

    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    free(toplevel);
}

static void begin_interactive(struct wsland_toplevel *toplevel, enum wsland_cursor_mode mode, uint32_t edges) {
    server.grabbed_toplevel = toplevel;
    server.cursor_mode = mode;

    if (mode == WSLAND_CURSOR_MOVE) {
        server.grab_x = server.cursor->x - toplevel->scene_tree->node.x;
        server.grab_y = server.cursor->y - toplevel->scene_tree->node.y;
    }
    else {
        struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

        double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
            ((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
        double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
            ((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
        server.grab_x = server.cursor->x - border_x;
        server.grab_y = server.cursor->y - border_y;

        server.grab_geobox = *geo_box;
        server.grab_geobox.x += toplevel->scene_tree->node.x;
        server.grab_geobox.y += toplevel->scene_tree->node.y;

        server.resize_edges = edges;
    }
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, request_move);
    begin_interactive(toplevel, WSLAND_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    begin_interactive(toplevel, WSLAND_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, request_maximize);

    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct wsland_toplevel *toplevel = wl_container_of(listener, toplevel, request_fullscreen);

    if (toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
    }
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    struct wsland_toplevel *toplevel = calloc(1, sizeof(*toplevel));
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_xdg_surface_create(&server.scene->tree, xdg_toplevel->base);
    toplevel->scene_tree->node.data = toplevel;
    xdg_toplevel->base->data = toplevel->scene_tree;

    toplevel->map.notify = xdg_toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = xdg_toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = xdg_toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);

    toplevel->destroy.notify = xdg_toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

    toplevel->request_move.notify = xdg_toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = xdg_toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct wsland_popup *popup = wl_container_of(listener, popup, commit);

    if (popup->xdg_popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct wsland_popup *popup = wl_container_of(listener, popup, destroy);

    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
    struct wlr_xdg_popup *xdg_popup = data;

    struct wsland_popup *popup = calloc(1, sizeof(*popup));
    popup->xdg_popup = xdg_popup;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = xdg_popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);

    popup->destroy.notify = xdg_popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void process_cursor_move(void) {
    struct wsland_toplevel *toplevel = server.grabbed_toplevel;

    wlr_scene_node_set_position(
        &toplevel->scene_tree->node,
        server.cursor->x - server.grab_x,
        server.cursor->y - server.grab_y
    );
}

static struct wsland_toplevel* desktop_toplevel_at(double lx, double ly, struct wlr_surface **surface, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server.scene->tree.node, lx, ly, sx, sy
    );
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

    return tree != NULL ? tree->node.data : NULL;
}

static void process_cursor_resize(void) {
    struct wsland_toplevel *toplevel = server.grabbed_toplevel;

    double border_x = server.cursor->x - server.grab_x;
    double border_y = server.cursor->y - server.grab_y;
    int new_left = server.grab_geobox.x;
    int new_right = server.grab_geobox.x + server.grab_geobox.width;
    int new_top = server.grab_geobox.y;
    int new_bottom = server.grab_geobox.y + server.grab_geobox.height;

    if (server.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom) {
            new_top = new_bottom - 1;
        }
    }
    else if (server.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top) {
            new_bottom = new_top + 1;
        }
    }
    if (server.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right) {
            new_left = new_right - 1;
        }
    }
    else if (server.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left) {
            new_right = new_left + 1;
        }
    }

    struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(
        &toplevel->scene_tree->node, new_left - geo_box->x, new_top - geo_box->y
    );

    int new_width = new_right - new_left;
    int new_height = new_bottom - new_top;
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_width, new_height);
}

static void process_cursor_motion(uint32_t time) {
    if (server.cursor_mode == WSLAND_CURSOR_MOVE) {
        process_cursor_move();
        return;
    }
    else if (server.cursor_mode == WSLAND_CURSOR_RESIZE) {
        process_cursor_resize();
        return;
    }

    double sx, sy;
    struct wlr_seat *seat = server.seat;
    struct wlr_surface *surface = NULL;
    struct wsland_toplevel *toplevel = desktop_toplevel_at(
        server.cursor->x, server.cursor->y, &surface, &sx, &sy
    );
    if (!toplevel) {
        wlr_cursor_set_xcursor(server.cursor, server.cursor_mgr, "default");
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
    struct wlr_pointer_motion_event *event = data;

    wlr_cursor_move(server.cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    struct wlr_pointer_motion_absolute_event *event = data;

    wlr_cursor_warp_closest(server.cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
    struct wlr_pointer_button_event *event = data;

    wlr_seat_pointer_notify_button(server.seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        reset_cursor_mode();
    }
    else {
        double sx, sy;
        struct wlr_surface *surface = NULL;
        struct wsland_toplevel *toplevel = desktop_toplevel_at(
            server.cursor->x, server.cursor->y, &surface, &sx, &sy
        );
        focus_toplevel(toplevel);
    }
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
    struct wlr_pointer_axis_event *event = data;

    wlr_seat_pointer_notify_axis(
        server.seat,
        event->time_msec, event->orientation, event->delta,
        event->delta_discrete, event->source, event->relative_direction
    );
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
    struct wlr_cursor *cursor = data;

    wsland_freerdp_surface_cursor_update(cursor);
    wlr_seat_pointer_notify_frame(server.seat);
}

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);

    wlr_seat_set_keyboard(server.seat, &keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(server.seat, &keyboard->keyboard.modifiers);
}

static bool handle_keybinding(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server.wl_display);
        break;
    case XKB_KEY_F1:
        if (wl_list_length(&server.primary_output->toplevels) < 2) {
            break;
        }
        struct wsland_toplevel *next_toplevel = wl_container_of(server.primary_output->toplevels.prev, next_toplevel, link);
        focus_toplevel(next_toplevel);
        break;
    case XKB_KEY_F12:
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", "weston-terminal", (void*)NULL);
        }
        break;
    default:
        return false;
    }
    return true;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, key);
    struct wlr_keyboard_key_event *event = data;
    struct wlr_seat *seat = server.seat;

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t *syms;
    int nsyms = xkb_state_key_get_syms(keyboard->keyboard.xkb_state, keycode, &syms);

    bool handled = false;

    uint32_t modifiers = wlr_keyboard_get_modifiers(&keyboard->keyboard);
    if ((modifiers & WLR_MODIFIER_ALT) &&
        event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < nsyms; i++) {
            handled = handle_keybinding(syms[i]);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(seat, &keyboard->keyboard);
        wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
    struct wsland_keyboard *keyboard = wl_container_of(listener, keyboard, destroy);

    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->link);
    free(keyboard);
}

static void server_new_keyboard(struct wlr_input_device *device) {
    struct wsland_peer_context *context = device->data;

    struct wsland_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wlr_log(WLR_ERROR, "falied to allocate wsland keyboard");
        return;
    }
    wlr_keyboard_init(&keyboard->keyboard, &keyboard_impl, device->name);
    context->keyboard = keyboard;

    struct xkb_keymap *keymap = {0};
    struct xkb_rule_names rule_names = fetch_xkb_rule_names(context->peer->settings);
    if (rule_names.layout) {
        struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        if (!xkb_context) {
            wlr_log(WLR_DEBUG, "failed to allocate xkb context");

            free(keyboard);
            return;
        }
    }
    struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    keymap = xkb_keymap_new_from_names(xkb_context, &rule_names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&keyboard->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_context);

    wlr_keyboard_set_repeat_info(&keyboard->keyboard, 25, 600);

    keyboard->modifiers.notify = keyboard_handle_modifiers;
    wl_signal_add(&keyboard->keyboard.events.modifiers, &keyboard->modifiers);
    keyboard->key.notify = keyboard_handle_key;
    wl_signal_add(&keyboard->keyboard.events.key, &keyboard->key);
    keyboard->destroy.notify = keyboard_handle_destroy;
    wl_signal_add(&keyboard->keyboard.base.events.destroy, &keyboard->destroy);

    wlr_seat_set_keyboard(server.seat, &keyboard->keyboard);

    wl_list_insert(&server.keyboards, &keyboard->link);
    context->flags |= WSLAND_RDP_PEER_KEYBOARD_ENABLE;
}

static void pointer_handle_destroy(struct wl_listener *listener, void *data) {
    struct wsland_pointer *pointer = wl_container_of(listener, pointer, destroy);

    wlr_cursor_detach_input_device(server.cursor, &pointer->pointer.base);
    wl_list_remove(&pointer->destroy.link);
}

static void server_new_pointer(struct wlr_input_device *device) {
    struct wsland_peer_context *context = device->data;

    struct wsland_pointer *pointer = calloc(1, sizeof(*pointer));
    if (!pointer) {
        wlr_log(WLR_ERROR, "falied to allocate wsland pointer");
        return;
    }
    wlr_pointer_init(&pointer->pointer, &pointer_impl, device->name);
    context->pointer = pointer;

    pointer->destroy.notify = pointer_handle_destroy;
    wl_signal_add(&pointer->pointer.base.events.destroy, &pointer->destroy);

    wlr_cursor_attach_input_device(server.cursor, &pointer->pointer.base);
    context->flags |= WSLAND_RDP_PEER_POINTER_ENABLE;
}

static void server_new_input(struct wl_listener *listener, void *data) {
    struct wlr_input_device *device = data;
    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD:
        server_new_keyboard(device);
        break;
    case WLR_INPUT_DEVICE_POINTER:
        server_new_pointer(device);
        break;
    default:
        break;
    }

    uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
    if (!wl_list_empty(&server.keyboards)) {
        caps |= WL_SEAT_CAPABILITY_KEYBOARD;
    }
    wlr_seat_set_capabilities(server.seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
    struct wlr_seat_pointer_request_set_cursor_event *event = data;

    struct wlr_seat_client *focused_client = server.seat->pointer_state.focused_client;
    if (focused_client == event->seat_client) {
        wlr_cursor_set_surface(server.cursor, event->surface, event->hotspot_x, event->hotspot_y);
    }
}

static void seat_request_set_selection(struct wl_listener *listener, void *data) {
    struct wlr_seat_request_set_selection_event *event = data;

    wlr_seat_set_selection(server.seat, event->source, event->serial);
}

static bool wsland_server_parse_args(int argc, char *argv[]) {
    int c;
    while ((c = getopt(argc, argv, "s:h")) != -1) {
        switch (c) {
        case 's':
            server.command = optarg;
            break;
        default:
            return false;
        }
    }

    if (optind < argc) {
        return false;
    }

    return true;
}

static bool wsland_server_parse_envs(void) {
    server.freerdp.address = "0.0.0.0";
    server.freerdp.port = 3389;

    const char *address = getenv("WSLAND_ADDR");
    if (address) {
        server.freerdp.address = strdup(address);
    }

    const char *temp_port = getenv("WSLAND_PORT");
    if (temp_port) {
        char *endptr;
        int port = (int)strtol(temp_port, &endptr, 10);
        if (*endptr || port <= 0 || port > 65535) {
            wlr_log(WLR_ERROR, "Expected WLR_RDP_PORT to be a positive integer less or equal to 65535");
            return false;
        }
        server.freerdp.port = port;
    }

    setenv("WLR_BACKENDS", "headless", true);
    setenv("WLR_HEADLESS_OUTPUTS", "0", true);
    return true;
}

static bool wsland_server_component_init(void) {
    server.wl_display = wl_display_create();
    if (!server.wl_display) {
        wlr_log(WLR_ERROR, "failed to invoke wl_display_create");
        return false;
    }

    server.event_loop = wl_display_get_event_loop(server.wl_display);
    if (!server.event_loop) {
        wlr_log(WLR_ERROR, "failed to invoke wl_display_get_event_loop");
        return false;
    }

    server.backend = wlr_headless_backend_create(server.event_loop);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_headless_backend_create");
        return false;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_renderer_autocreate");
        return false;
    }

    if (!wlr_renderer_init_wl_display(server.renderer, server.wl_display)) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_renderer_init_wl_display");
        return false;
    }

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_allocator_autocreate");
        return false;
    }

    server.compositor = wlr_compositor_create(server.wl_display, 5, server.renderer);
    if (!server.compositor) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_compositor_create");
        return false;
    }

    server.subcompositor = wlr_subcompositor_create(server.wl_display);
    if (!server.subcompositor) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_subcompositor_create");
        return false;
    }

    server.data_device_manager = wlr_data_device_manager_create(server.wl_display);
    if (!server.data_device_manager) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_data_device_manager_create");
        return false;
    }

    server.output_layout = wlr_output_layout_create(server.wl_display);
    if (!server.output_layout) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_output_layout_create");
        return false;
    }

    server.scene = wlr_scene_create();
    if (!server.scene) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_scene_create");
        return false;
    }

    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);
    if (!server.scene_layout) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_scene_attach_output_layout");
        return false;
    }

    server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 3);
    if (!server.xdg_shell) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_xdg_shell_create");
        return false;
    }

    server.cursor = wlr_cursor_create();
    if (!server.cursor) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_cursor_create");
        return false;
    }

    server.cursor_mgr = wlr_xcursor_manager_create("default", 42);
    if (!server.cursor_mgr) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_xcursor_manager_create");
        return false;
    }

    server.seat = wlr_seat_create(server.wl_display, "seat0");
    if (!server.seat) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_seat_create");
        return false;
    }

    { // backend event
        wl_list_init(&server.outputs);
        server.new_output.notify = server_new_output;
        wl_signal_add(&server.backend->events.new_output, &server.new_output);

        wl_list_init(&server.keyboards);
        server.new_input.notify = server_new_input;
        wl_signal_add(&server.backend->events.new_input, &server.new_input);
    }

    { // xdg shell event
        server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
        wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
        server.new_xdg_popup.notify = server_new_xdg_popup;
        wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);
    }

    { // cursor event
        wlr_cursor_attach_output_layout(server.cursor, server.output_layout);

        server.cursor_mode = WSLAND_CURSOR_PASSTHROUGH;
        server.cursor_motion.notify = server_cursor_motion;
        wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
        server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
        wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
        server.cursor_button.notify = server_cursor_button;
        wl_signal_add(&server.cursor->events.button, &server.cursor_button);
        server.cursor_axis.notify = server_cursor_axis;
        wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
        server.cursor_frame.notify = server_cursor_frame;
        wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);
    }

    { // seat event
        server.request_cursor.notify = seat_request_cursor;
        wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
        server.request_set_selection.notify = seat_request_set_selection;
        wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);
    }

    server.socket_name = wl_display_add_socket_auto(server.wl_display);
    if (!server.socket_name) {
        wlr_log(WLR_ERROR, "failed to invoke wl_display_add_socket_auto");
        return false;
    }

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "failed to invoke wlr_backend_start");
        return false;
    }

    return true;
}

static void wsland_server_destroy(void) {
    wsland_freerdp_destroy();

    wl_display_destroy_clients(server.wl_display);

    wl_list_remove(&server.new_xdg_toplevel.link);
    wl_list_remove(&server.new_xdg_popup.link);

    wl_list_remove(&server.cursor_motion.link);
    wl_list_remove(&server.cursor_motion_absolute.link);
    wl_list_remove(&server.cursor_button.link);
    wl_list_remove(&server.cursor_axis.link);
    wl_list_remove(&server.cursor_frame.link);

    wl_list_remove(&server.new_input.link);
    wl_list_remove(&server.request_cursor.link);
    wl_list_remove(&server.request_set_selection.link);

    wl_list_remove(&server.new_output.link);

    wlr_scene_node_destroy(&server.scene->tree.node);
    wlr_xcursor_manager_destroy(server.cursor_mgr);
    wlr_cursor_destroy(server.cursor);

    if (server.allocator) {
        wlr_allocator_destroy(server.allocator);
    }

    if (server.renderer) {
        wlr_renderer_destroy(server.renderer);
    }

    if (server.backend) {
        wlr_backend_destroy(server.backend);
    }

    if (server.event_loop) {
        wl_event_loop_destroy(server.event_loop);
    }

    if (server.wl_display) {
        wl_display_destroy(server.wl_display);
    }
}

bool wsland_server_init(int argc, char *argv[]) {
    if (!wsland_server_parse_args(argc, argv)) {
        printf("usage: %s [-s command]", argv[0]);
        return true;
    }

    if (!wsland_server_parse_envs()) {
        return false;
    }

    if (!wsland_server_component_init() || !wsland_freerdp_init()) {
        wsland_server_destroy();
        return false;
    }

    { // boot wayland
        setenv("WAYLAND_DISPLAY", server.socket_name, true);
        wlr_log(WLR_INFO, "running wayland compositor [ WAYLAND_DISPLAY=%s ]", server.socket_name);
        wl_display_run(server.wl_display);
    }

    wsland_server_destroy();
    return true;
}
