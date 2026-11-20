#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr-layer-shell-unstable-v1-protocol.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>

#include "common/config.h"
#include "adapter.h"
#include "server.h"


static bool window_check(struct wsland_window *window) {
    assert(window->type == LAYER);

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
        if (window->role == TOPLEVEL) {
            wlr_surface_get_extents(window->layer->surface, &region);
        } else {
            wlr_surface_get_extents(window->wayland->surface, &region);
        }
    }

    return region;
}

static struct wlr_surface *fetch_surface(struct wsland_window *window) {
    if (window_check(window)) {
        return window->layer->surface;
    }

    return NULL;
}

static struct wlr_box fetch_geometry(struct wsland_window *window) {
    return fetch_bounds(window);
}

static void fetch_render_position(struct wsland_window *window, int sx, int sy, int *dx, int *dy) {
    if (window_check(window)) {
        if (window->role == TOPLEVEL) {
            *dx = sx - window->scene_layer->tree->node.x;
            *dy = sy - window->scene_layer->tree->node.y;
        } else {
            *dx = sx - window->scene_surface->node.x;
            *dy = sy - window->scene_surface->node.y;
        }
    }
}

static void fetch_offset(struct wsland_window *window, int *offset_x, int *offset_y) {
    if (window_check(window)) {
        if (window->role == TOPLEVEL) {
            wlr_scene_node_coords(&window->scene_layer->tree->node, offset_x, offset_y);
        } else {
            wlr_scene_node_coords(&window->scene_surface->node, offset_x, offset_y);
        }
    } else {
        *offset_x = 0;
        *offset_y = 0;
    }
}

static void window_resize(struct wsland_window *window, struct wlr_box box) {
    if (window_check(window) && !wlr_box_empty(&box)) {
        if (box.x != window->scene->node.x || box.y != window->scene->node.y) {
            wlr_scene_node_set_position(&window->scene->node, box.x, box. y);
        }

        struct wlr_layer_surface_v1 *wls = window->layer;
        if (window->role == TOPLEVEL && ((int)wls->current.actual_width != box.width || (int)wls->current.actual_height != box.height)) {
            window->render.resizing = wlr_layer_surface_v1_configure(window->layer, box.width, box.height);
        }
    }
}

static void window_motion(struct wsland_window *window, int pos_x, int pos_y) {
    if (window_check(window)) {
        wlr_scene_node_set_position(&window->scene_layer->tree->node, pos_x, pos_y);
    }
}

static void window_activate(struct wsland_window *window, bool enabled) {
    if (window_check(window)) {}
}

static void window_shutdown(struct wsland_window *window) {
    if (window_check(window)) {
        wlr_layer_surface_v1_destroy(window->layer);
    }
}

