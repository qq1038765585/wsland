#include <assert.h>

#include <wlr/xcursor.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_scene.h>

#include "common/config.h"
#include "common/logger.h"
#include "adapter.h"
#include "server.h"


static void load_default_theme(struct wsland_server *server) {
    wlr_xcursor_manager_load(server->cursor_manager, 1);

    struct wlr_xcursor *xcursor;
    if ((xcursor = wlr_xcursor_manager_get_xcursor(server->cursor_manager, "default", 1)) != NULL) {
        struct wlr_xcursor_image *wxi = xcursor->images[0];
        struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(wxi);
        wlr_xwayland_set_cursor(server->xwayland, buffer, (int)wxi->hotspot_x, (int)wxi->hotspot_y);
    }
}

static bool window_check(struct wsland_window *window) {
    assert(window->type == X11);

    return window->metadata.mapped;
}

static char* fetch_title(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        char *title = window->xwayland->title;

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
        char *appid = window->xwayland->class;

        if (!appid) {
            char ta[32];
            snprintf(ta, sizeof(ta), "%d", window->metadata.ids.window);
            appid = ta;
        }
        return appid;
    }

    return NULL;
}

static struct wsland_window* fetch_parent(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xwayland_surface *wxs = window->xwayland->parent;

        while (wxs) {
            struct wlr_scene_tree *pt;
            if ((pt = wxs->surface->data) != NULL) {
                struct wsland_window *pw = pt->node.data;

                if (pw && pw->role == POPUP && window->role == TOPLEVEL) {
                    wxs = pw->xwayland->parent;
                    continue;
                }

                return pw;
            }
            break;
        }
    }

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
    struct wlr_box bounds = (struct wlr_box) {0};

    if (window_check(window)) {
        struct wlr_surface *surface = window->xwayland->surface;

        if (surface) {
            bounds.height = surface->current.height;
            bounds.width = surface->current.width;
        }
    }

    return bounds;
}

static struct wlr_surface *fetch_surface(struct wsland_window *window) {
    if (window_check(window)) {
        return window->xwayland->surface;
    }

    return NULL;
}

static struct wlr_box fetch_geometry(struct wsland_window *window) {
    if (window_check(window)) {
        return window->handle->fetch_bounds(window);
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
        *dx = sx - window->scene->node.x;
        *dy = sy - window->scene->node.y;
    }
}

static void fetch_offset(struct wsland_window *window, int *offset_x, int *offset_y) {
    if (window_check(window) && window->render.wo) {
        if (window->role == TOPLEVEL) {
            wlr_scene_node_coords(&window->scene->node, offset_x, offset_y);
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
            wlr_scene_node_set_position(&window->scene->node, box.x, box.y);
        }

        window->handle->fetch_offset(window, &box.x, &box.y);
        wlr_xwayland_surface_configure(window->xwayland, (short)box.x, (short)box.y, box.width, box.height);
        window->render.resizing = true;
    }
}

static void window_column(struct wsland_window *window) {
    struct wsland_server *server = window->server;

    struct wsland_output *wo;
    if (window->role == TOPLEVEL && (wo = window->render.wo) != NULL) {
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
        wlr_xwayland_surface_set_fullscreen(window->xwayland, false);
        wlr_xwayland_surface_set_maximized(window->xwayland, false, false);
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

        area.width = window->xwayland->width;
        area.height = window->xwayland->height;
        window->handle->window_resize(window, area);
    }
}

static void window_half_output(struct wsland_window *window) {
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (window->role == TOPLEVEL && (output = window->render.wo) != NULL) {
        xcb_size_hints_t *sh = window->xwayland->size_hints;

        struct wlr_box box = {
            .width = output->max_area.width / 2,
            .height = output->max_area.height / 2
        };

        server->service->scale_box(server, &box, 1 / output->scale.desktop);
        if (sh && sh->min_width  == sh->max_width && sh->min_height == sh->max_height) {
            box.height = window->xwayland->height;
            box.width = window->xwayland->width;
        }

        window->handle->window_resize(window, box);

        window->metadata.column = false;
        wlr_xwayland_surface_set_fullscreen(window->xwayland, false);
        wlr_xwayland_surface_set_maximized(window->xwayland, false, false);
    }
}

