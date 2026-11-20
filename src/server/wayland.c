#include <assert.h>
#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>

#include "common/config.h"
#include "common/logger.h"
#include "adapter.h"
#include "server.h"


static bool window_check(struct wsland_window *window) {
    assert(window->type == XDG);

    return window->metadata.mapped;
}

static char* fetch_title(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        char *title = window->wayland->toplevel->title;

        if (!title) {
            char ta[32];
            snprintf(ta, sizeof(ta), "%d", window->metadata.ids.window);
            title = ta;
        }
        return title;
    }

    return NULL;
}

static char* fetch_appid(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        char *appid = window->wayland->toplevel->app_id;

        if (!appid) {
            char ta[32];
            snprintf(ta, sizeof(ta), "%d", window->metadata.ids.window);
            appid = ta;
        }
        return appid;
    }

    return NULL;
}

static struct wsland_window *fetch_parent(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        if (wxt->parent) {
            struct wlr_scene_tree *pt = wxt->parent->base->surface->data;
            if (pt) { return pt->node.data; }
        }
    }

    return NULL;
}

static struct wsland_output *fetch_output(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_box box = {0};
        window->handle->fetch_offset(window, &box.x, &box.y);
        return window->server->service->fetch_output(window->server, box.x, box.y);
    }

    return NULL;
}

static struct wlr_box fetch_bounds(struct wsland_window *window) {
    struct wlr_box region = {0};

    if (window_check(window)) {
        wlr_surface_get_extents(window->wayland->surface, &region);
    }

    return region;
}

static struct wlr_surface *fetch_surface(struct wsland_window *window) {
    if (window_check(window)) {
        return window->wayland->surface;
    }

    return NULL;
}

static struct wlr_box fetch_geometry(struct wsland_window *window) {
    if (window_check(window)) {
        if (window->role == TOPLEVEL) {
            return window->wayland->geometry;
        }

        return fetch_bounds(window);
    }

    return (struct wlr_box) {0};
}

static bool fetch_activate(struct wsland_window *window) {
    struct wlr_surface *fs = window->server->seat->keyboard_state.focused_surface;
    struct wlr_surface *cs = fetch_surface(window);
    return fs == cs;
}

static void fetch_render_position(struct wsland_window *window, int sx, int sy, int *dx, int *dy) {
    if (window_check(window)) {
        if (window->role == TOPLEVEL) {
            *dx = sx - window->scene->node.x;
            *dy = sy - window->scene->node.y;
        } else {
            *dx = sx - window->scene_surface->node.x + window->wayland->geometry.x - window->metadata.geometry.x;
            *dy = sy - window->scene_surface->node.y + window->wayland->geometry.y - window->metadata.geometry.y;
        }
    }
}

static void fetch_offset(struct wsland_window *window, int *offset_x, int *offset_y) {
    if (window_check(window) && window->render.wo) {
        if (window->role == TOPLEVEL) {
            wlr_scene_node_coords(&window->scene->node, offset_x, offset_y);
        } else {
            struct wlr_box pg = window->wayland->popup->current.geometry;
            window->server->service->scale_box(window->server, &pg, window->render.wo->scale.desktop);

            if (window->scene_surface->node.x != pg.x || window->scene_surface->node.y != pg.y) {
                wlr_scene_node_set_position(&window->scene_surface->node, pg.x, pg.y);
            }

            wlr_scene_node_coords(&window->scene_surface->node, offset_x, offset_y);
            *offset_x = *offset_x - window->wayland->geometry.x + window->metadata.geometry.x;
            *offset_y = *offset_y - window->wayland->geometry.y + window->metadata.geometry.y;
        }
    } else {
        *offset_x = 0;
        *offset_y = 0;
    }
}

static void window_motion(struct wsland_window *window, int pos_x, int pos_y) {
    if (window_check(window)) {
        wlr_scene_node_set_position(&window->scene->node, pos_x, pos_y);
    }
}

