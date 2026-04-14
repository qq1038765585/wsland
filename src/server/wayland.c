// ReSharper disable All
#include <assert.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wlr/interfaces/wlr_output.h>

#include <wlr/util/edges.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_subcompositor.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


static char* fetch_title(wsland_window *window) {
    if (window->type == TOPLEVEL && !window->parent) {
        return window->wayland->toplevel->title;
    }
    return NULL;
}

static wsland_window *fetch_parent(wsland_window *window) {
    if (window->type == TOPLEVEL && window->wayland->toplevel->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->toplevel->parent->base->data;
        if (parent_tree) { return parent_tree->node.data; }
    } else if (window->type == POPUP) {
        return window->parent;
    }

    return NULL;
}

static wsland_output *fetch_output(wsland_window *window) {
    int pos_x = MAX(0, window->tree->node.x);
    int pos_y = MAX(0, window->tree->node.y);

    if (window->parent) {
        pos_x = MAX(0, window->parent->current.x);
        pos_y = MAX(0, window->parent->current.y);
    }

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static struct wlr_surface *fetch_surface(wsland_window *window) {
    return window->wayland->surface;
}

static struct wlr_box fetch_geometry(wsland_window *window) {
    return window->wayland->toplevel->base->geometry;
}

static bool fetch_activate(wsland_window *window) {
    struct wlr_surface *focus_surface = window->server->seat->keyboard_state.focused_surface;
    struct wlr_surface *current_surface = fetch_surface(window);
    return focus_surface == current_surface;
}

static void window_resize(wsland_window *window, int width, int height) {
    if (window->type != TOPLEVEL) {
        return;
    }

    window->resize_serial = wlr_xdg_toplevel_set_size(window->wayland->toplevel, width, height);
}

static void window_title_detection(wsland_window *window) {
    if (window->type != TOPLEVEL) {
        return;
    }

    window->title = window->wayland->toplevel->title;
}

static void window_parent_detection(wsland_window *window) {
    if (window->wayland->toplevel->parent && window->wayland->toplevel->parent->base->data) {
        wsland_window *parent = window->wayland->toplevel->parent->base->data;
        window->parent_id = parent->window_id;
    }
}

static void window_center(wsland_window *window) {
    if (window->type != TOPLEVEL) {
        return;
    }

    wsland_output *output;
    if ((output = fetch_output(window)) != NULL) {
        struct wlr_box bounds;
        wlr_surface_get_extents(window->wayland->surface, &bounds);

        int pos_x = output->work_area.x + (output->work_area.width - bounds.width) / 2;
        int pos_y = output->work_area.y + (output->work_area.height - bounds.height) / 2;
        wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
    }
}

static void window_motion(wsland_window *window, int pos_x, int pos_y) {
    wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
}

static void window_activate(wsland_window *window, bool enabled) {
    if (window->type != TOPLEVEL) {
        return;
    }

    wlr_xdg_toplevel_set_activated(window->wayland->toplevel, enabled);

    if (!enabled) {
        wlr_seat_keyboard_notify_clear_focus(window->server->seat);
    }
}

static void window_maximize(wsland_window *window) {
    if (window->type != TOPLEVEL) {
        return;
    }

    if (window->wayland->toplevel->base->surface->mapped) {
        wsland_output *output = fetch_output(window);

        if (output) {
            if (window->wayland->toplevel->current.maximized) {
                if (wlr_box_empty(&window->before)) {
                    window->before.width = output->work_area.width / 2;
                    window->before.height = output->work_area.height / 2;
                }

                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland->toplevel, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->toplevel->base->current.geometry.width,
                    window->wayland->toplevel->base->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->work_area.x, output->work_area.y);
                wlr_xdg_toplevel_set_size(window->wayland->toplevel, output->work_area.width, output->work_area.height);
            }
            wlr_xdg_toplevel_set_maximized(window->wayland->toplevel, !window->wayland->toplevel->current.maximized);
        }
        wlr_xdg_surface_schedule_configure(window->wayland->toplevel->base);
    }
}

static void window_shutdown(wsland_window *window) {
    if (window->type != TOPLEVEL) {
        return;
    }

    wlr_xdg_toplevel_send_close(window->wayland->toplevel);
}