static void window_motion(struct wsland_window *window,  int pos_x, int pos_y) {
    if (window_check(window)) {
        wlr_scene_node_set_position(&window->scene->node, pos_x, pos_y);
    }
}

static void window_activate(struct wsland_window *window, bool enabled) {
    if (window_check(window)) {
        struct wsland_adapter *adapter = window->server->adapter;
        if (!enabled) {
            wlr_xwayland_surface_activate(window->xwayland, enabled);
        } else if (window->xwayland->minimized) {
            wlr_xwayland_surface_set_minimized(window->xwayland, false);
        }
        window = adapter->handle->find_bottom_window(window, &window->scene->children);
        wlr_xwayland_surface_activate(window->xwayland, enabled);

        if (!enabled) {
            wlr_seat_keyboard_notify_clear_focus(window->server->seat);

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
        struct wsland_adapter *adapter = window->server->adapter;

        if (window->handle->fetch_surface(window)->mapped && !window->render.resizing) {
            wlr_xwayland_surface_set_minimized(window->xwayland, true);

            wlr_scene_node_set_enabled(&window->scene->node, false);
            adapter->handle->backend_window_minimize(window, false);
        }
    }
}

static void window_maximize(struct wsland_window *window) {
    if (window_check(window)) {
        wlr_xwayland_surface_set_maximized(window->xwayland, !window->xwayland->maximized_horz, !window->xwayland->maximized_vert);
        wl_signal_emit(&window->xwayland->events.request_maximize, window->xwayland);
    }
}

static void window_fullscreen(struct wsland_window *window) {
    if (window_check(window)) {
        wlr_xwayland_surface_set_fullscreen(window->xwayland, !window->xwayland->fullscreen);
        wl_signal_emit(&window->xwayland->events.request_fullscreen, window->xwayland);
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
        wlr_xwayland_surface_close(window->xwayland);
    }
}

static void window_grab_reset(struct wsland_window *window) {
    if (window->server->grab.mode == WSLAND_CURSOR_MOVE) {
        window->handle->window_resize(window, (struct wlr_box) {
            .x = window->scene->node.x, .y = window->scene->node.y,
            .width = window->xwayland->width, .height = window->xwayland->height
        });
    }
}

static bool window_grab_cannot(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xwayland_surface *wxs = window->xwayland;

        return window->metadata.column || wxs->fullscreen || (wxs->maximized_horz && wxs->maximized_vert);
    }

    return true;
}

static bool window_click_cannot(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xwayland_surface *wxs = window->xwayland;

        return window->metadata.column || wxs->fullscreen || (wxs->maximized_horz && wxs->maximized_vert);
    }

    return true;
}

static bool window_have_border(struct wsland_window *window) {
    if (window_check(window) && window->role == TOPLEVEL) {
        struct wlr_xwayland_surface *wxs = window->xwayland;

        return !wxs->fullscreen && !wxs->maximized_vert && !wxs->maximized_horz;
    }

    return false;
}

static bool window_have_above(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xwayland_surface *wxs = window->xwayland;

        if (window->parent) {
            return false;
        }

        if (wxs->modal) {
            return true;
        }

        if (wlr_xwayland_surface_has_window_type(wxs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_DIALOG)
            || wlr_xwayland_surface_has_window_type(wxs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_SPLASH)
            || wlr_xwayland_surface_has_window_type(wxs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_TOOLBAR)
            || wlr_xwayland_surface_has_window_type(wxs, WLR_XWAYLAND_NET_WM_WINDOW_TYPE_UTILITY)) {
            return true;
        }

        //xcb_size_hints_t *sh = wxs->size_hints;
        //return sh && sh->min_width > 0 && sh->min_height > 0 && sh->max_width == sh->min_width && sh->max_height == sh->min_height;
        char *title;
        if ((title = window->handle->fetch_title(window)) != NULL && strcmp("splash", title) == 0) {
            return true;
        }
    }

    return false;
}