static void window_resize(struct wsland_window *window, struct wlr_box box) {
    if (window_check(window) && !wlr_box_empty(&box)) {
        if (box.x != window->scene->node.x || box.y != window->scene->node.y) {
            wlr_scene_node_set_position(&window->scene->node, box.x, box.y);
        }

        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;
        if (window->role == TOPLEVEL && (wxt->current.width != box.width || wxt->current.height != box.height)) {
            window->render.resizing = wlr_xdg_toplevel_set_size(window->wayland->toplevel, box.width, box.height);
        }
    }
}

static void window_column(struct wsland_window *window) {
    struct wsland_server *server = window->server;

    struct wsland_output *wo;
    if (!window->render.resizing && window->role == TOPLEVEL && (wo = window->render.wo) != NULL) {
        struct wsland_window *pw = window->parent;
        struct wlr_box *wa = &wo->max_area;

        if (window->metadata.column) {
            if (wlr_box_empty(&window->metadata.restore)) {
                window->handle->window_half_output(window);
                window->handle->window_center(window);
            } else {
                window->handle->window_resize(window, window->metadata.restore);
            }
        } else {
            window->metadata.restore = (struct wlr_box){
                .height = window->metadata.geometry.height,
                .width = window->metadata.geometry.width,
                .y = window->scene->node.y,
                .x = window->scene->node.x,
            };

            int CW = window->server->config->border.column_gaps;
            struct wlr_box box = {
                .x = wa->x + CW, .y = wa->y + CW,
                .width = wa->width - CW * 2, .height = wa->height - CW * 2
            };

            if (window->handle->window_have_border(window)) {
                int BW = window->server->config->border.width;
                box.height -= BW * 2;
                box.width -= BW *2;
            }

            if (pw) {
                box.x -= pw->metadata.target.x;
                box.y -= pw->metadata.target.y;
            }

            server->service->scale_box(server, &box, 1 / wo->scale.desktop);
            window->handle->window_resize(window, box);
        }
        window->metadata.column = !window->metadata.column;
    }
}

static void window_center(struct wsland_window *window) {
    struct wsland_output *output;
    if (window->role == TOPLEVEL && (output = window->render.wo) != NULL) {
        struct wlr_box area = {
            .x = output->max_area.x + (output->max_area.width - window->metadata.target.width) / 2,
            .y = output->max_area.y + (output->max_area.height - window->metadata.target.height) / 2,
        };

        struct wsland_window *pw;
        if ((pw = window->parent) != NULL && window_check(pw)) {
            area.x = (pw->metadata.target.width - window->metadata.target.width) / 2;
            area.y = (pw->metadata.target.height - window->metadata.target.height) / 2;
        }

        window->handle->window_motion(window, area.x, area.y);
    }
}

static void window_half_output(struct wsland_window *window) {
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (window_check(window) && (output = window->render.wo) != NULL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        struct wlr_box box = {
            .width = output->max_area.width / 2,
            .height = output->max_area.height / 2
        };

        struct wlr_xdg_toplevel_state c = wxt->current;
        server->service->scale_box(server, &box, 1 / output->scale.desktop);
        if (c.max_width && c.max_height && c.min_width == c.max_width && c.min_height == c.max_height) {
            box.height = c.max_height;
            box.width = c.max_width;
        }

        window->handle->window_resize(window, box);

        window->metadata.column = false;
        wlr_xdg_toplevel_set_maximized(wxt, false);
        wlr_xdg_toplevel_set_fullscreen(wxt, false);
    }
}

static void window_activate(struct wsland_window *window, bool enabled) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wsland_adapter *adapter = window->server->adapter;
        if (!enabled) { wlr_xdg_toplevel_set_activated(window->wayland->toplevel, enabled); }
        window = adapter->handle->find_bottom_window(window, &window->scene->children);
        wlr_xdg_toplevel_set_activated(window->wayland->toplevel, enabled);

        if (!enabled) {
            wlr_seat_keyboard_clear_focus(window->server->seat);

            if (window->buffer_border) {
                struct wlr_render_color BX = window->server->config->border.inactive_color;
                window->server->service->window_border_color(window, &BX);

                pixman_region32_union_rect(
                    &window->render.damage, &window->render.damage, 0, 0,
                    window->metadata.target.width, window->metadata.target.height
                );
            }
        }
    }
}