static void surface_activate(struct wlr_surface *surface, bool enabled) {
    struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);

    if (xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_activated(xdg_toplevel, enabled);
    }
}

static bool window_grab_cannot(wsland_window *window) {
    if (window->type != TOPLEVEL) {
        return true;
    }

    return window->wayland->toplevel->current.maximized || window->wayland->toplevel->current.fullscreen;
}

static bool window_click_cannot(wsland_window *window) {
    return window->type == POPUP || window->wayland->toplevel->current.maximized || window->wayland->toplevel->current.fullscreen;
}

static bool window_resize_cannot(wsland_window *window) {
    if (window->resize_serial && window->resize_serial != window->wayland->pending.configure_serial) {
        return true;
    }
    return false;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);
    window->parent = window->handle->fetch_parent(window);
    window->wayland->surface->data = window;
    window_center(window);

    if (window->parent) {
        wl_list_insert(&window->parent->children, &window->parent_link);
    }

    wl_list_insert(&window->server->windows, &window->server_link);
    window->server->zorder = true;
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    wl_list_remove(&window->server_link);
    wl_list_remove(&window->parent_link);
    wlr_scene_node_destroy(&window->tree->node);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.commit);

    if (window->wayland->initial_commit) {
        wlr_xdg_toplevel_set_size(window->wayland->toplevel, 0, 0);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.new_popup.link);
    wl_list_remove(&window->events.request_move.link);
    wl_list_remove(&window->events.request_resize.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_fullscreen.link);
    wl_list_remove(&window->events.destroy.link);
    wl_list_remove(&window->children);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    free(window);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_move);
    struct wlr_xdg_toplevel_move_event *event = data;

    window->server->handle->begin_window_interactive(window, WSLAND_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    window->server->handle->begin_window_interactive(window, WSLAND_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_maximize);

    window_maximize(window);
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_fullscreen);

    if (window->wayland->surface->mapped) {
        wsland_output *output = fetch_output(window);

        if (output) {
            if (window->wayland->toplevel->current.fullscreen) {
                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland->toplevel, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->current.geometry.width,
                    window->wayland->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->monitor.x, output->monitor.y);
                wlr_xdg_toplevel_set_size(window->wayland->toplevel, output->monitor.width, output->monitor.height);
            }
            wlr_xdg_toplevel_set_fullscreen(window->wayland->toplevel, !window->wayland->toplevel->current.fullscreen);
        }
        wlr_xdg_surface_schedule_configure(window->wayland);
    }
}

static void popup_map(struct wl_listener *listener, void *data) {
    wsland_window *popup = wl_container_of(listener, popup, events.map);
    popup->wayland->surface->data = popup;

    if (popup->parent) {
        wl_list_insert(&popup->parent->children, &popup->parent_link);
    }

    wsland_output *output = popup->handle->fetch_output(popup);
    if (output) {
        struct wlr_box box_space = {
            .x = output->work_area.x - popup->parent->tree->node.x,
            .y = output->work_area.y - popup->parent->tree->node.y,
            .width = output->work_area.width, .height = output->work_area.height
        };
        wlr_xdg_popup_unconstrain_from_box(popup->wayland->popup, &box_space);
    }

    wl_list_insert(&popup->server->windows, &popup->server_link);
    popup->server->zorder = true;
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    wsland_window *popup = wl_container_of(listener, popup, events.commit);

    if (popup->wayland->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->wayland);
    } else if (popup->wayland->surface->mapped) {
        wsland_output *output = popup->handle->fetch_output(popup);
        if (output) {
            int pos_x = popup->tree->node.x;
            int pos_y = popup->tree->node.y;
            if (popup->parent) {
                pos_x += popup->parent->tree->node.x;
                pos_y += popup->parent->tree->node.y;
            }
            wlr_scene_node_set_position(&popup->tree->node, pos_x, pos_y);
        }
    }
}

static void xdg_popup_reposition(struct wl_listener *listener, void *data) {
    wsland_window *popup = wl_container_of(listener, popup, events.reposition);

    int pos_x = popup->parent->tree->node.x + popup->wayland->popup->current.geometry.x;
    int pos_y = popup->parent->tree->node.y + popup->wayland->popup->current.geometry.y;
    wlr_scene_node_set_position(&popup->tree->node, pos_x, pos_y);
}