static void window_surface_position(struct wsland_window *window, double *sx, double *sy) {
    if (window_check(window)) {}
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

static void window_output_config(struct wsland_window *window, bool activate) {
    if (window_check(window)) {
        if (wlr_layer_surface_v1_get_exclusive_edge(window->layer) == WLR_EDGE_NONE) {
            return;
        }

        struct wsland_output *wo;
        if ((wo = window->render.wo) != NULL) {
            struct wlr_layer_surface_v1_state current = window->layer->current;

            if (activate) {
                wo->max_area.y += current.exclusive_zone;
                wo->max_area.height -= current.exclusive_zone;
            } else {
                wo->max_area.y -= current.exclusive_zone;
                wo->max_area.height += current.exclusive_zone;
            }
        }
    }
}

static void window_center(struct wsland_window *window) {
    if (window_check(window)) {
        if (window->layer->current.layer != ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY) {
            return;
        }

        struct wsland_output *wo;
        if ((wo = window->render.wo) != NULL) {
            struct wlr_box area = {
                .x = wo->max_area.x + (wo->max_area.width - window->metadata.target.width) / 2,
                .y = wo->max_area.y + (wo->max_area.height - window->metadata.target.height) / 2,
            };

            struct wsland_window *pw;
            if ((pw = window->parent) != NULL && window_check(pw)) {
                area.x = (pw->metadata.target.width - window->metadata.target.width) / 2;
                area.y = (pw->metadata.target.height - window->metadata.target.height) / 2;
            }

            window->handle->window_motion(window, area.x, area.y);
        }
    }
}

static void window_column(struct wsland_window *window) {
    if (window_check(window)) {}
}

static void layer_toplevel_map(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.map);
    struct wsland_server *server = window->server;

    window->metadata.mapped = true;
    if (server->service->window_parent_reset(window) && window->parent) {
        wlr_scene_node_reparent(&window->scene->node, window->parent->scene);
    }

    switch (window->layer->current.layer) {
        case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
            window_output_config(window, true);
            window->metadata.above = true;
            break;
        case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
            window->metadata.above = true;
            window->render.center = true;
            break;
        default:
            return;
    }

    server->adapter->handle->backend_window_create(window);

    wl_list_init(&window->link);
    wl_list_insert(&server->windows, &window->link);
    server->service->dispatch_window_focus(window);
}

static void layer_toplevel_unmap(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.unmap);
    struct wsland_server *server = window->server;

    window_output_config(window, false);

    server->service->window_focus_clear(window);
    server->adapter->handle->backend_window_destroy(window);
    wl_list_remove(&window->link);

    window->metadata.mapped = false;
}

static void layer_toplevel_commit(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.commit);
    struct wlr_layer_surface_v1 *wls = window->layer;

    if (wls->initial_commit) {
        struct wsland_output *wo;
        if ((wls->output && (wo = wls->output->data) != NULL) || (wo = window->server->service->fetch_output(window->server, 0, 0)) != NULL) {
            wlr_scene_layer_surface_v1_configure(window->scene_layer, &wo->full_area, &wo->max_area);
            window->render.wo = wo;
        }
    }

    if (window->render.resizing && window->render.resizing <= window->layer->current.configure_serial) {
        window->render.resizing = false;
    }
}

static void layer_toplevel_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.new_popup.link);
    wl_list_remove(&window->events.destroy.link);

    if (window->scene->node.enabled) { wlr_scene_node_destroy(&window->scene->node); }
    free(window);
}

static void xdg_popup_unconstrain(struct wsland_window *popup) {
    struct wsland_output *wo;
    struct wsland_window *wt = popup->server->service->find_toplevel_window(&popup->scene->node);
    if ((wo = popup->render.wo) != NULL && wt) {
        int pos_x, pos_y;
        wt->handle->fetch_offset(wt, &pos_x, &pos_y);

        struct wlr_box box_space = {
            .x = wo->max_area.x - pos_x, .y = wo->max_area.y - pos_y,
            .width = wo->max_area.width, .height = wo->max_area.height
        };
        wlr_xdg_popup_unconstrain_from_box(popup->wayland->popup, &box_space);
    }
}

static void layer_popup_map(struct wl_listener *listener, void *user_data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.map);
    struct wsland_server *server = popup->server;

    popup->metadata.mapped = true;
    popup->render.wo = popup->handle->fetch_output(popup);
    xdg_popup_unconstrain(popup);

    server->adapter->handle->backend_window_create(popup);

    wl_list_init(&popup->link);
    wl_list_insert(&server->windows, &popup->link);
}

static void layer_popup_unmap(struct wl_listener *listener, void *user_data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.unmap);
    struct wsland_server *server = popup->server;

    server->adapter->handle->backend_window_destroy(popup);
    wl_list_remove(&popup->link);

    popup->metadata.mapped = false;
}

static void layer_popup_commit(struct wl_listener *listener, void *user_data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.commit);

    if (popup->wayland->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->wayland);
    }
}

