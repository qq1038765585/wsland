// ReSharper disable All
#include <wlr/xwayland/xwayland.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


typedef struct wsland_unmanaged {
    struct wlr_xwayland_surface *xwayland;
    struct wlr_scene_tree *tree;

    struct {
        struct wl_listener map;
        struct wl_listener unmap;
        struct wl_listener associate;
        struct wl_listener dissociate;
        struct wl_listener request_configure;
        struct wl_listener set_parent;
        struct wl_listener destroy;
    } events;

    wsland_server *server;
} wsland_unmanaged;

static char* window_fetch_title(wsland_window *window) {
    return window->xwayland->title;
}

static wsland_window* window_fetch_parent(wsland_window *window) {
    if (window->xwayland->parent) {
        return window->xwayland->parent->data;
    }
    return NULL;
}

static wsland_output *window_fetch_output(wsland_window *window) {
    int pos_x = MAX(1, window->tree->node.x);
    int pos_y = MAX(1, window->tree->node.y);

    if (window->xwayland->parent) {
        wsland_window *parent = window->xwayland->parent->data;
        if (parent) {
            pos_x = MAX(1, window->tree->node.x);
            pos_y = MAX(1, window->tree->node.y);
        }
    }

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static struct wlr_surface *window_fetch_surface(wsland_window *window) {
    return window->xwayland->surface;
}

static struct wlr_box *window_fetch_geometry(wsland_window *window) {
    return &(struct wlr_box) {
        window->xwayland->x, window->xwayland->y,
        window->xwayland->width, window->xwayland->height
    };
}

static void window_resize(wsland_window *window, int width, int height) {
    wlr_xwayland_surface_configure(
        window->xwayland, window->xwayland->x, window->xwayland->y,
        width, height
    );
}

static void window_center(wsland_window *window) {
    wsland_output *output = window_fetch_output(window);
    if (output) {
        struct wlr_box bounds = {0};
        wlr_surface_get_extends(window->xwayland->surface, &bounds);

        int pos_x = output->work_area.x + (output->work_area.width - bounds.width) / 2;
        int pos_y = output->work_area.y + (output->work_area.height - bounds.height) / 2;
        wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);
    }
}

static void window_activate(wsland_window *window, bool enabled) {
    wlr_xwayland_surface_activate(window->xwayland, enabled);

    if (!enabled) {
        wlr_seat_keyboard_notify_clear_focus(window->server->seat);
    }
}

static void window_maximize(wsland_window *window) {
    if (window->xwayland->surface->mapped) {
        wsland_output *output = window_fetch_output(window);

        if (output) {
            bool maximized = window->xwayland->maximized_horz && window->xwayland->maximized_vert;
            if (maximized) {
                if (wlr_box_empty(&window->before)) {
                    window->before.width = output->work_area.width / 2;
                    window->before.height = output->work_area.height / 2;
                }

                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);
                wlr_xwayland_surface_configure(window->xwayland, window->before.x, window->before.y, window->before.width, window->before.height);
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->xwayland->width,
                    window->xwayland->height,
                };

                wlr_scene_node_set_position(&window->tree->node, output->work_area.x, output->work_area.y);
                wlr_xwayland_surface_configure(window->xwayland, output->work_area.x, output->work_area.y, output->work_area.width, output->work_area.height);
            }
            wlr_xwayland_surface_set_maximized(window->xwayland, !maximized);
        }
    }
}

static void window_shutdown(wsland_window *window) {
    wlr_xwayland_surface_close(window->xwayland);
}

static void surface_activate(struct wlr_surface *surface, bool enabled) {
    struct wlr_xwayland_surface *xwayland_surface = wlr_xwayland_surface_try_from_wlr_surface(surface);

    if (xwayland_surface) {
        wlr_xwayland_surface_activate(xwayland_surface, enabled);
    }
}

static bool window_grab_cannot(wsland_window *window) {
    return (window->xwayland->maximized_horz && window->xwayland->maximized_vert) || window->xwayland->fullscreen || !window->xwayland->surface;
}

static wsland_window *find_parent(wsland_unmanaged *unmanaged) {
    if (unmanaged->xwayland->parent) {
        if (unmanaged->xwayland->parent->override_redirect) {
            wsland_unmanaged *parent = unmanaged->xwayland->parent->data;
            return find_parent(parent);
        } else {
            return unmanaged->xwayland->parent->data;
        }
    }
    return NULL;
}

static void unmanaged_map(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.map);

    wsland_window *parent = find_parent(unmanaged);

    if (!parent) {
        struct wlr_surface *root_surface = unmanaged->server->seat->keyboard_state.focused_surface;
        if (root_surface) {
            struct wlr_xwayland_surface *xwayland_surface = wlr_xwayland_surface_try_from_wlr_surface(root_surface);
            if (xwayland_surface && xwayland_surface->data) {
                parent = xwayland_surface->data;
            }
        }
    }

    if (parent) {
        unmanaged->tree = wlr_scene_subsurface_tree_create(
            parent->tree, unmanaged->xwayland->surface
        );

        int pos_x = unmanaged->xwayland->x - parent->xwayland->x;
        int pos_y = unmanaged->xwayland->y - parent->xwayland->y;
        wlr_scene_node_set_position(&unmanaged->tree->node, pos_x, pos_y);
    }
}

static void unmanaged_unmap(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.unmap);

    wlr_scene_node_destroy(&unmanaged->tree->node);
}

