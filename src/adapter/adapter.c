// ReSharper disable All
#include <drm/drm_fourcc.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/drm_format_set.h>
#include "wlr/types/wlr_output_layout.h"
#include "wlr/interfaces/wlr_output.h"
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/pixman.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"

void wsland_adapter_activate_for_peer(wsland_peer *peer, uint32_t window_id, bool enabled) {
    wsland_toplevel *toplevel;
    wl_list_for_each(toplevel, &peer->freerdp->adapter->server->toplevels, server_link) {
        wsland_window *window = toplevel->window_data;

        if (window && window->window_id == window_id) {
            if (enabled) {
                wlr_scene_node_raise_to_top(&toplevel->tree->node);
            } else {
                wlr_scene_node_lower_to_bottom(&toplevel->tree->node);
            }
            wlr_xdg_toplevel_set_activated(toplevel->toplevel, enabled);
        }
    }
}

void wsland_adapter_work_area_for_peer(wsland_peer *peer, struct wlr_box area) {
    struct wlr_output *wo = wlr_output_layout_output_at(peer->freerdp->adapter->server->output_layout, area.x, area.y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) {
            output->work_area = area;
        }
    }
}

void wsland_adapter_taskbar_area_for_peer(wsland_peer *peer, struct wlr_box area) {
    struct wlr_output *wo = wlr_output_layout_output_at(peer->freerdp->adapter->server->output_layout, area.x, area.y);
    if (wo) {
        wsland_output *output = wo->data;
        if (output) {
            output->taskbar_area = area;
        }
    }
}

void wsland_adapter_create_keyboard_for_peer(wsland_peer *peer, rdpSettings *settings) {
    wsland_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        wsland_log(ADAPTER, ERROR, "calloc failed for wsland_keyboard");
        return;
    }
    peer->keyboard = keyboard;

    wlr_keyboard_init(&keyboard->keyboard, &wsland_keyboard_impl, wsland_keyboard_impl.name);

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(&keyboard->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(&keyboard->keyboard, 25, 600);

    keyboard->keyboard.base.data = keyboard;
    wl_signal_emit_mutable(&peer->freerdp->adapter->server->backend->events.new_input, &keyboard->keyboard.base);
}

void wsland_adapter_create_output_for_peer(wsland_peer *peer, rdpMonitor *monitor) {
    struct wlr_output_mode mode = {0};
    mode.width = monitor->width;
    mode.height = monitor->height;
    mode.preferred = monitor->is_primary;
    mode.refresh = WSLAND_DEFAULT_REFRESH;

    wsland_output *output = wsland_output_create(peer->freerdp->adapter->server, monitor->width, monitor->height);
    if (!output) {
        wsland_log(ADAPTER, ERROR, "failed to invoke wsland_output_init");
        return;
    }

    output->primary = monitor->is_primary;
    output->monitor = (struct wlr_box){monitor->x, monitor->y, monitor->width, monitor->height};
    wl_list_insert(&peer->outputs, &output->peer_link);
}


wsland_adapter* wsland_adapter_create(wsland_server *server) {
    wsland_adapter *adapter = calloc(1, sizeof(*adapter));
    if (!server) {
        wsland_log(ADAPTER, ERROR, "calloc failed for wsland_adapter");
        return NULL;
    }
    adapter->server = server;

    adapter->handle = wsland_adapter_handle_init(adapter);
    if (!adapter->handle) {
        wsland_log(ADAPTER, ERROR, "failed to invoke wsland_adapter_handle_init");
        goto create_failed;
    }

    {
        adapter->events.wsland_window_motion.notify = adapter->handle->wsland_window_motion;
        wl_signal_add(&server->events.wsland_window_motion, &adapter->events.wsland_window_motion);

        adapter->events.wsland_window_destroy.notify = adapter->handle->wsland_window_destroy;
        wl_signal_add(&server->events.wsland_window_destroy, &adapter->events.wsland_window_destroy);

        adapter->events.wsland_cursor_frame.notify = adapter->handle->wsland_cursor_frame;
        wl_signal_add(&server->events.wsland_cursor_frame, &adapter->events.wsland_cursor_frame);

        adapter->events.wsland_output_frame.notify = adapter->handle->wsland_output_frame;
        wl_signal_add(&server->events.wsland_output_frame, &adapter->events.wsland_output_frame);
    }
    return adapter;

create_failed:
    wsland_adapter_destroy(adapter);
    return NULL;
}

void wsland_adapter_destroy(wsland_adapter *adapter) {
    if (adapter) {
        wl_list_remove(&adapter->events.wsland_window_motion.link);
        wl_list_remove(&adapter->events.wsland_window_destroy.link);

        wl_list_remove(&adapter->events.wsland_cursor_frame.link);
        wl_list_remove(&adapter->events.wsland_output_frame.link);
        free(adapter);
    }
}