static void unmanaged_unconstrain(struct wsland_window *uw, bool configure) {
    struct wsland_server *server = uw->server;

    struct wsland_window *pw;
    if (window_check(uw) && (pw = uw->parent) != NULL) {
        struct wlr_box box = {
            .x = uw->xwayland->x - pw->xwayland->x,
            .y = uw->xwayland->y - pw->xwayland->y,
            .height = uw->xwayland->height,
            .width = uw->xwayland->width,
        };

        struct wsland_output *wo;
        if ((wo = uw->render.wo) != NULL) {
            server->service->scale_box(server, &box, wo->scale.desktop);

            int px = pw->xwayland->x + box.x;
            int py = pw->xwayland->y + box.y;
            struct wlr_box max_area = wo->max_area;
            if (px + box.width > max_area.x + max_area.width) {
                px = max_area.x + max_area.width - box.width - 8;
            }
            if (py + box.height > max_area.y + max_area.height) {
                py = max_area.y + max_area.height - box.height - 8;
            }
            if (px < max_area.x) {
                px = max_area.x + 8;
            }
            if (py < max_area.y) {
                py = max_area.y + 8;
            }

            box.x = px - pw->xwayland->x;
            box.y = py - pw->xwayland->y;
        }

        uw->handle->window_motion(uw, box.x, box.y);

        if (configure) {
            uw->handle->fetch_offset(uw, &box.x, &box.y);
            wlr_xwayland_surface_configure(uw->xwayland, (short)box.x, (short)box.y, uw->xwayland->width, uw->xwayland->height);
        }
    }
}

static void unmanaged_map(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.map);
    struct wsland_server *server = uw->server;

    uw->metadata.mapped = true;
    uw->render.wo = server->service->fetch_output(server, uw->xwayland->x, uw->xwayland->y);

    if (!uw->parent) {
        uw->parent = uw->handle->fetch_parent(uw);
        if (!uw->parent) {
            struct wsland_window *pw, *tmp;
            wl_list_for_each_safe(pw, tmp, &uw->server->windows, link) {
                if (pw->type == X11 && pw->role == TOPLEVEL && pw->xwayland->pid == uw->xwayland->pid) {
                    uw->parent = pw;
                    break;
                }
            }
        }
    }

    struct wsland_window *pw;
    if ((pw = uw->parent) != NULL && window_check(pw)) {
        wlr_scene_node_reparent(&uw->scene->node, pw->scene_surface);

        unmanaged_unconstrain(uw, true);
        server->adapter->handle->backend_window_create(uw);

        wl_list_init(&uw->link);
        wl_list_insert(&server->windows, &uw->link);
        return;
    }

    uw->metadata.mapped = false;
    server_log(ERROR, "unmanaged map error: no parent found!");
}

static void unmanaged_commit(struct wl_listener *listener, void *user_data) {
    struct wsland_window *unmanaged = wl_container_of(listener, unmanaged, events.commit);
    struct wsland_server *server = unmanaged->server;

    if (window_check(unmanaged) && server->config->output.scale) {
        server->service->window_dest_scale(unmanaged);
    }
}

static void unmanaged_unmap(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.unmap);
    struct wsland_server *server = uw->server;

    struct wsland_window *bottom;
    while ((bottom = server->adapter->handle->find_bottom_window(uw, &uw->scene->children)) != NULL) {
        if (uw == bottom) { break; }

        wlr_scene_node_reparent(&bottom->scene->node, server->scene_float);
        bottom->handle->window_shutdown(bottom);
    }

    if (uw->metadata.mapped) {
        server->adapter->handle->backend_window_destroy(uw);
        wl_list_remove(&uw->link);
    }

    uw->metadata.mapped = false;
}

