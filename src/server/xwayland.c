// ReSharper disable All
#include <wlr/xwayland/xwayland.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/utils/log.h"


static char* fetch_title(wsland_window *window) {
    if (window->type == XWAYLAND && !window->parent) {
        return window->xwayland->title;
    }
    return NULL;
}

static wsland_window* fetch_parent(wsland_window *window) {
    if ((window->type == XWAYLAND || window->type == POPUP) && window->xwayland->parent) {
        return window->xwayland->parent->data;
    }
    return NULL;
}

static wsland_output *fetch_output(wsland_window *window) {
    int pos_x = MAX(1, window->xwayland->x);
    int pos_y = MAX(1, window->xwayland->y);

    struct wlr_output *wo = wlr_output_layout_output_at(window->server->output_layout, pos_x, pos_y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) { return output; }
    }

    return NULL;
}

static struct wlr_surface *fetch_surface(wsland_window *window) {
    return window->xwayland->surface;
}

static struct wlr_box fetch_geometry(wsland_window *window) {
    return (struct wlr_box) {
        window->xwayland->x, window->xwayland->y,
        window->xwayland->width, window->xwayland->height
    };
}

static bool fetch_activate(wsland_window *window) {
    struct wlr_surface *focus_surface = window->server->seat->keyboard_state.focused_surface;
    struct wlr_surface *current_surface = fetch_surface(window);
    return focus_surface == current_surface;
}

static void window_resize(wsland_window *window, int width, int height) {
    wlr_xwayland_surface_configure(
        window->xwayland, window->xwayland->x, window->xwayland->y,
        width, height
    );
}

static void window_center(wsland_window *window) {
    wsland_output *output = fetch_output(window);

    if (output) {
        struct wlr_box bounds = {0};
        wlr_surface_get_extents(window->xwayland->surface, &bounds);

        int pos_x = output->work_area.x + (output->work_area.width - bounds.width) / 2;
        int pos_y = output->work_area.y + (output->work_area.height - bounds.height) / 2;
        wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);

        wlr_xwayland_surface_configure(
            window->xwayland, pos_x, pos_y, window->xwayland->width, window->xwayland->height
        );
    }
}

static void window_motion(wsland_window *window,  int pos_x, int pos_y) {
    wlr_scene_node_set_position(&window->tree->node, pos_x, pos_y);

    wlr_xwayland_surface_configure(
        window->xwayland, pos_x, pos_y,
        window->xwayland->width, window->xwayland->height
    );
}

static void window_activate(wsland_window *window, bool enabled) {
    wlr_xwayland_surface_activate(window->xwayland, enabled);

    if (!enabled) {
        wlr_seat_keyboard_notify_clear_focus(window->server->seat);
    }
}