static void layer_popup_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.destroy);

    wl_list_remove(&popup->events.map.link);
    wl_list_remove(&popup->events.unmap.link);
    wl_list_remove(&popup->events.commit.link);
    wl_list_remove(&popup->events.new_popup.link);
    wl_list_remove(&popup->events.destroy.link);

    if (popup->scene->node.enabled) { wlr_scene_node_destroy(&popup->scene->node); }
    free(popup);
}

static const struct wsland_window_handle wsland_layer_window_impl = {
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

static void window_new_popup(struct wl_listener *listener, void *user_data) {
    struct wsland_window *parent = wl_container_of(listener, parent, events.new_popup);
    struct wlr_xdg_popup *wxp = user_data;

    if (!window_check(parent)) {
        return;
    }

    struct wsland_window *popup = calloc(1, sizeof(*popup));
    popup->handle = &wsland_layer_window_impl;
    popup->server = parent->server;
    popup->wayland = wxp->base;
    popup->parent = parent;

    popup->type = LAYER;
    popup->role = POPUP;

    struct wlr_scene_tree *wst = parent->role == TOPLEVEL ? parent->scene_layer->tree : parent->scene_surface;
    popup->scene = wxp->base->surface->data = wlr_scene_tree_create(wst);
    popup->scene_surface = wlr_scene_xdg_surface_create(popup->scene, popup->wayland);
    popup->scene_border = wlr_scene_buffer_create(popup->scene, NULL);

    popup->scene->node.data = popup->scene_border->node.data = popup->scene_surface->node.data = popup;
    popup->scene_border->point_accepts_input = parent->server->service->window_border_accepts_input;

    pixman_region32_init(&popup->render.damage);
    parent->server->service->window_surface_register(wxp->base->surface, popup);
    LISTEN(&wxp->base->surface->events.map, &popup->events.map, layer_popup_map);
    LISTEN(&wxp->base->surface->events.unmap, &popup->events.unmap, layer_popup_unmap);
    LISTEN(&wxp->base->surface->events.commit, &popup->events.commit, layer_popup_commit);
    LISTEN(&wxp->events.destroy, &popup->events.destroy, layer_popup_destroy);

    LISTEN(&wxp->base->events.new_popup, &popup->events.new_popup, window_new_popup);
}

static void layer_new_surface(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.layer_new_surface);
    struct wlr_layer_surface_v1 *wls = user_data;

    struct wlr_scene_tree *tree = NULL;
    switch (wls->current.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        tree = server->scene_top;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        tree = server->scene_bottom;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        tree = server->scene_overlay;
        break;
    default:
        return;
    }

    struct wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_layer_window_impl;
    window->server = server;
    window->layer = wls;

    window->type = LAYER;
    window->role = TOPLEVEL;

    window->scene = wls->surface->data = wlr_scene_tree_create(tree);
    window->scene_layer = wlr_scene_layer_surface_v1_create(window->scene, window->layer);
    window->scene_border = wlr_scene_buffer_create(window->scene, NULL);

    window->scene->node.data = window->scene_border->node.data = window->scene_layer->tree->node.data = window;
    window->scene_border->point_accepts_input = server->service->window_border_accepts_input;

    pixman_region32_init(&window->render.damage);
    server->service->window_surface_register(wls->surface, window);
    LISTEN(&wls->surface->events.map, &window->events.map, layer_toplevel_map);
    LISTEN(&wls->surface->events.unmap, &window->events.unmap, layer_toplevel_unmap);
    LISTEN(&wls->surface->events.commit, &window->events.commit, layer_toplevel_commit);
    LISTEN(&wls->events.destroy, &window->events.destroy, layer_toplevel_destroy);

    LISTEN(&wls->events.new_popup, &window->events.new_popup, window_new_popup);
}

void layer_event_init(struct wsland_server *server) {
    LISTEN(&server->layer_shell->events.new_surface, &server->events.layer_new_surface, layer_new_surface);
}