static void unmanaged_associate(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.associate);
    struct wlr_surface *ws = uw->xwayland->surface;

    ws->data = uw->scene;
    uw->scene_surface = wlr_scene_subsurface_tree_create(uw->scene, ws);
    wlr_scene_node_place_above(&uw->scene_border->node, &uw->scene_surface->node);
    uw->scene_surface->node.data = uw;

    pixman_region32_init(&uw->render.damage);
    uw->server->service->window_surface_register(ws, uw);

    LISTEN(&ws->events.map, &uw->events.map, unmanaged_map);
    LISTEN(&ws->events.unmap, &uw->events.unmap, unmanaged_unmap);
    LISTEN(&ws->events.commit, &uw->events.commit, unmanaged_commit);
}

static void unmanaged_dissociate(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.dissociate);

    if (uw->metadata.mapped) {
        wl_signal_emit(&uw->xwayland->surface->events.unmap, uw->xwayland->surface);
    }

    wl_list_remove(&uw->events.map.link);
    wl_list_remove(&uw->events.unmap.link);
    wl_list_remove(&uw->events.commit.link);
}

static void unmanaged_set_geometry(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.set_geometry);

    unmanaged_unconstrain(uw, false);
}

static void unmanaged_request_configure(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = user_data;

    if (!event->surface->surface || !event->surface->surface->mapped) {
        wlr_xwayland_surface_configure(event->surface, event->x, event->y, event->width, event->height);
    }
}

static void unmanaged_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_window *uw = wl_container_of(listener, uw, events.destroy);

    if (uw->metadata.mapped) {
        wl_signal_emit(&uw->xwayland->events.dissociate, uw);
    }

    wl_list_remove(&uw->events.request_configure.link);
    wl_list_remove(&uw->events.set_geometry.link);
    wl_list_remove(&uw->events.dissociate.link);
    wl_list_remove(&uw->events.associate.link);
    wl_list_remove(&uw->events.destroy.link);

    if (uw->scene->node.enabled) { wlr_scene_node_destroy(&uw->scene->node); }
    free(uw);
}

static void xwayland_map(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.map);
    struct wsland_server *server = window->server;

    window->metadata.mapped = true;
    window->render.wo = window->handle->fetch_output(window);

    if (server->service->window_parent_reset(window) && window->parent) {
        wlr_scene_node_reparent(&window->scene->node, window->parent->scene);
    } else {
        window_half_output(window);
        window->metadata.icon = !window->metadata.above;
    }

    window->metadata.above = window->handle->window_have_above(window);
    if (window->metadata.above) {
        wlr_scene_node_reparent(&window->scene->node, server->scene_float);
    }

    window->render.center = true;
    server->adapter->handle->backend_window_create(window);

    wl_list_init(&window->link);
    wl_list_insert(&server->windows, &window->link);
    server->service->dispatch_window_focus(window);
}

static bool window_resize_finish(struct wsland_window *window) {
    if (window_check(window)) {
        struct wlr_xwayland_surface *wxs = window->xwayland;

        struct wlr_box geometry = window->handle->fetch_geometry(window);
        return geometry.width == wxs->width && geometry.height == wxs->height;
    }

    return true;
}

static void xwayland_commit(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.commit);
    struct wsland_server *server = window->server;

    if (window->render.resizing && window_resize_finish(window)) {
        window->render.resizing = false;
    }

    if (window_check(window) && server->config->output.scale) {
        server->service->window_dest_scale(window);
    }
}

static void xwayland_unmap(struct wl_listener *listener, void *user_data) {
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

static void xwayland_associate(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.associate);
    struct wlr_surface *ws = window->xwayland->surface;

    ws->data = window->scene;
    window->scene_surface = wlr_scene_subsurface_tree_create(window->scene, ws);
    wlr_scene_node_place_above(&window->scene_border->node, &window->scene_surface->node);
    window->scene_surface->node.data = window;

    pixman_region32_init(&window->render.damage);
    window->server->service->window_surface_register(ws, window);
    wlr_xwayland_surface_set_fullscreen(window->xwayland, false);
    wlr_xwayland_surface_set_maximized(window->xwayland, false, false);

    LISTEN(&ws->events.map, &window->events.map, xwayland_map);
    LISTEN(&ws->events.unmap, &window->events.unmap, xwayland_unmap);
    LISTEN(&ws->events.commit, &window->events.commit, xwayland_commit);
}