static void window_minimize(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        wxt->requested.minimized = true;
        wl_signal_emit(&wxt->events.request_minimize, wxt);
    }
}

static void window_maximize(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        wxt->requested.maximized = !wxt->current.maximized;
        wl_signal_emit(&wxt->events.request_maximize, wxt);
    }
}

static void window_fullscreen(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        wxt->requested.fullscreen = !wxt->current.fullscreen;
        wl_signal_emit(&wxt->events.request_fullscreen, wxt);
    }
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
    if (window_check(window)) {
        wlr_scene_node_set_position(&window->scene_surface->node, pos_x, pos_y);
    }
}

static void window_shutdown(struct wsland_window *window) {
    if (window_check(window)) {
        if (window->role == TOPLEVEL) {
            wlr_xdg_toplevel_send_close(window->wayland->toplevel);
        } else {
            wlr_xdg_popup_destroy(window->wayland->popup);
        }
    }
}

static void window_grab_reset(struct wsland_window *window) {

}

static bool window_grab_cannot(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        return window->metadata.column || wxt->current.maximized || wxt->current.fullscreen;
    }

    return true;
}

static bool window_click_cannot(struct wsland_window *window) {

    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        return window->metadata.column || wxt->current.maximized || wxt->current.fullscreen;
    }

    return true;
}

static bool window_have_border(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        return !wxt->current.maximized && !wxt->current.fullscreen;
    }

    return false;
}

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.map);
    struct wsland_server *server = window->server;

    window->metadata.mapped = true;
    window->render.wo = window->handle->fetch_output(window);
    if (server->service->window_parent_reset(window) && window->parent) {
        wlr_scene_node_reparent(&window->scene->node, window->parent->scene);
    } else {
        window_half_output(window);
        window->metadata.icon = true;
    }

    window->render.center = true;
    server->adapter->handle->backend_window_create(window);

    wl_list_init(&window->link);
    wl_list_insert(&server->windows, &window->link);
    server->service->dispatch_window_focus(window);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.unmap);
    struct wsland_server *server = window->server;

    struct wsland_window *bottom;
    while ((bottom = server->adapter->handle->find_bottom_window(window, &window->scene->children)) != NULL) {
        if (window == bottom) { break; }


        wlr_scene_node_reparent(&bottom->scene->node, server->scene_float);
        bottom->handle->window_shutdown(bottom);
    }

    server->service->window_focus_clear(window);
    server->adapter->handle->backend_window_destroy(window);
    wl_list_remove(&window->link);

    window->metadata.mapped = false;
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.commit);
    struct wsland_server *server = window->server;

    if (window->wayland->initial_commit) {
        struct wsland_output *output;
        if ((output = server->service->fetch_output(server, 0, 0)) != NULL) {
            wlr_fractional_scale_v1_notify_scale(window->wayland->surface, output->scale.desktop);
            wlr_surface_set_preferred_buffer_scale(window->wayland->surface, (int)output->scale.desktop);
        }
        if (window->decoration) {
            wlr_xdg_toplevel_decoration_v1_set_mode(window->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        wlr_xdg_toplevel_set_size(window->wayland->toplevel, 0, 0);
        wlr_xdg_toplevel_set_maximized(window->wayland->toplevel, false);
        wlr_xdg_toplevel_set_fullscreen(window->wayland->toplevel, false);
    }

    if (window->render.resizing && window->render.resizing <= window->wayland->current.configure_serial) {
        window->render.resizing = false;
    }

    if (window_check(window) && server->config->output.scale) {
        server->service->window_dest_scale(window);
    }
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.destroy);

    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
    wl_list_remove(&window->events.new_popup.link);
    wl_list_remove(&window->events.request_move.link);
    wl_list_remove(&window->events.request_resize.link);
    wl_list_remove(&window->events.request_minimize.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_fullscreen.link);
    wl_list_remove(&window->events.set_title.link);
    wl_list_remove(&window->events.set_appid.link);
    wl_list_remove(&window->events.destroy.link);

    if (window->scene->node.enabled) { wlr_scene_node_destroy(&window->scene->node); }
    free(window);
}

