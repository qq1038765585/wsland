#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "common/config.h"
#include "common/logger.h"
#include "adapter.h"
#include "server.h"


static bool window_check(struct wsland_window *window) {
    assert(window->type == ICON);

    return window->metadata.mapped;
}

static char* fetch_title(struct wsland_window *window) {
    if (window_check(window)) {}

    return NULL;
}

static char* fetch_appid(struct wsland_window *window) {
    if (window_check(window)) {}

    return NULL;
}

static struct wsland_window *fetch_parent(struct wsland_window *window) {
    if (window_check(window)) {}

    return NULL;
}

static struct wsland_output *fetch_output(struct wsland_window *window) {
    if (window_check(window)) {
        int pos_x, pos_y;
        window->handle->fetch_offset(window, &pos_x, &pos_y);
        return window->server->service->fetch_output(window->server, pos_x, pos_y);
    }

    return NULL;
}

static struct wlr_box fetch_bounds(struct wsland_window *window) {
    struct wlr_box region = {0};

    if (window_check(window)) {
        wlr_surface_get_extents(window->icon->surface, &region);
    }

    return region;
}

static struct wlr_surface *fetch_surface(struct wsland_window *window) {
    if (window_check(window)) {
        return window->icon->surface;
    }

    return NULL;
}

static struct wlr_box fetch_geometry(struct wsland_window *window) {
    return fetch_bounds(window);
}

static void fetch_render_position(struct wsland_window *window, int sx, int sy, int *dx, int *dy) {
    if (window_check(window)) {
        *dx = sx - window->scene->node.x * 2;
        *dy = sy - window->scene->node.y * 2;
    }
}

static void fetch_offset(struct wsland_window *window, int *offset_x, int *offset_y) {
    if (window_check(window)) {
        wlr_scene_node_coords(&window->scene->node, offset_x, offset_y);
    } else {
        *offset_x = 0;
        *offset_y = 0;
    }
}

static void window_resize(struct wsland_window *window, struct wlr_box box) {
    if (window_check(window)) {}
}

static void window_motion(struct wsland_window *window, int pos_x, int pos_y) {
    if (window_check(window)) {}
}

static void window_center(struct wsland_window *window) {
    if (window_check(window)) {}
}

static void window_column(struct wsland_window *window) {
    if (window_check(window)) {}
}

static void window_activate(struct wsland_window *window, bool enabled) {
    if (window_check(window)) {}
}

static void window_shutdown(struct wsland_window *window) {
    if (window_check(window)) {}
}

static void window_surface_position(struct wsland_window *window, double *sx, double *sy) {
    if (window_check(window)) {
        struct wlr_fbox box = { .x = *sx, .y = *sy, .width = window->metadata.geometry.x, .height = window->metadata.geometry.y};
        window->server->service->scale_fbox(window->server, &box, 1 / window->render.wo->scale.desktop);
        *sx = box.x + (window->metadata.geometry.x - box.width);
        *sy = box.y + (window->metadata.geometry.y - box.height);
    }
}

static void window_scene_position(struct wsland_window *window, int pos_x, int pos_y) {
    if (window_check(window)) {}
}

static void window_grab_reset(struct wsland_window *window) {

}

static bool window_have_border(struct wsland_window *window) {
    if (window_check(window)) {}

    return false;
}

static bool window_click_cannot(struct wsland_window *window) {
    if (window_check(window)) {}

    return true;
}

static void drag_icon_map(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.map);
    struct wsland_server *server = window->server;

    window->metadata.above = true;
    window->metadata.mapped = true;
    server->adapter->handle->backend_window_create(window);

    wl_list_init(&window->link);
    wl_list_insert(&server->windows, &window->link);
}

static void drag_icon_commit(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.commit);
    struct wlr_surface_state current = window->icon->surface->current;
    struct wsland_server *server = window->server;

    if (window_check(window)) {
        int x = window->scene->node.x + current.dx;
        int y = window->scene->node.y + current.dy;
        wlr_scene_node_set_position(&window->scene->node, x, y);
    }

    if (window_check(window) && server->config->output.scale) {
        server->service->window_dest_scale(window);
    }
}

static void drag_icon_destroy(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.destroy);
    struct wsland_server *server = window->server;

    server->adapter->handle->backend_window_destroy(window);
    wl_list_remove(&window->link);
    window->metadata.mapped = false;

    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.destroy.link);

    if (window->scene->node.enabled) { wlr_scene_node_destroy(&window->scene->node); }
    free(window);
}

static const struct wsland_window_handle wsland_dnd_window_impl = {
    .fetch_title = fetch_title,
    .fetch_appid = fetch_appid,
    .fetch_output = fetch_output,
    .fetch_parent = fetch_parent,
    .fetch_bounds = fetch_bounds,
    .fetch_surface = fetch_surface,
    .fetch_geometry = fetch_geometry,
    .fetch_render_position = fetch_render_position,
    .fetch_offset = fetch_offset,

    .window_resize = window_resize,
    .window_motion = window_motion,
    .window_center = window_center,
    .window_column = window_column,
    .window_activate = window_activate,
    .window_shutdown = window_shutdown,
    .window_surface_position = window_surface_position,
    .window_scene_position = window_scene_position,
    .window_grab_reset = window_grab_reset,
    .window_have_border = window_have_border,
    .window_click_cannot = window_click_cannot,
};

static void seat_start_drag(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.start_drag);

    struct wlr_drag *drag = user_data;
    if (!drag->icon) { return; }

    struct wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_dnd_window_impl;
    window->icon = drag->icon;
    window->server = server;

    window->type = ICON;
    window->role = TOPLEVEL;

    window->scene = drag->icon->surface->data = wlr_scene_tree_create(server->scene_grab);
    window->scene_surface = wlr_scene_drag_icon_create(window->scene, drag->icon);
    window->scene_border = wlr_scene_buffer_create(window->scene, NULL);

    window->scene->node.data = window->scene_border->node.data = window->scene_surface->node.data = window;
    window->scene_border->point_accepts_input = server->service->window_border_accepts_input;

    pixman_region32_init(&window->render.damage);
    server->service->window_surface_register(drag->icon->surface, window);
    LISTEN(&drag->icon->surface->events.map, &window->events.map, drag_icon_map);
    LISTEN(&drag->icon->surface->events.commit, &window->events.commit, drag_icon_commit);
    LISTEN(&drag->icon->events.destroy, &window->events.destroy, drag_icon_destroy);
}

void dnd_event_init(struct wsland_server *server) {
    LISTEN(&server->seat->events.start_drag, &server->events.start_drag, seat_start_drag);
}