static void xwayland_set_hints(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.set_hints);

    if (!window->xwayland->hints) {
        return;
    }

    xcb_icccm_wm_hints_get_urgency(window->xwayland->hints);
}

static void xwayland_set_appid(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.set_appid);
    struct wsland_server *server = window->server;

    if (server->service->window_appid_reset(window)) {
        server->adapter->handle->backend_window_appid(window);
    }
}

static void xwayland_set_title(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.set_title);
    struct wsland_server *server = window->server;

    if (server->service->window_title_reset(window)) {
        server->adapter->handle->backend_window_title(window);
    }
}

static void xwayland_dissociate(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.dissociate);

    if (window->metadata.mapped) {
        wl_signal_emit(&window->xwayland->surface->events.unmap, window);
    }

    wl_list_remove(&window->events.map.link);
    wl_list_remove(&window->events.unmap.link);
    wl_list_remove(&window->events.commit.link);
}

static void xwayland_request_configure(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_configure);
    struct wlr_xwayland_surface_configure_event *event = user_data;

    if (!event->surface->surface || !event->surface->surface->mapped) {
        wlr_xwayland_surface_configure(event->surface, event->x, event->y, event->width, event->height);
    }
}

static void xwayland_request_activate(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_activate);

    window->server->service->dispatch_window_focus(window);
}

static void xwayland_request_fullscreen(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_fullscreen);
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (!window->render.resizing && (output = window->render.wo) != NULL) {
        if (window->xwayland->fullscreen) {
            window->metadata.restore = (struct wlr_box) {
                .x = window->scene->node.x,
                .y = window->scene->node.y,
                .width = window->xwayland->width,
                .height = window->xwayland->height,
            };

            struct wlr_box box = output->full_area;
            server->service->scale_box(server, &box, 1 / output->scale.desktop);

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
        wlr_xwayland_surface_set_maximized(window->xwayland, false, false);
    }
}

static void xwayland_request_maximize(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.request_maximize);
    struct wsland_server *server = window->server;

    struct wsland_output *output;
    if (!window->render.resizing && (output = window->render.wo) != NULL) {
        struct wsland_adapter *adapter = window->server->adapter;

        if (window->xwayland->maximized_horz && window->xwayland->maximized_vert) {
            window->metadata.restore = (struct wlr_box) {
                .x = window->scene->node.x,
                .y = window->scene->node.y,
                .width = window->xwayland->width,
                .height = window->xwayland->height,
            };

            struct wlr_box box = output->max_area;
            server->service->scale_box(server, &box, 1 / output->scale.desktop);

            window->handle->window_resize(window, box);
            adapter->handle->backend_window_maximize(window, true);
        } else {
            if (wlr_box_empty(&window->metadata.restore)) {
                window->handle->window_half_output(window);
                window->handle->window_center(window);
            } else {
                window->handle->window_resize(window, window->metadata.restore);
            }

            adapter->handle->backend_window_maximize(window, false);
        }

        window->metadata.column = false;
        wlr_xwayland_surface_set_fullscreen(window->xwayland, false);
    }
}

static void xwayland_destroy(struct wl_listener *listener, void *user_data) {
    struct wsland_window *window = wl_container_of(listener, window, events.destroy);

    if (window->metadata.mapped) {
        wl_signal_emit(&window->xwayland->events.dissociate, window);
    }

    wl_list_remove(&window->events.request_fullscreen.link);
    wl_list_remove(&window->events.request_configure.link);
    wl_list_remove(&window->events.request_maximize.link);
    wl_list_remove(&window->events.request_activate.link);
    wl_list_remove(&window->events.dissociate.link);
    wl_list_remove(&window->events.associate.link);
    wl_list_remove(&window->events.set_appid.link);
    wl_list_remove(&window->events.set_hints.link);
    wl_list_remove(&window->events.set_title.link);
    wl_list_remove(&window->events.destroy.link);

    if (window->scene->node.enabled) { wlr_scene_node_destroy(&window->scene->node); }
    free(window);
}

static struct wsland_window_handle wsland_xwayland_window_impl = {
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
    .window_have_above = window_have_above,
};