static void xdg_toplevel_request_move(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_move);
    // struct wlr_xdg_toplevel_move_event *event = data;

    window->server->service->begin_window_interactive(window, WSLAND_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_resize);
    struct wlr_xdg_toplevel_resize_event *event = data;

    window->server->service->begin_window_interactive(window, WSLAND_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_minimize(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_minimize);
    struct wsland_server *server = window->server;

    if (!window->render.resizing && window_check(window)) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        wlr_scene_node_set_enabled(&window->scene->node, !wxt->requested.minimized);
        server->adapter->handle->backend_window_minimize(window, !wxt->requested.minimized);
    }
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_maximize);
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (!window->render.resizing && (output = window->render.wo) != NULL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        if (wxt->requested.maximized) {
            window->metadata.restore = (struct wlr_box){
                .height = window->metadata.geometry.height,
                .width = window->metadata.geometry.width,
                .y = window->scene->node.y,
                .x = window->scene->node.x,
            };

            struct wsland_window *pw;
            struct wlr_box box = output->max_area;
            server->service->scale_box(server, &box, 1 / output->scale.desktop);
            if ((pw = window->parent) != NULL && window_check(pw)) {
                int pos_x, pos_y;
                pw->handle->fetch_offset(pw, &pos_x, &pos_y);
                box.x -= pos_x;
                box.y -= pos_y;
            }

            window->handle->window_resize(window, box);
            server->adapter->handle->backend_window_maximize(window, true);
        } else {
            if (wlr_box_empty(&window->metadata.restore)) {
                window->handle->window_half_output(window);
                window->handle->window_center(window);
            } else {
                window->handle->window_resize(window, window->metadata.restore);
            }

            server->adapter->handle->backend_window_maximize(window, false);
        }

        window->metadata.column = false;
        window->render.resizing =  wlr_xdg_toplevel_set_fullscreen(wxt, false);
        window->render.resizing =  wlr_xdg_toplevel_set_maximized(wxt, wxt->requested.maximized);
    }
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_fullscreen);
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (!window->render.resizing && (output = window->render.wo) != NULL) {
        struct wlr_xdg_toplevel *wxt = window->wayland->toplevel;

        if (wxt->requested.fullscreen) {
            window->metadata.restore = (struct wlr_box){
                .height = window->metadata.geometry.height,
                .width = window->metadata.geometry.width,
                .y = window->scene->node.y,
                .x = window->scene->node.x,
            };

            struct wsland_window *pw;
            struct wlr_box box = output->full_area;
            server->service->scale_box(server, &box, 1 / output->scale.desktop);
            if ((pw = window->parent) != NULL && window_check(pw)) {
                int pos_x, pos_y;
                pw->handle->fetch_offset(pw, &pos_x, &pos_y);
                box.x -= pos_x;
                box.y -= pos_y;
            }

            window->handle->window_resize(window, box);
        } else {
            if (wlr_box_empty(&window->metadata.restore)) {
                window->handle->window_half_output(window);
                window->handle->window_center(window);
            } else {
                window->handle->window_resize(window, window->metadata.restore);
            }
        }

        window->metadata.column = false;
        window->render.resizing = wlr_xdg_toplevel_set_maximized(wxt, false);
        window->render.resizing = wlr_xdg_toplevel_set_fullscreen(wxt, wxt->requested.fullscreen);
    }
}

static void xdg_toplevel_set_appid(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.set_appid);
    struct wsland_server *server = window->server;

    if (server->service->window_appid_reset(window)) {
        server->adapter->handle->backend_window_appid(window);
    }
}

static void xdg_toplevel_set_title(struct wl_listener *listener, void *data) {
    struct wsland_window *window = wl_container_of(listener, window, events.set_title);
    struct wsland_server *server = window->server;

    if (server->service->window_title_reset(window)) {
        server->adapter->handle->backend_window_title(window);
    }
}

