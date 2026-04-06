// ReSharper disable All
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"
#include "wsland/utils/time.h"

static wsland_output* wsland_output_from_point(wsland_peer *peer, int x, int y) {
    wsland_output *output;
    wl_list_for_each(output, &peer->outputs, peer_link) {
        if (wlr_box_contains_point(&output->monitor, x, y)) {
            return output;
        }
    }
    return NULL;
}

static bool accumulate_axis(wsland_peer *peer, UINT16 flags, enum wl_pointer_axis axis, double *delta, int32_t *delta_discrete) {
    INT8 rdp_delta = (INT8)(flags & 0xFF);
    int wheel_delta = -rdp_delta / WLR_POINTER_AXIS_DISCRETE_STEP;

    *delta = wheel_delta * 15;
    *delta_discrete = wheel_delta * WLR_POINTER_AXIS_DISCRETE_STEP;
    return true;
}

static void rail_sync_window_state(wsland_peer *peer) {
    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVESECURE,
            .setScreenSaveSecure = 0,
        };
        peer->ctx_server_rail->ServerSysparam(peer->ctx_server_rail, &sysParamOrder);
        peer->peer->DrainOutputBuffer(peer->peer);
    }

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVEACTIVE,
            .setScreenSaveActive = 0,
        };
        peer->ctx_server_rail->ServerSysparam(peer->ctx_server_rail, &sysParamOrder);
        peer->peer->DrainOutputBuffer(peer->peer);
    }

    {
        RAIL_ZORDER_SYNC zOrderSync = {
            .windowIdMarker = RAIL_MARKER_WINDOW_ID,
        };
        peer->ctx_server_rail->ServerZOrderSync(peer->ctx_server_rail, &zOrderSync);
        peer->peer->DrainOutputBuffer(peer->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED | WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->peer->update->window->MonitoredDesktop(
            peer->peer->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->peer->DrainOutputBuffer(peer->peer);
    }

    {
        uint32_t windowsIdArray[1] = {0};
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
        };
        windowsIdArray[0] = RAIL_MARKER_WINDOW_ID;

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {
            .activeWindowId = RAIL_DESKTOP_WINDOW_ID,
            .numWindowIds = 1, .windowIds = (UINT *)&windowsIdArray,
        };

        peer->peer->update->window->MonitoredDesktop(
            peer->peer->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->peer->DrainOutputBuffer(peer->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->peer->update->window->MonitoredDesktop(
            peer->peer->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->peer->DrainOutputBuffer(peer->peer);
    }
}

static void rail_peer_destroy(wsland_peer *peer) {
    if (peer->ctx_server_drdynvc) {
        peer->ctx_server_drdynvc->Stop(peer->ctx_server_drdynvc);

        drdynvc_server_context_free(peer->ctx_server_drdynvc);
        peer->ctx_server_drdynvc = NULL;
    }
    if (peer->ctx_server_rail) {
        peer->ctx_server_rail->Stop(peer->ctx_server_rail);

        rail_server_context_free(peer->ctx_server_rail);
        peer->ctx_server_rail = NULL;
    }
    if (peer->ctx_server_disp) {
        peer->ctx_server_disp->Close(peer->ctx_server_disp);

        disp_server_context_free(peer->ctx_server_disp);
        peer->ctx_server_disp = NULL;
    }
    if (peer->ctx_server_rdpgfx) {
        peer->ctx_server_rdpgfx->Close(peer->ctx_server_rdpgfx);

        rdpgfx_server_context_free(peer->ctx_server_rdpgfx);
        peer->activation_graphics_completed = false;
        peer->ctx_server_rdpgfx = NULL;
    }
    if (peer->ctx_server_gfxredir) {
        peer->ctx_server_gfxredir->Close(peer->ctx_server_gfxredir);

        gfxredir_server_context_free(peer->ctx_server_gfxredir);
        peer->activation_graphics_redirection_completed = false;
        peer->ctx_server_gfxredir = NULL;
    }
    if (peer->ctx_server_applist) {
        peer->ctx_server_applist->Close(peer->ctx_server_applist);

        rdpapplist_server_context_free(peer->ctx_server_applist);
        peer->ctx_server_applist = NULL;
    }
}

static bool rail_peer_init(wsland_peer *peer) {
    peer->peer->settings->DesktopResize = FALSE; /* Server must not ask client to resize */

    if (!ctx_drdynvc_init(peer)) {
        goto failed;
    }

    if (!ctx_rail_init(peer)) {
        goto failed;
    }

    if (!ctx_disp_init(peer)) {
        goto failed;
    }

    if (!ctx_rdpgfx_init(peer)) {
        goto failed;
    }

    if (peer->freerdp->use_gfxredir) {
        if (!ctx_gfxredir_init(peer)) {
            goto failed;
        }
    }

    if (!ctx_applist_init(peer)) {
        goto failed;
    }

    peer->current_frame_id = 0;
    peer->acknowledged_frame_id = 0;
    return true;

failed:
    rail_peer_destroy(peer);
    return false;
}

static BOOL xf_peer_adjust_monitor_layout(freerdp_peer *rdp_peer) {
    wsland_log(FREERDP, INFO, "invoke ==> xf_peer_adjust_monitor_layout");
    return TRUE;
}

static BOOL xf_peer_capabilities(freerdp_peer *rdp_peer) {
    wsland_log(FREERDP, INFO, "invoke ==> xf_peer_capabilities");
    return TRUE;
}

static BOOL xf_peer_post_connect(freerdp_peer *rdp_peer) {
    wsland_log(FREERDP, INFO, "invoke ==> xf_peer_post_connect");
    return TRUE;
}

static BOOL xf_peer_activate(freerdp_peer *rdp_peer) {
    wsland_log(FREERDP, INFO, "invoke ==> xf_peer_activate");

    wsland_peer *peer = (wsland_peer*)rdp_peer->context;
    rdpSettings *settings = rdp_peer->settings;

    if (!settings->SurfaceCommandsEnabled) {
        wlr_log(WLR_ERROR, "freerdp client does not support SurfaceCommands");
        return FALSE;
    }

    if (!settings->RemoteApplicationMode) {
        wlr_log(WLR_ERROR, "freerdp client does not support RemoteApplicationMode");
        return FALSE;
    }

    if (!rail_peer_init(peer)) {
        return FALSE;
    }

    rail_sync_window_state(peer);
    if (peer->flags & WSLAND_PEER_ACTIVATED) {
        return TRUE;
    }

    {
        if (!settings->MonitorCount || settings->MonitorCount > RDP_MAX_MONITOR) {
            settings->MonitorCount = 1;
            settings->MonitorDefArray[0].is_primary = true;
            settings->MonitorDefArray[0].x = 0;
            settings->MonitorDefArray[0].y = 0;
            settings->MonitorDefArray[0].width = (int)settings->DesktopWidth;
            settings->MonitorDefArray[0].height = (int)settings->DesktopHeight;
            settings->MonitorDefArray[0].attributes.orientation = settings->DesktopOrientation;
            settings->MonitorDefArray[0].attributes.physicalWidth = settings->DesktopPhysicalWidth;
            settings->MonitorDefArray[0].attributes.physicalHeight = settings->DesktopPhysicalHeight;
            settings->MonitorDefArray[0].attributes.desktopScaleFactor = settings->DesktopScaleFactor;
            settings->MonitorDefArray[0].attributes.deviceScaleFactor = settings->DeviceScaleFactor;
        }

        for (uint32_t index = 0; index < settings->MonitorCount; ++index) {
            rdpMonitor monitor = settings->MonitorDefArray[index];

            wsland_adapter_create_output_for_peer(peer, &monitor);
        }
        wsland_adapter_create_keyboard_for_peer(peer, settings);
        peer->flags |= WSLAND_PEER_OUTPUT_ENABLED;
    }

    if (!rail_clipboard_init(peer)) {
        return FALSE;
    }

    peer->flags |= WSLAND_PEER_ACTIVATED;
    return TRUE;
}

static BOOL xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area) {
    return TRUE;
}

static BOOL xf_input_synchronize_event(rdpInput *input, UINT32 flags) {
    wsland_peer *peer = (wsland_peer*)input->context;
    wsland_keyboard *keyboard = peer->keyboard;

    /*struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int idx = 0; idx < MAX_FREERDP_KEYS; idx++) {
        if (peer->pressed[idx]) {
            peer->pressed[idx] = false;

            struct wlr_keyboard_key_event event = {0};
            event.time_msec = timespec_to_msec(&now);
            event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
            event.update_state = true;
            event.keycode = idx;
            wlr_keyboard_notify_key(&keyboard->keyboard, &event);
        }
    }*/

    return TRUE;
}

static BOOL xf_input_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    wsland_peer *peer = (wsland_peer*)input->context;
    wsland_output *output = wsland_output_from_point(peer, x, y);

    if (!(peer->flags & WSLAND_PEER_OUTPUT_ENABLED) || !output) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool frame = false;
    if (flags & PTR_FLAGS_MOVE) {
        struct wlr_pointer_motion_absolute_event event = {0};
        event.pointer = &output->pointer;
        event.time_msec = timespec_to_msec(&now);
        event.x = x;
        event.y = y;
        wl_signal_emit_mutable(&output->pointer.events.motion_absolute, &event);
        frame = true;
    }

    uint32_t button = 0;
    if (flags & PTR_FLAGS_BUTTON1) {
        button = BTN_LEFT;
    }
    else if (flags & PTR_FLAGS_BUTTON2) {
        button = BTN_RIGHT;
    }
    else if (flags & PTR_FLAGS_BUTTON3) {
        button = BTN_MIDDLE;
    }

    if (button) {
        struct wlr_pointer_button_event event = {0};
        event.pointer = &output->pointer;
        event.time_msec = timespec_to_msec(&now);
        event.button = button;
        event.state = (flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        wl_signal_emit_mutable(&output->pointer.events.button, &event);
        frame = true;
    }

    double delta;
    int32_t delta_discrete;
    enum wl_pointer_axis axis;
    if (flags & PTR_FLAGS_WHEEL) {
        axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        if (accumulate_axis(peer, flags, axis, &delta, &delta_discrete)) {
            frame = true;

            struct wlr_pointer_axis_event event = {0};
            event.pointer = &output->pointer;
            event.time_msec = timespec_to_msec(&now);
            event.source = WL_POINTER_AXIS_SOURCE_WHEEL;
            event.delta_discrete = delta_discrete;
            event.orientation = axis;
            event.delta = delta;
            wl_signal_emit_mutable(&output->pointer.events.axis, &event);
        }
    } else if (flags & PTR_FLAGS_HWHEEL) {
        axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        if (accumulate_axis(peer, flags, axis, &delta, &delta_discrete)) {
            frame = true;

            struct wlr_pointer_axis_event event = {0};
            event.pointer = &output->pointer;
            event.time_msec = timespec_to_msec(&now);
            event.source = WL_POINTER_AXIS_SOURCE_WHEEL;
            event.orientation = axis;
            event.delta_discrete = delta_discrete;
            event.delta = delta;
            wl_signal_emit_mutable(&output->pointer.events.axis, &event);
        }
    }

    if (frame) {
        wl_signal_emit_mutable(&output->pointer.events.frame, &output->pointer);
    }

    return TRUE;
}

static BOOL xf_input_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    return TRUE;
}

static BOOL xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code) {
    wsland_peer *peer = (wsland_peer*)input->context;
    wsland_keyboard *keyboard = peer->keyboard;

    if (!(peer->flags & WSLAND_PEER_OUTPUT_ENABLED)) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    bool is_release = flags & KBD_FLAGS_RELEASE;
    bool is_extended = flags & KBD_FLAGS_EXTENDED;
    enum wl_keyboard_key_state state = is_release ? WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED;

    {
        uint32_t full_code = code;
        if (is_extended) {
            full_code |= KBD_FLAGS_EXTENDED;
        }

        uint32_t vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
        if (is_extended) {
            vk_code |= KBDEXT;
        }

        bool notify = false;
        uint32_t scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);
        uint32_t key_code = scan_code;

        if (is_release) {
            if (peer->pressed[key_code]) {
                peer->pressed[key_code] = false;
                notify = true;
            }
        } else {
            if (!peer->pressed[key_code]) {
                peer->pressed[key_code] = true;
                notify = true;
            }
        }

        if (notify) {
            struct wlr_keyboard_key_event event = {0};
            event.time_msec = timespec_to_msec(&now);
            event.keycode = key_code - 8;
            event.state = state;
            event.update_state = true;
            wlr_keyboard_notify_key(&keyboard->keyboard, &event);
        }
    }

    return TRUE;
}