static void window_maximize(wsland_window *window) {
    if (window->xwayland->surface->mapped) {
        wsland_output *output = fetch_output(window);

        if (output) {
            bool maximized = window->xwayland->maximized_horz && window->xwayland->maximized_vert;
            if (maximized) {
                if (wlr_box_empty(&window->before)) {
                    window->before.width = output->work_area.width / 2;
                    window->before.height = output->work_area.height / 2;
                }
                wlr_scene_node_set_position(&window->tree->node, window->before.x, window->before.y);

                wlr_xwayland_surface_configure(
                    window->xwayland, window->before.x, window->before.y, window->before.width, window->before.height
                );
            } else {
                window->before = (struct wlr_box){
                    window->tree->node.x,
                    window->tree->node.y,
                    window->xwayland->width,
                    window->xwayland->height,
                };
                wlr_scene_node_set_position(&window->tree->node, output->work_area.x, output->work_area.y);

                wlr_xwayland_surface_configure(
                    window->xwayland, output->work_area.x, output->work_area.y, output->work_area.width, output->work_area.height
                );
            }
            wlr_xwayland_surface_set_maximized(window->xwayland, !maximized, !maximized);
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

static bool window_click_cannot(wsland_window *window) {
    return window->type == POPUP || window->xwayland->fullscreen || (window->xwayland->maximized_horz && window->xwayland->maximized_vert);
}

static bool window_resize_cannot(wsland_window *window) {
    return false;
}

static void unmanaged_map(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.map);
    unmanaged->parent = unmanaged->handle->fetch_parent(unmanaged);

    if (!unmanaged->parent) {
        wsland_window *parent;
        wl_list_for_each(parent, &unmanaged->server->windows, server_link) {
            if (parent->type == XWAYLAND && parent->xwayland->pid == unmanaged->xwayland->pid) {
                unmanaged->parent = parent;
                break;
            }
        }
    }

    if (unmanaged->parent) {
        unmanaged->tree = wlr_scene_subsurface_tree_create(&unmanaged->server->scene->tree, unmanaged->xwayland->surface);
        unmanaged->tree->node.data = unmanaged->xwayland->surface->data = unmanaged;

        int pos_x = unmanaged->xwayland->x;
        int pos_y = unmanaged->xwayland->y;
        wsland_output *output = unmanaged->handle->fetch_output(unmanaged);
        if (output && (output->work_area.width != output->monitor.width || output->work_area.height != output->monitor.height)) {
            int offset_x = unmanaged->xwayland->x + unmanaged->xwayland->width - (output->work_area.x + output->work_area.width);
            int offset_y = unmanaged->xwayland->y + unmanaged->xwayland->height - (output->work_area.y + output->work_area.height);
            offset_x = offset_x < 0 ? 0 : offset_x + 8;
            offset_y = offset_y < 0 ? 0 : offset_y + 8;
            pos_x -= offset_x;
            pos_y -= offset_y;

            wlr_xwayland_surface_configure(
                unmanaged->xwayland,
                unmanaged->xwayland->x - offset_x,
                unmanaged->xwayland->y - offset_y,
                unmanaged->xwayland->width, unmanaged->xwayland->height
            );
        }
        wlr_scene_node_set_position(&unmanaged->tree->node, pos_x, pos_y);
        wl_list_insert(&unmanaged->parent->children, &unmanaged->parent_link);
    }

    wl_list_insert(&unmanaged->server->windows, &unmanaged->server_link);
    unmanaged->server->zorder = true;
}

static void unmanaged_unmap(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.unmap);

    wl_list_remove(&unmanaged->server_link);
    wl_list_remove(&unmanaged->parent_link);
    wlr_scene_node_destroy(&unmanaged->tree->node);
}

static void unmanaged_associate(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.associate);

    LISTEN(&unmanaged->xwayland->surface->events.map, &unmanaged->events.map, unmanaged_map);
    LISTEN(&unmanaged->xwayland->surface->events.unmap, &unmanaged->events.unmap, unmanaged_unmap);
}

static void unmanaged_dissociate(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.dissociate);

    wl_list_remove(&unmanaged->events.map.link);
    wl_list_remove(&unmanaged->events.unmap.link);
}

static void unmanaged_set_geometry(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.set_geometry);

    if (unmanaged->tree && !unmanaged->server->framing) {
        wlr_scene_node_set_position(&unmanaged->tree->node, unmanaged->xwayland->x, unmanaged->xwayland->y);
    }
}
static void unmanaged_request_configure(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = user_data;

    wlr_scene_node_set_position(&unmanaged->tree->node, event->x, event->y);
    wlr_xwayland_surface_configure(unmanaged->xwayland, event->x, event->y, event->width, event->height);
}

static void unmanaged_destroy(struct wl_listener *listener, void *user_data) {
    wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.destroy);

    wl_list_remove(&unmanaged->events.request_configure.link);
    wl_list_remove(&unmanaged->events.set_geometry.link);
    wl_list_remove(&unmanaged->events.dissociate.link);
    wl_list_remove(&unmanaged->events.associate.link);
    wl_list_remove(&unmanaged->events.destroy.link);
    wl_list_remove(&unmanaged->children);

    wl_signal_emit(&unmanaged->server->events.wsland_window_destroy, unmanaged);
    free(unmanaged);
}

static void xwayland_map(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.map);
    window->parent = window->handle->fetch_parent(window);

    window->tree = wlr_scene_tree_create(&window->server->scene->tree);
    wlr_scene_surface_create(window->tree, window->xwayland->surface);
    window->tree->node.data = window->xwayland->surface->data = window;
    window_center(window);

    if (window->parent) {
        wl_list_insert(&window->parent->children, &window->parent_link);
    }

    wl_list_insert(&window->server->windows, &window->server_link);
    window->server->zorder = true;
}

static void xwayland_unmap(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.unmap);

    wl_list_remove(&window->server_link);
    wl_list_remove(&window->parent_link);
    wlr_scene_node_destroy(&window->tree->node);
}