static void xdg_popup_unconstrain(struct wsland_window *popup) {
    struct wsland_server *server = popup->server;

    struct wsland_window *window;
    if (window_check(popup) && (window = server->service->find_toplevel_window(&popup->scene->node)) != NULL) {
        struct wlr_box pg = popup->wayland->popup->current.geometry;

        int pos_x, pos_y;
        window->handle->fetch_offset(window, &pos_x, &pos_y);

        struct wsland_output *wo;
        if ((wo = server->service->fetch_output(server, pos_x + pg.x, pos_y + pg.y)) != NULL) {
            struct wlr_box box_space = {
                .x = wo->max_area.x - pos_x, .y = wo->max_area.y - pos_y,
                .width = wo->max_area.width, .height = wo->max_area.height
            };

            server->service->scale_box(server, &box_space, 1 / wo->scale.desktop);
            wlr_xdg_popup_unconstrain_from_box(popup->wayland->popup, &box_space);
            popup->render.resizing = popup->wayland->scheduled_serial;
        }
    }
}

static void xdg_popup_map(struct wl_listener *listener, void *data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.map);
    struct wsland_server *server = popup->server;

    popup->metadata.mapped = true;
    popup->render.wo = popup->handle->fetch_output(popup);
    xdg_popup_unconstrain(popup);

    struct wlr_surface *ws;
    if ((ws = popup->handle->fetch_surface(popup)) != NULL && wl_list_empty(&ws->current.subsurfaces_above) && wl_list_empty(&ws->current.subsurfaces_below)) {
        popup->render.resizing = false;
    }

    server->adapter->handle->backend_window_create(popup);

    wl_list_init(&popup->link);
    wl_list_insert(&server->windows, &popup->link);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.commit);
    struct wsland_server *server = popup->server;

    if (popup->wayland->initial_commit) {
        wlr_xdg_surface_schedule_configure(popup->wayland);
    }

    if (popup->render.resizing && popup->render.resizing <= popup->wayland->current.configure_serial) {
        popup->render.resizing = false;
    }

    if (window_check(popup) && server->config->output.scale) {
        server->service->window_dest_scale(popup);
    }
}

static void xdg_popup_unmap(struct wl_listener *listener, void *data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.unmap);
    struct wsland_server *server = popup->server;

    struct wsland_window *bottom;
    while ((bottom = server->adapter->handle->find_bottom_window(popup, &popup->scene->children)) != NULL) {
        if (popup == bottom) { break; }

        wlr_scene_node_reparent(&bottom->scene->node, server->scene_float);
        bottom->handle->window_shutdown(bottom);
    }

    server->adapter->handle->backend_window_destroy(popup);
    wl_list_remove(&popup->link);

    popup->metadata.mapped = false;
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
    struct wsland_window *popup = wl_container_of(listener, popup, events.destroy);

    wl_list_remove(&popup->events.map.link);
    wl_list_remove(&popup->events.unmap.link);
    wl_list_remove(&popup->events.commit.link);
    wl_list_remove(&popup->events.new_popup.link);
    wl_list_remove(&popup->events.destroy.link);

    if (popup->scene->node.enabled) { wlr_scene_node_destroy(&popup->scene->node); }
    free(popup);
}

static const struct wsland_window_handle wsland_wayland_window_impl = {
    .fetch_title = fetch_title,
    .fetch_appid = fetch_appid,
    .fetch_parent = fetch_parent,
    .fetch_output = fetch_output,
    .fetch_bounds = fetch_bounds,
    .fetch_surface = fetch_surface,
    .fetch_geometry = fetch_geometry,
    .fetch_activate = fetch_activate,
    .fetch_render_position = fetch_render_position,
    .fetch_offset = fetch_offset,

    .window_resize = window_resize,
    .window_column = window_column,
    .window_center = window_center,
    .window_motion = window_motion,
    .window_activate = window_activate,
    .window_minimize = window_minimize,
    .window_maximize = window_maximize,
    .window_shutdown = window_shutdown,
    .window_fullscreen = window_fullscreen,
    .window_surface_position = window_surface_position,
    .window_scene_position = window_scene_position,
    .window_half_output = window_half_output,
    .window_grab_reset = window_grab_reset,
    .window_grab_cannot = window_grab_cannot,
    .window_click_cannot = window_click_cannot,
    .window_have_border = window_have_border,
};

