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

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


typedef struct wsland_popup {
    struct wlr_xdg_popup *popup;

    struct {
        struct wl_listener map;
        struct wl_listener commit;
        struct wl_listener destroy;
    } events;
} wsland_popup;

static char* window_fetch_title(wsland_window *window) {
    return window->wayland->title;
}

static wsland_window *window_fetch_parent(wsland_window *window) {
    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        if (parent_tree) {
            return parent_tree->node.data;
        }
    }
    return NULL;
}

static wsland_output *window_fetch_output(wsland_window *window) {
    int pos_x = MAX(0, window->tree->node.x);
    int pos_y = MAX(0, window->tree->node.y);

    if (window->wayland->parent) {
        struct wlr_scene_tree *parent_tree = window->wayland->parent->base->data;
        pos_x = MAX(0, window->tree->node.x);
        pos_y = MAX(0, window->tree->node.y);
    }

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static struct wlr_surface *window_fetch_surface(wsland_window *window) {
    return window->wayland->base->surface;
}

static struct wlr_box *window_fetch_geometry(wsland_window *window) {
    return &window->wayland->base->current.geometry;
}

static void window_resize(wsland_window *window, int width, int height) {
    wlr_xdg_toplevel_set_size(window->wayland, width, height);
}

static void window_center(wsland_window *window) {
    wsland_output *output = window_fetch_output(window);
    if (output) {
        struct wlr_box bounds = {0};
        wlr_surface_get_extends(window->wayland->base->surface, &bounds);

        int pos_x = output->work_area.x + (output->work_area.width - bounds.width) / 2;
        int pos_y = output->work_area.y + (output->work_area.height - bounds.height) / 2;
        wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
    }
}

static void window_activate(wsland_window *window, bool enabled) {
    wlr_xdg_toplevel_set_activated(window->wayland, enabled);

    if (!enabled) {
        wlr_seat_keyboard_notify_clear_focus(window->server->seat);
    }
}

static void window_maximize(wsland_window *window) {
    if (window->wayland->base->surface->mapped) {
        wsland_output *output = window_fetch_output(window);

        if (output) {
            if (window->wayland->current.maximized) {
                if (wlr_box_empty(&window->before)) {
                    window->before.width = output->work_area.width / 2;
                    window->before.height = output->work_area.height / 2;
                }

                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->base->current.geometry.width,
                    window->wayland->base->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->work_area.x, output->work_area.y);
                wlr_xdg_toplevel_set_size(window->wayland, output->work_area.width, output->work_area.height);
            }
            wlr_xdg_toplevel_set_maximized(window->wayland, !window->wayland->current.maximized);
        }
        wlr_xdg_surface_schedule_configure(window->wayland->base);
    }
}

static void window_shutdown(wsland_window *window) {
    wlr_xdg_toplevel_send_close(window->wayland);
}

static void surface_activate(struct wlr_surface *surface, bool enabled) {
    struct wlr_xdg_toplevel *xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);

    if (xdg_toplevel != NULL) {
        wlr_xdg_toplevel_set_activated(xdg_toplevel, enabled);
    }
}

static bool window_grab_cannot(wsland_window *window) {
    return window->wayland->current.maximized || window->wayland->current.fullscreen;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);
    wl_list_insert(&window->server->windows, &window->server_link);
    window->server->handle->dispatch_window_focus(window);

    window_center(window);

    if (window->wayland->parent) {
        wsland_window *parent = window->handle->window_fetch_parent(window);

        if (parent) {
            wl_list_insert(&parent->children, &window->parent_link);
        }
    }
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    if (window == window->server->grab.window) {
        window->server->handle->reset_server_cursor(window->server);
    }

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->parent_link);
    wl_list_remove(&window->server_link);
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.commit);

    if (window->wayland->base->initial_commit) {
        wlr_xdg_toplevel_set_size(window->wayland, 0, 0);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.destroy.link);
    wl_list_remove(&window->events.request_move.link);
    wl_list_remove(&window->events.request_resize.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_fullscreen.link);
    wl_list_remove(&window->children);
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

    if (window->wayland->base->surface->mapped) {
        wsland_output *output = window_fetch_output(window);

        if (output) {
            if (window->wayland->current.fullscreen) {
                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xdg_toplevel_set_size(window->wayland, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->wayland->base->current.geometry.width,
                    window->wayland->base->current.geometry.height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->monitor.x, output->monitor.y);
                wlr_xdg_toplevel_set_size(window->wayland, output->monitor.width, output->monitor.height);
            }
            wlr_xdg_toplevel_set_fullscreen(window->wayland, !window->wayland->current.fullscreen);
        }
        wlr_xdg_surface_schedule_configure(window->wayland->base);
    }
}