static void unmanaged_associate(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.associate);

    LISTEN(&unmanaged->xwayland->surface->events.map, &unmanaged->events.map, unmanaged_map);
    LISTEN(&unmanaged->xwayland->surface->events.unmap, &unmanaged->events.unmap, unmanaged_unmap);
}

static void unmanaged_dissociate(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.dissociate);

    wl_list_remove(&unmanaged->events.map.link);
    wl_list_remove(&unmanaged->events.unmap.link);
}

static void unmanaged_request_configure(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;

    wlr_xwayland_surface_configure(unmanaged->xwayland, event->x, event->y, event->width, event->height);
    wlr_scene_node_set_position(&unmanaged->tree->node, event->x, event->y);
}

static void unmanaged_set_parent(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.set_parent);
}

static void unmanaged_destroy(struct wl_listener *listener, void *data) {
    wsland_unmanaged *unmanaged = wl_container_of(listener, unmanaged, events.destroy);

    wl_list_remove(&unmanaged->events.request_configure.link);
    wl_list_remove(&unmanaged->events.set_parent.link);
    wl_list_remove(&unmanaged->events.dissociate.link);
    wl_list_remove(&unmanaged->events.associate.link);
    wl_list_remove(&unmanaged->events.destroy.link);
    free(unmanaged);
}

static wsland_unmanaged *create_unmanaged(wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    wsland_unmanaged *unmanaged = calloc(1, sizeof(*unmanaged));
    unmanaged->xwayland = xwayland_surface;
    unmanaged->server = server;

    unmanaged->xwayland->data = unmanaged;

    LISTEN(&xwayland_surface->events.request_configure, &unmanaged->events.request_configure, unmanaged_request_configure);
    LISTEN(&xwayland_surface->events.set_parent, &unmanaged->events.set_parent, unmanaged_set_parent);
    LISTEN(&xwayland_surface->events.dissociate, &unmanaged->events.dissociate, unmanaged_dissociate);
    LISTEN(&xwayland_surface->events.associate, &unmanaged->events.associate, unmanaged_associate);
    LISTEN(&xwayland_surface->events.destroy, &unmanaged->events.destroy, unmanaged_destroy);
    return unmanaged;
}

static void xwayland_map(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.map);

    window->tree = wlr_scene_subsurface_tree_create(&window->server->scene->tree, window->xwayland->surface);
    window->tree->node.data = window;

    window_center(window);

    if (window->xwayland->parent) {
        wsland_window *parent = window->handle->window_fetch_parent(window);
        wl_list_insert(&parent->children, &window->parent_link);
    }

    wl_list_insert(&window->server->windows, &window->server_link);
}

static void xwayland_unmap(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    wlr_scene_node_destroy(&window->tree->node);
    wl_list_remove(&window->parent_link);
    wl_list_remove(&window->server_link);
}

static void xwayland_associate(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.associate);

    LISTEN(&window->xwayland->surface->events.map, &window->events.map, xwayland_map);
    LISTEN(&window->xwayland->surface->events.unmap, &window->events.unmap, xwayland_unmap);
}

static void xwayland_dissociate(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.dissociate);

    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.map.link);
}

static void xwayland_request_configure(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;

    if (!window->xwayland->surface || !window->xwayland->surface->mapped) {
        wlr_xwayland_surface_configure(window->xwayland, event->x, event->y, event->width, event->height);
    }
}

static void xwayland_request_activate(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_activate);

    window->server->handle->dispatch_window_focus(window);
}

static void xwayland_request_maximize(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.request_maximize);

    if (window->xwayland->surface) {
        window_maximize(window);
    }
}

static void xwayland_destroy(struct wl_listener *listener, void *data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    wl_list_remove(&window->events.request_configure.link);
    wl_list_remove(&window->events.dissociate.link);
    wl_list_remove(&window->events.associate.link);
    wl_list_remove(&window->events.destroy.link);
    free(window);
}

wsland_window_handle wsland_xwayland_window_impl = {
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

static wsland_window *create_xwayland_window(wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_xwayland_window_impl;
    window->xwayland = xwayland_surface;
    window->server = server;
    window->type = XWAYLAND;

    window->xwayland->data = window;

    LISTEN(&window->xwayland->events.request_configure, &window->events.request_configure, xwayland_request_configure);
    LISTEN(&window->xwayland->events.request_activate, &window->events.request_activate, xwayland_request_activate);
    LISTEN(&window->xwayland->events.request_maximize, &window->events.request_maximize, xwayland_request_maximize);
    LISTEN(&window->xwayland->events.dissociate, &window->events.dissociate, xwayland_dissociate);
    LISTEN(&window->xwayland->events.associate, &window->events.associate, xwayland_associate);
    LISTEN(&window->xwayland->events.destroy, &window->events.destroy, xwayland_destroy);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->server_link);
    wl_list_init(&window->children);
    return window;
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.xwayland_ready);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
}

static void xwayland_new_toplevel(struct wl_listener *listener, void *data) {
    wsland_server *server = wl_container_of(listener, server, events.xwayland_new_toplevel);
    struct wlr_xwayland_surface *xwayland_surface = data;

    if (xwayland_surface->override_redirect) {
        create_unmanaged(server, xwayland_surface);
        return;
    }
    create_xwayland_window(server, xwayland_surface);
}

void xwayland_event_init(wsland_server *server) {
    LISTEN(&server->xwayland->events.ready, &server->events.xwayland_ready, xwayland_ready);
    LISTEN(&server->xwayland->events.new_surface, &server->events.xwayland_new_toplevel, xwayland_new_toplevel);
}