static void xwayland_associate(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.associate);

    LISTEN(&window->xwayland->surface->events.map, &window->events.map, xwayland_map);
    LISTEN(&window->xwayland->surface->events.unmap, &window->events.unmap, xwayland_unmap);
}

static void xwayland_set_hints(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.set_hints);

    if (!window->xwayland->hints) {
        return;
    }
    xcb_icccm_wm_hints_get_urgency(window->xwayland->hints);
}

static void xwayland_dissociate(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.dissociate);

    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.map.link);
}

static void xwayland_request_configure(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = user_data;

    if (!window->xwayland->surface || !window->xwayland->surface->mapped) {
        wlr_xwayland_surface_configure(window->xwayland, event->x, event->y, event->width, event->height);
    }
}

static void xwayland_request_activate(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.request_activate);

    window->server->handle->dispatch_window_focus(window);
}

static void xwayland_request_maximize(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.request_maximize);

    if (window->xwayland->surface) {
        window_maximize(window);
    }
}

static void xwayland_destroy(struct wl_listener *listener, void *user_data) {
    wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_list_remove(&window->events.request_configure.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_activate.link);
    wl_list_remove(&window->events.dissociate.link);
    wl_list_remove(&window->events.associate.link);
    wl_list_remove(&window->events.set_hints.link);
    wl_list_remove(&window->events.destroy.link);
    wl_list_remove(&window->children);

    wl_signal_emit(&window->server->events.wsland_window_destroy, window);
    free(window);
}

wsland_window_handle wsland_xwayland_window_impl = {
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

static wsland_window *create_unmanaged(wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    wsland_window *unmanaged = calloc(1, sizeof(*unmanaged));
    unmanaged->handle = &wsland_xwayland_window_impl;
    unmanaged->xwayland = xwayland_surface;
    unmanaged->server = server;
    unmanaged->type = POPUP;

    unmanaged->xwayland->data = unmanaged;

    LISTEN(&xwayland_surface->events.set_geometry, &unmanaged->events.set_geometry, unmanaged_set_geometry);
    LISTEN(&xwayland_surface->events.request_configure, &unmanaged->events.request_configure, unmanaged_request_configure);
    LISTEN(&xwayland_surface->events.dissociate, &unmanaged->events.dissociate, unmanaged_dissociate);
    LISTEN(&xwayland_surface->events.associate, &unmanaged->events.associate, unmanaged_associate);
    LISTEN(&xwayland_surface->events.destroy, &unmanaged->events.destroy, unmanaged_destroy);
    wl_list_init(&unmanaged->parent_link);
    wl_list_init(&unmanaged->server_link);
    wl_list_init(&unmanaged->children);
    return unmanaged;
}

static wsland_window *create_xwayland_window(wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_xwayland_window_impl;
    window->xwayland = xwayland_surface;
    window->server = server;

    window->type = XWAYLAND;
    window->xwayland->data = window;

    LISTEN(&xwayland_surface->events.request_configure, &window->events.request_configure, xwayland_request_configure);
    LISTEN(&xwayland_surface->events.request_activate, &window->events.request_activate, xwayland_request_activate);
    LISTEN(&xwayland_surface->events.request_maximize, &window->events.request_maximize, xwayland_request_maximize);
    LISTEN(&xwayland_surface->events.dissociate, &window->events.dissociate, xwayland_dissociate);
    LISTEN(&xwayland_surface->events.associate, &window->events.associate, xwayland_associate);
    LISTEN(&xwayland_surface->events.set_hints, &window->events.set_hints, xwayland_set_hints);
    LISTEN(&xwayland_surface->events.destroy, &window->events.destroy, xwayland_destroy);
    wl_list_init(&window->parent_link);
    wl_list_init(&window->server_link);
    wl_list_init(&window->children);
    return window;
}

static void xwayland_ready(struct wl_listener *listener, void *user_data) {
    wsland_server *server = wl_container_of(listener, server, events.xwayland_ready);

    wlr_xwayland_set_seat(server->xwayland, server->seat);
}

static void xwayland_new_toplevel(struct wl_listener *listener, void *user_data) {
    wsland_server *server = wl_container_of(listener, server, events.xwayland_new_toplevel);
    struct wlr_xwayland_surface *xwayland_surface = user_data;

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