wsland_window_handle wsland_wayland_window_impl = {
    .window_fetch_title = window_fetch_title,
    .window_fetch_parent = window_fetch_parent,
    .window_fetch_output = window_fetch_output,
    .window_fetch_surface = window_fetch_surface,
    .window_fetch_geometry = window_fetch_geometry,


    .window_resize = window_resize,
    .window_center = window_center,
    .window_activate = window_activate,
    .window_maximize = window_maximize,
    .window_shutdown = window_shutdown,
    .surface_activate = surface_activate,
    .window_grab_cannot = window_grab_cannot,
};

static void wayland_new_toplevel(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.wayland_new_toplevel);
    struct wlr_xdg_toplevel *xdg_toplevel = data;

    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_wayland_window_impl;
    window->wayland = xdg_toplevel;
    window->server = server;
    window->type = WAYLAND;

    window->tree = wlr_scene_xdg_surface_create(
        &window->server->scene->tree, window->wayland->base
    );
    window->tree->node.data = window;
    window->wayland->base->data = window->tree;

    LISTEN(&window->wayland->base->surface->events.map, &window->events.map, xdg_toplevel_map);
    LISTEN(&window->wayland->base->surface->events.unmap, &window->events.unmap, xdg_toplevel_unmap);
    LISTEN(&window->wayland->base->surface->events.commit, &window->events.commit, xdg_toplevel_commit);
    LISTEN(&window->wayland->events.destroy, &window->events.destroy, xdg_toplevel_destroy);

    LISTEN(&window->wayland->events.request_move, &window->events.request_move, xdg_toplevel_request_move);
    LISTEN(&window->wayland->events.request_resize, &window->events.request_resize, xdg_toplevel_request_resize);
    LISTEN(&window->wayland->events.request_maximize, &window->events.request_maximize, xdg_toplevel_request_maximize);
    LISTEN(&window->wayland->events.request_fullscreen, &window->events.request_fullscreen, xdg_toplevel_request_fullscreen);

    wl_list_init(&window->server_link);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->children);
}

static void xdg_popup_map(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.map);

    struct wlr_box toplevel_space_box;
    wlr_surface_get_extends(popup->popup->parent, &toplevel_space_box);
    wlr_xdg_popup_unconstrain_from_box(popup->popup, &toplevel_space_box);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.commit);

    if (popup->popup->base->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->popup->base);
    }
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    wsland_popup *popup = wl_container_of(listener, popup, events.destroy);

    wl_list_remove(&popup->events.map.link);
    wl_list_remove(&popup->events.commit.link);
    wl_list_remove(&popup->events.destroy.link);
    free(popup);
}

static void wayland_new_popup(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.wayland_new_popup);

    wsland_popup *popup = calloc(1, sizeof(*popup));
    if (!popup) {
        wsland_log(SERVER, ERROR, "failed to allocate wsland_popup");
        return;
    }

    popup->popup = data;

    struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->popup->parent);
    assert(parent != NULL);
    struct wlr_scene_tree *parent_tree = parent->data;
    popup->popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->popup->base);

    LISTEN(&popup->popup->base->surface->events.map, &popup->events.map, xdg_popup_map);
    LISTEN(&popup->popup->base->surface->events.commit, &popup->events.commit, xdg_popup_commit);
    LISTEN(&popup->popup->events.destroy, &popup->events.destroy, xdg_popup_destroy);
}

void wayland_event_init(wsland_server *server) {
    LISTEN(&server->xdg_shell->events.new_toplevel, &server->events.wayland_new_toplevel, wayland_new_toplevel);
    LISTEN(&server->xdg_shell->events.new_popup, &server->events.wayland_new_popup, wayland_new_popup);
}