static BOOL xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code) {
    return TRUE;
}

static void rail_client_activate(bool free_only, void *user_data) {
    dispatch_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        wsland_server *server = data->peer->freerdp->adapter->server;

        wsland_window *window;
        wl_list_for_each(window, &server->windows, server_link) {
            if (window && window->window_id == data->activate.windowId) {
                if (data->activate.enabled) {
                    window->server->handle->dispatch_window_focus(window);
                } else {
                    window->handle->window_activate(window, data->activate.enabled);
                }
            }
        }
    }

    free(data);
}

static void rail_client_sysparam(bool free_only, void *user_data) {
    dispatch_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        if (data->sysparam.params & SPI_MASK_SET_WORK_AREA) {
            wsland_adapter_work_area_for_peer(data->peer, (struct wlr_box){
                .x = data->sysparam.workArea.left,
                .y = data->sysparam.workArea.top,
                .width = data->sysparam.workArea.right - data->sysparam.workArea.left,
                .height = data->sysparam.workArea.bottom - data->sysparam.workArea.top
            });
        }

        if (data->sysparam.params & SPI_MASK_TASKBAR_POS) {
            wsland_adapter_taskbar_area_for_peer(data->peer, (struct wlr_box){
                .x = data->sysparam.workArea.left,
                .y = data->sysparam.workArea.top,
                .width = data->sysparam.workArea.right - data->sysparam.workArea.left,
                .height = data->sysparam.workArea.bottom - data->sysparam.workArea.top
            });
        }
    }

    free(data);
}