static void popup_unmap(struct wl_listener *listener, void *data) {
    wsland_window *popup = wl_container_of(listener, popup, events.unmap);

    wl_list_remove(&popup->server_link);
    wl_list_remove(&popup->parent_link);
    wlr_scene_node_destroy(&popup->tree->node);
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    wsland_window *popup = wl_container_of(listener, popup, events.destroy);

    wl_list_remove(&popup->events.map.link);
    wl_list_remove(&popup->events.unmap.link);
    wl_list_remove(&popup->events.commit.link);
    wl_list_remove(&popup->events.new_popup.link);
    wl_list_remove(&popup->events.reposition.link);
    wl_list_remove(&popup->events.destroy.link);

    wl_signal_emit(&popup->server->events.wsland_window_destroy, popup);
    free(popup);
}

wsland_window_handle wsland_wayland_window_impl = {
    .fetch_title = fetch_title,
    .fetch_parent = fetch_parent,
    .fetch_output = fetch_output,
    .fetch_surface = fetch_surface,
    .fetch_geometry = fetch_geometry,
    .fetch_activate = fetch_activate,

    .window_resize = window_resize,
    .window_center = window_center,
    .window_motion = window_motion,
    .window_activate = window_activate,
    .window_maximize = window_maximize,
    .window_shutdown = window_shutdown,
    .surface_activate = surface_activate,
    .window_grab_cannot = window_grab_cannot,
    .window_click_cannot = window_click_cannot,
    .window_resize_cannot = window_resize_cannot,
};

static void window_new_popup(struct wl_listener *listener, void *data) {
    wsland_window *parent = wl_container_of(listener, parent, events.new_popup);
    struct wlr_xdg_popup *popup = data;

    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_wayland_window_impl;
    window->server = parent->server;
    window->wayland = popup->base;
    window->parent = parent;
    window->type = POPUP;

    window->tree = wlr_scene_xdg_surface_create(&window->server->scene->tree, window->wayland);
    window->wayland->data = window->tree;
    window->tree->node.data = window;

    LISTEN(&popup->base->surface->events.map, &window->events.map, popup_map);
    LISTEN(&popup->base->surface->events.unmap, &window->events.unmap, popup_unmap);
    LISTEN(&popup->base->surface->events.commit, &window->events.commit, xdg_popup_commit);
    LISTEN(&popup->events.reposition, &window->events.reposition, xdg_popup_reposition);
    LISTEN(&popup->events.destroy, &window->events.destroy, xdg_popup_destroy);
    LISTEN(&popup->base->events.new_popup, &window->events.new_popup, window_new_popup);

    wl_list_init(&window->server_link);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->children);
}

static void wayland_new_toplevel(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.wayland_new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;

    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_wayland_window_impl;
    window->wayland = toplevel->base;
    window->server = server;
    window->type = TOPLEVEL;

    window->tree = wlr_scene_xdg_surface_create(&server->scene->tree, window->wayland);
    window->wayland->data = window->tree;
    window->tree->node.data = window;

    LISTEN(&toplevel->base->surface->events.map, &window->events.map, xdg_toplevel_map);
    LISTEN(&toplevel->base->surface->events.unmap, &window->events.unmap, xdg_toplevel_unmap);
    LISTEN(&toplevel->base->surface->events.commit, &window->events.commit, xdg_toplevel_commit);
    LISTEN(&toplevel->events.destroy, &window->events.destroy, xdg_toplevel_destroy);

    LISTEN(&toplevel->events.request_move, &window->events.request_move, xdg_toplevel_request_move);
    LISTEN(&toplevel->events.request_resize, &window->events.request_resize, xdg_toplevel_request_resize);
    LISTEN(&toplevel->events.request_maximize, &window->events.request_maximize, xdg_toplevel_request_maximize);
    LISTEN(&toplevel->events.request_fullscreen, &window->events.request_fullscreen, xdg_toplevel_request_fullscreen);

    LISTEN(&toplevel->base->events.new_popup, &window->events.new_popup, window_new_popup);

    wl_list_init(&window->server_link);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->children);
}

void wayland_event_init(wsland_server *server) {
    LISTEN(&server->xdg_shell->events.new_toplevel, &server->events.wayland_new_toplevel, wayland_new_toplevel);
}