static struct wsland_window *create_unmanaged(struct wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    struct wsland_window *unmanaged = calloc(1, sizeof(*unmanaged));
    unmanaged->handle = &wsland_xwayland_window_impl;
    unmanaged->xwayland = xwayland_surface;
    unmanaged->server = server;

    unmanaged->type = X11;
    unmanaged->role = POPUP;

    unmanaged->scene = wlr_scene_tree_create(unmanaged->server->scene_apps);
    unmanaged->scene_border = wlr_scene_buffer_create(unmanaged->scene, NULL);

    unmanaged->scene->node.data = unmanaged->scene_border->node.data = unmanaged;
    unmanaged->scene_border->point_accepts_input = unmanaged->server->service->window_border_accepts_input;

    LISTEN(&xwayland_surface->events.set_geometry, &unmanaged->events.set_geometry, unmanaged_set_geometry);
    LISTEN(&xwayland_surface->events.request_configure, &unmanaged->events.request_configure, unmanaged_request_configure);
    LISTEN(&xwayland_surface->events.dissociate, &unmanaged->events.dissociate, unmanaged_dissociate);
    LISTEN(&xwayland_surface->events.associate, &unmanaged->events.associate, unmanaged_associate);
    LISTEN(&xwayland_surface->events.destroy, &unmanaged->events.destroy, unmanaged_destroy);
    return unmanaged;
}

static struct wsland_window *create_xwayland_window(struct wsland_server *server, struct wlr_xwayland_surface *xwayland_surface) {
    struct wsland_window *window = calloc(1, sizeof(*window));
    window->handle = &wsland_xwayland_window_impl;
    window->xwayland = xwayland_surface;
    window->server = server;

    window->type = X11;
    window->role = TOPLEVEL;

    window->scene = wlr_scene_tree_create(window->server->scene_apps);
    window->scene_border = wlr_scene_buffer_create(window->scene, NULL);

    window->scene->node.data = window->scene_border->node.data = window;
    window->scene_border->point_accepts_input = window->server->service->window_border_accepts_input;

    LISTEN(&xwayland_surface->events.request_configure, &window->events.request_configure, xwayland_request_configure);
    LISTEN(&xwayland_surface->events.request_fullscreen, &window->events.request_fullscreen, xwayland_request_fullscreen);
    LISTEN(&xwayland_surface->events.request_maximize, &window->events.request_maximize, xwayland_request_maximize);
    LISTEN(&xwayland_surface->events.request_activate, &window->events.request_activate, xwayland_request_activate);
    LISTEN(&xwayland_surface->events.dissociate, &window->events.dissociate, xwayland_dissociate);
    LISTEN(&xwayland_surface->events.associate, &window->events.associate, xwayland_associate);
    LISTEN(&xwayland_surface->events.set_hints, &window->events.set_hints, xwayland_set_hints);
    LISTEN(&xwayland_surface->events.set_class, &window->events.set_appid, xwayland_set_appid);
    LISTEN(&xwayland_surface->events.set_title, &window->events.set_title, xwayland_set_title);
    LISTEN(&xwayland_surface->events.destroy, &window->events.destroy, xwayland_destroy);
    return window;
}

static void xwayland_ready(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.xwayland_ready);

    wlr_xwayland_set_seat(server->xwayland, server->seat);
    load_default_theme(server);
}

static void xwayland_new_toplevel(struct wl_listener *listener, void *user_data) {
    struct wsland_server *server = wl_container_of(listener, server, events.xwayland_new_toplevel);
    struct wlr_xwayland_surface *xwayland_surface = user_data;

    if (xwayland_surface->override_redirect) {
        create_unmanaged(server, xwayland_surface);
        return;
    }
    create_xwayland_window(server, xwayland_surface);
}

void xwayland_event_init(struct wsland_server *server) {
    LISTEN(&server->xwayland->events.ready, &server->events.xwayland_ready, xwayland_ready);
    LISTEN(&server->xwayland->events.new_surface, &server->events.xwayland_new_toplevel, xwayland_new_toplevel);
}