static void rdpgfx_frame_acknowledge(bool free_only, void *user_data) {
    dispatch_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        wsland_adapter_frame_for_peer(data->peer, data->frame_acknowledge);
    }

    free(data);
}

static void gfxredir_frame_acknowledge(wsland_peer *peer, uint64_t window_id) {
    wsland_window *window;
    wl_list_for_each(window, &peer->freerdp->adapter->server->windows, server_link) {
        if (window->window_id == window_id) {
            window->update_pending = false;
            break;
        }
    }
}

wsland_peer_handle wsland_peer_handle_impl = {
    .xf_peer_adjust_monitor_layout = xf_peer_adjust_monitor_layout,
    .xf_peer_capabilities = xf_peer_capabilities,
    .xf_peer_post_connect = xf_peer_post_connect,
    .xf_peer_activate = xf_peer_activate,

    .xf_suppress_output = xf_suppress_output,
    .xf_input_synchronize_event = xf_input_synchronize_event,
    .xf_input_mouse_event = xf_input_mouse_event,
    .xf_input_extended_mouse_event = xf_input_extended_mouse_event,
    .xf_input_keyboard_event = xf_input_keyboard_event,
    .xf_input_unicode_keyboard_event = xf_input_unicode_keyboard_event,

    .rail_client_activate = rail_client_activate,
    .rail_client_sysparam = rail_client_sysparam,
    .rdpgfx_frame_acknowledge = rdpgfx_frame_acknowledge,
    .gfxredir_frame_acknowledge = gfxredir_frame_acknowledge,
    .rail_peer_destroy = rail_peer_destroy,
};

void wsland_peer_handle_init(wsland_peer *peer) {
    peer->handle = &wsland_peer_handle_impl;
}