static void window_new_popup(struct wl_listener *listener, void *data) {
    struct wsland_window *parent = wl_container_of(listener, parent, events.new_popup);
    struct wlr_xdg_popup *wxp = data;

    if (!window_check(parent)) {
        return;
    }

    struct wsland_window *popup = calloc(1, sizeof(*popup));
    popup->handle = &wsland_wayland_window_impl;
    popup->server = parent->server;
    popup->wayland = wxp->base;
    popup->parent = parent;

    popup->type = XDG;
    popup->role = POPUP;

    popup->scene = wxp->base->surface->data = wlr_scene_tree_create(parent->scene_surface);
    popup->scene_surface = wlr_scene_xdg_surface_create(popup->scene, popup->wayland);
    popup->scene_border = wlr_scene_buffer_create(popup->scene, NULL);

    popup->scene->node.data = popup->scene_border->node.data = popup->scene_surface->node.data = popup;
    popup->scene_border->point_accepts_input = parent->server->service->window_border_accepts_input;

    pixman_region32_init(&popup->render.damage);
    parent->server->service->window_surface_register(wxp->base->surface, popup);
    LISTEN(&wxp->base->surface->events.map, &popup->events.map, xdg_popup_map);
    LISTEN(&wxp->base->surface->events.unmap, &popup->events.unmap, xdg_popup_unmap);
    LISTEN(&wxp->base->surface->events.commit, &popup->events.commit, xdg_popup_commit);
    LISTEN(&wxp->events.destroy, &popup->events.destroy, xdg_popup_destroy);

    LISTEN(&wxp->base->events.new_popup, &popup->events.new_popup, window_new_popup);
}

static void wayland_new_toplevel(struct wl_listener *listener, void *data) {
    struct wsland_server *server = wl_container_of(listener, server, events.wayland_new_toplevel);
    struct wlr_xdg_toplevel *wxt = data;

    struct wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_wayland_window_impl;
    window->wayland = wxt->base;
    window->server = server;

    window->type = XDG;
    window->role = TOPLEVEL;
    wxt->base->data = window;

    window->scene = wxt->base->surface->data = wlr_scene_tree_create(server->scene_apps);
    window->scene_surface = wlr_scene_xdg_surface_create(window->scene, window->wayland);
    window->scene_border = wlr_scene_buffer_create(window->scene, NULL);

    window->scene->node.data = window->scene_border->node.data = window->scene_surface->node.data = window;
    window->scene_border->point_accepts_input = server->service->window_border_accepts_input;

    pixman_region32_init(&window->render.damage);
    server->service->window_surface_register(wxt->base->surface, window);
    LISTEN(&wxt->base->surface->events.map, &window->events.map, xdg_toplevel_map);
    LISTEN(&wxt->base->surface->events.unmap, &window->events.unmap, xdg_toplevel_unmap);
    LISTEN(&wxt->base->surface->events.commit, &window->events.commit, xdg_toplevel_commit);
    LISTEN(&wxt->events.destroy, &window->events.destroy, xdg_toplevel_destroy);

    LISTEN(&wxt->events.request_move, &window->events.request_move, xdg_toplevel_request_move);
    LISTEN(&wxt->events.request_resize, &window->events.request_resize, xdg_toplevel_request_resize);
    LISTEN(&wxt->events.request_minimize, &window->events.request_minimize, xdg_toplevel_request_minimize);
    LISTEN(&wxt->events.request_maximize, &window->events.request_maximize, xdg_toplevel_request_maximize);
    LISTEN(&wxt->events.request_fullscreen, &window->events.request_fullscreen, xdg_toplevel_request_fullscreen);
    LISTEN(&wxt->events.set_app_id, &window->events.set_appid, xdg_toplevel_set_appid);
    LISTEN(&wxt->events.set_title, &window->events.set_title, xdg_toplevel_set_title);

    LISTEN(&wxt->base->events.new_popup, &window->events.new_popup, window_new_popup);
}

void wayland_event_init(struct wsland_server *server) {
    LISTEN(&server->xdg_shell->events.new_toplevel, &server->events.wayland_new_toplevel, wayland_new_toplevel);
}