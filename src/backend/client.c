#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <linux/input-event-codes.h>
#include <freerdp/channels/drdynvc.h>

#include "common/logger.h"
#include "common/time.h"
#include "backend.h"
#include "adapter.h"
#include "server.h"

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);

static void backend_client_channel_destroy(struct backend_client *client) {
    if (client->rail_ctx) {
        client->rail_ctx->Stop(client->rail_ctx);

        rail_server_context_free(client->rail_ctx);
        client->rail_ctx = NULL;
    }
    if (client->disp_ctx) {
        client->disp_ctx->Close(client->disp_ctx);

        disp_server_context_free(client->disp_ctx);
        client->disp_ctx = NULL;
    }
    if (client->rdpgfx_ctx) {
        client->rdpgfx_ctx->Close(client->rdpgfx_ctx);

        rdpgfx_server_context_free(client->rdpgfx_ctx);
        client->activation_graphics_completed = false;
        client->rdpgfx_ctx = NULL;
    }
    if (client->clipboard_ctx) {
        backend_client_clipboard_source_destroy(client);
    }
    if (client->gfxredir_ctx) {
        client->gfxredir_ctx->Close(client->gfxredir_ctx);

        gfxredir_server_context_free(client->gfxredir_ctx);
        client->activation_graphics_redirection_completed = false;
        client->gfxredir_ctx = NULL;
    }
    if (client->drdynvc_ctx) {
        client->drdynvc_ctx->Stop(client->drdynvc_ctx);

        drdynvc_server_context_free(client->drdynvc_ctx);
        client->drdynvc_ctx = NULL;
    }
    if (client->audio_tx) {
        backend_client_audio_tx_destroy(client);
        client->audio_tx = NULL;
    }
    if (client->audio_rx) {
        backend_client_audio_rx_destroy(client);
        client->audio_rx = NULL;
    }
}

static bool backend_client_channel_init(struct backend_client *client) {
    freerdp_settings_set_bool(client->peer->context->settings, FreeRDP_DesktopResize, FALSE);

    if (!backend_client_drdynvc_init(client)) {
        goto failed;
    }

    if (!backend_client_rail_init(client)) {
        goto failed;
    }

    if (!backend_client_disp_init(client)) {
        goto failed;
    }

    if (!backend_client_rdpgfx_init(client)) {
        goto failed;
    }

    if (client->backend->use_gfxredir) {
        if (!backend_client_gfxredir_init(client)) {
            goto failed;
        }
    }

    if (!backend_client_clipboard_init(client)) {
        goto failed;
    }

    if (!backend_client_audiorx_init(client)) {
        goto failed;
    }

    if (!backend_client_audiotx_init(client)) {
        goto failed;
    }


    {
        int waitRetry = 0; /* wait graphics channel (and optionally graphics redir channel) response from client */
        while (!client->activation_graphics_completed || (client->gfxredir_ctx && !client->activation_graphics_redirection_completed)) {
            if (++waitRetry > 10000) { /* timeout after 100 sec. */
                return false;
            }

            usleep(10000); /* wait 0.01 sec. */
            client->peer->CheckFileDescriptor(client->peer);
            WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
        }
    }

    client->current_frame_id = 0;
    client->acknowledged_frame_id = 0;
    return true;

failed:
    backend_client_channel_destroy(client);
    return false;
}

static void backend_client_sync_window_state(struct backend_client *client) {
    freerdp_peer *peer = client->peer;

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVESECURE,
            .setScreenSaveSecure = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVEACTIVE,
            .setScreenSaveActive = 0,
        };
        client->rail_ctx->ServerSysparam(client->rail_ctx, &sysParamOrder);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        RAIL_ZORDER_SYNC zOrderSync = {
            .windowIdMarker = BACKEND_WINDOW_MARKER_ID,
        };
        client->rail_ctx->ServerZOrderSync(client->rail_ctx, &zOrderSync);
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = BACKEND_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_HOOKED | WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        uint32_t windowsIdArray[1] = {0};
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = BACKEND_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
        };
        windowsIdArray[0] = BACKEND_WINDOW_MARKER_ID;

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {
            .activeWindowId = BACKEND_WINDOW_DESKTOP_ID,
            .numWindowIds = 1, .windowIds = (UINT *)&windowsIdArray,
        };

        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = BACKEND_WINDOW_MARKER_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
        };

        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};
        peer->context->update->window->MonitoredDesktop(
            peer->context->update->context, &window_order_info, &monitored_desktop_order
        );
        peer->DrainOutputBuffer(client->peer);
    }

    client->activation_rail_completed = true;
}

static BOOL xf_peer_adjust_monitor_layout(freerdp_peer *peer) {
    backend_log(INFO, "invoke ==> xf_peer_adjust_monitor_layout");

    struct backend_client *client = (struct backend_client*)peer->context;
    struct wsland_adapter *adapter = client->backend->adapter;

    rdpSettings *settings = client->peer->context->settings;
    int count = (int)freerdp_settings_get_uint32(settings, FreeRDP_MonitorCount);
    if (count > BACKEND_MAX_MONITOR) {
        backend_log(ERROR, "freerdp client reports more monitors then expected: [now: %d, max: %d]", count, BACKEND_MAX_MONITOR);
        return FALSE;
    }

    const rdpMonitor *rdp_monitor = freerdp_settings_get_pointer(settings, FreeRDP_MonitorDefArray);

    rdpMonitor *monitors;
    if (count > 0 && rdp_monitor) {
        monitors = malloc(sizeof(*monitors) * count);
        for (int idx = 0; idx < count; idx++) {
            monitors[idx] = rdp_monitor[idx];
            if (!freerdp_settings_get_bool(settings, FreeRDP_HasMonitorAttributes)) {
                monitors[idx].attributes.physicalWidth = 0;
                monitors[idx].attributes.physicalHeight = 0;
                monitors[idx].attributes.orientation = ORIENTATION_LANDSCAPE;
                monitors[idx].attributes.desktopScaleFactor = 100;
                monitors[idx].attributes.deviceScaleFactor = 100;
            }
        }
    } else {
        count = 1;
        monitors = malloc(sizeof(*monitors) * count);
        monitors[0].x = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopPosX);
        monitors[0].y = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopPosY);
        monitors[0].width = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopWidth);
        monitors[0].height = (int)freerdp_settings_get_uint32(settings, FreeRDP_DesktopHeight);
        monitors[0].attributes.orientation = freerdp_settings_get_uint16(settings, FreeRDP_DesktopOrientation);
        monitors[0].attributes.physicalWidth = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalWidth);
        monitors[0].attributes.physicalHeight = freerdp_settings_get_uint32(settings, FreeRDP_DesktopPhysicalHeight);
        monitors[0].attributes.desktopScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DesktopScaleFactor);
        monitors[0].attributes.deviceScaleFactor = freerdp_settings_get_uint32(settings, FreeRDP_DeviceScaleFactor);
        monitors[0].orig_screen = 0;
        monitors[0].is_primary = 1;
    }

    client->flags &= ~BACKEND_OUTPUT_ENABLED;
    BOOL success = adapter->handle->backend_adjust_monitor_layout(adapter, count, monitors);
    client->flags |= BACKEND_OUTPUT_ENABLED;

    free(monitors);
    return success;
}

static BOOL xf_peer_capabilities(freerdp_peer *peer) {
    backend_log(INFO, "invoke ==> xf_peer_capabilities");
    return TRUE;
}

static BOOL xf_peer_post_connect(freerdp_peer *peer) {
    backend_log(INFO, "invoke ==> xf_peer_post_connect");
    return TRUE;
}

static BOOL xf_peer_activate(freerdp_peer *peer) {
    backend_log(INFO, "invoke ==> xf_peer_activate");

    struct backend_client *client = (struct backend_client*)peer->context;
    struct wsland_adapter *adapter = client->backend->adapter;

    rdpSettings *settings = peer->context->settings;
    if (!freerdp_settings_get_bool(settings, FreeRDP_SurfaceCommandsEnabled)) {
        backend_log(ERROR, "freerdp client does not support SurfaceCommands");
        return FALSE;
    }

    if (!freerdp_settings_get_bool(settings, FreeRDP_RemoteApplicationMode)) {
        backend_log(ERROR, "freerdp client does not support RemoteApplicationMode");
        return FALSE;
    }

    if (!backend_client_channel_init(client)) {
        return FALSE;
    }

    backend_client_sync_window_state(client);
    if (client->flags & BACKEND_CLIENT_ACTIVATED) {
        return TRUE;
    }

    xf_peer_adjust_monitor_layout(peer);
    client->pointer = adapter->handle->backend_pointer_init(adapter);
    client->keyboard = adapter->handle->backend_keyboard_init(adapter);

    if (adapter->reconnect) {
        adapter->handle->backend_window_reconnect(adapter);
        adapter->reconnect = false;
    }

    client->flags |= BACKEND_CLIENT_ACTIVATED;
    return TRUE;
}

static BOOL xf_suppress_output(rdpContext *context, BYTE allow, const RECTANGLE_16 *area) {
    backend_log(INFO, "invoke ==> xf_suppress_output");
    return TRUE;
}

static BOOL xf_input_synchronize_event(rdpInput *input, UINT32 flags) {
    // backend_log(INFO, "invoke ==> xf_input_synchronize_event");

    struct backend_client *client = (struct backend_client*)input->context;
    struct wsland_adapter *adapter = client->backend->adapter;

    if (!(client->flags & BACKEND_OUTPUT_ENABLED)) {
        return TRUE;
    }

    adapter->handle->backend_input_synchronize(adapter, flags);
    return TRUE;
}

static BOOL xf_input_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    // backend_log(INFO, "invoke ==> xf_input_mouse_event");

    struct backend_client *client = (struct backend_client*)input->context;
    struct wsland_adapter *adapter = client->backend->adapter;

    if (!(client->flags & BACKEND_OUTPUT_ENABLED)) {
        return TRUE;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (flags & PTR_FLAGS_MOVE) {
        struct wlr_pointer_motion_absolute_event event = {0};
        event.pointer = &client->pointer->pointer;
        event.time_msec = timespec_to_msec(&now);
        event.x = x + adapter->area.x;
        event.y = y + adapter->area.y;
        adapter->handle->backend_mouse_move(adapter, &event);
        goto need_frame;
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
        event.pointer = &client->pointer->pointer;
        event.state = flags & PTR_FLAGS_DOWN ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        event.time_msec = timespec_to_msec(&now);
        event.button = button;
        adapter->handle->backend_mouse_button(&event);
        goto need_frame;
    }

    if (flags & PTR_FLAGS_WHEEL || flags & PTR_FLAGS_HWHEEL) {
        enum wl_pointer_axis axis;
        if (flags & PTR_FLAGS_WHEEL) {
            axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
        } else {
            axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
        }
        int8_t rdp_delta = (int8_t)(flags & 0xFF);
        int wheel_delta = -rdp_delta / WLR_POINTER_AXIS_DISCRETE_STEP;

        double delta = wheel_delta * 15;
        int32_t delta_discrete = wheel_delta * WLR_POINTER_AXIS_DISCRETE_STEP;

        struct wlr_pointer_axis_event event = {0};
        event.pointer = &client->pointer->pointer;
        event.time_msec = timespec_to_msec(&now);
        event.source = WL_POINTER_AXIS_SOURCE_WHEEL;
        event.delta_discrete = delta_discrete;
        event.orientation = axis;
        event.delta = delta;
        adapter->handle->backend_mouse_axis(adapter, &event);
        goto need_frame;
    }
    return TRUE;

need_frame:
    adapter->handle->backend_mouse_frame(adapter);
    return TRUE;
}

static BOOL xf_input_extended_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    backend_log(INFO, "invoke ==> xf_input_extended_mouse_event");
    return TRUE;
}

static BOOL xf_input_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
    // backend_log(INFO, "invoke ==> xf_input_keyboard_event");

    struct backend_client *client = (struct backend_client*)input->context;
    struct wsland_adapter *adapter = client->backend->adapter;

    if (!(client->flags & BACKEND_OUTPUT_ENABLED)) {
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
        uint32_t scan_code = GetKeycodeFromVirtualKeyCode(vk_code, WINPR_KEYCODE_TYPE_XKB);
        uint32_t key_code = scan_code;

        if (is_release) {
            if (client->pressed[key_code]) {
                client->pressed[key_code] = false;
                notify = true;
            }
        } else {
            if (!client->pressed[key_code]) {
                client->pressed[key_code] = true;
                notify = true;
            }
        }

        if (notify) {
            struct wlr_keyboard_key_event event = {0};
            event.time_msec = timespec_to_msec(&now);
            event.keycode = key_code - 8;
            event.state = state;
            event.update_state = true;
            adapter->handle->backend_keyboard_key(client->keyboard, &event);
        }
    }

    return TRUE;
}

static BOOL xf_input_unicode_keyboard_event(rdpInput *input, UINT16 flags, UINT16 code) {
    backend_log(INFO, "invoke ==> xf_input_unicode_keyboard_event");
    return TRUE;
}

static int backend_client_context_new(freerdp_peer *peer, struct backend_client *client) {
    client->peer = peer;

    client->dispatch_fd = -1;
    wl_list_init(&client->dispatch_tasks);
    return true;
}

static void backend_client_context_free(freerdp_peer *_, struct backend_client *client) {
    if (!client) {
        return;
    }
    client->flags &= ~BACKEND_CLIENT_ACTIVATED;
    client->flags &= ~BACKEND_OUTPUT_ENABLED;


    struct wsland_adapter *adapter = client->backend->adapter;
    adapter->handle->backend_source_release(adapter, client->pointer, client->keyboard);

    for (int i = 0; i < BACKEND_MAX_FDS + 1; ++i) {
        adapter->handle->server_remove_event_source(client->sources[i]);
        client->sources[i] = 0;
    }

    if (client->dispatch_event_source) {
        adapter->handle->server_remove_event_source(client->dispatch_event_source);
        client->dispatch_event_source = NULL;
    }

    struct wsland_window *window, *temp;
    wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
        if (window->render.gfx_buffer) {
            adapter->handle->backend_window_buffer_destroy(window);
        }
    }

    struct backend_task *task, *t_temp;
    wl_list_for_each_reverse_safe(task, t_temp, &client->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        task->func(true, task);
    }

    if (client->dispatch_fd != -1) {
        close(client->dispatch_fd);
        client->dispatch_fd = -1;
    }

    pthread_mutex_destroy(&client->dispatch_mutex);
    backend_client_channel_destroy(client);
    client->backend->client = NULL;
    adapter->reconnect = true;
}

// ReSharper disable once CppDFAConstantFunctionResult
static int backend_client_activity(int fd, uint32_t mask, void *data) {
    struct backend_client *client = data;
    freerdp_peer *peer = client->peer;

    if (!peer->CheckFileDescriptor(peer)) {
        backend_log(ERROR, "Unable to check client file descriptor for %p", (void *) data);
        goto peer_release;
    }

    if (client->vcm) {
        if (!client->peer->activated || !WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
            return 0;
        }

        if (!WTSVirtualChannelManagerCheckFileDescriptor(client->vcm)) {
            backend_log(ERROR, "failed to check freerdp wts vc file descriptor for %p", data);
            goto peer_release;
        }
    }

    wsland_client_event_handle(client);
    return 0;

peer_release:
    freerdp_peer_context_free(peer);
    freerdp_peer_free(peer);
    return 0;
}

void backend_task_dispatch(struct backend_client *client, backend_task_func_t func, struct backend_task *task) {
    task->client = client;
    task->func = func;

    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_insert(&client->dispatch_tasks, &task->link);
    pthread_mutex_unlock(&client->dispatch_mutex);

    eventfd_write(client->dispatch_fd, 1);
}

static int backend_task_check(int fd, uint32_t mask, void *user_data) {
    struct backend_client* client = user_data;

    eventfd_t dummy;
    eventfd_read(client->dispatch_fd, &dummy);

    struct backend_task *task, *temp;
    pthread_mutex_lock(&client->dispatch_mutex);
    wl_list_for_each_reverse_safe(task, temp, &client->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        break;
    }

    pthread_mutex_unlock(&client->dispatch_mutex);
    task->func(false, task);
    return 0;
}

static bool backend_task_dispatch_init(struct backend_client* client) {
    if (pthread_mutex_init(&client->dispatch_mutex, NULL) == -1) { // NOLINT(*-posix-return)
        goto mutex_failed;
    }

    client->dispatch_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (client->dispatch_fd == -1) {
        goto dispatch_fd_error;
    }

    client->dispatch_event_source = client->backend->adapter->handle->server_dispatch_event_source(
        client->backend->adapter, client->dispatch_fd, WL_EVENT_READABLE, backend_task_check, client
    );

    if (!client->dispatch_event_source) {
        goto dispatch_event_source_error;
    }
    return true;

dispatch_event_source_error:
    close(client->dispatch_fd);
    client->dispatch_fd = -1;
dispatch_fd_error:
    pthread_mutex_destroy(&client->dispatch_mutex);
mutex_failed:
    return false;
}

static bool backend_client_init(struct wsland_backend *backend, freerdp_peer *peer) {
    bool start = false;

    if (backend->client) {
        backend_log(ERROR, "already a client connection, system current only support single client!")
        return false;
    }

    peer->ContextSize = sizeof(struct backend_client);
    peer->ContextNew = (psPeerContextNew)backend_client_context_new;
    peer->ContextFree = (psPeerContextFree)backend_client_context_free;
    freerdp_peer_context_new(peer);

    struct backend_client *client = (struct backend_client*)peer->context;
    client->backend = backend;

    rdpSettings *settings = peer->context->settings;
    if (backend->key_content && backend->cert_content) {
        rdpCertificate *cert = freerdp_certificate_new_from_pem(backend->cert_content);
        rdpPrivateKey *key = freerdp_key_new_from_pem(backend->key_content);

        if (!freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1) ||
            !freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1)) {
            freerdp_certificate_free(cert);
            freerdp_key_free(key);
            goto init_failed;
        }
    }
    else {
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, FALSE);
    }
    freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_ExtSecurity, FALSE);

    if (!peer->Initialize(peer)) {
        backend_log(ERROR, "failed to invoke freerdp peer Initialize");
        goto init_failed;
    }
    start = true;

    freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMajorType, OSMAJORTYPE_UNIX);
    freerdp_settings_set_uint32(settings, FreeRDP_OsMinorType, OSMINORTYPE_PSEUDO_XSERVER);
    freerdp_settings_set_bool(settings, FreeRDP_FrameMarkerCommandEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SurfaceFrameMarkerEnabled, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RefreshRect, TRUE);
    freerdp_settings_set_uint32(settings, FreeRDP_RemoteApplicationSupportLevel, RAIL_LEVEL_SUPPORTED |
        RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED | RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED | RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED);
    freerdp_settings_set_bool(settings, FreeRDP_SupportGraphicsPipeline, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMonitorLayoutPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportErrorInfoPdu, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_RedirectClipboard, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasExtendedMouseEvent, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_HasHorizontalWheel, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_FastPathInput, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioPlayback, TRUE);
    freerdp_settings_set_bool(settings, FreeRDP_AudioCapture, TRUE);

    freerdp_settings_set_bool(settings, FreeRDP_NetworkAutoDetect, FALSE);
    freerdp_settings_set_bool(settings, FreeRDP_SupportMultitransport, FALSE);
    freerdp_settings_set_uint32(settings, FreeRDP_MultitransportFlags, 0);

    peer->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;
    peer->Capabilities = xf_peer_capabilities;
    peer->PostConnect = xf_peer_post_connect;
    peer->Activate = xf_peer_activate;

    rdpUpdate *update = peer->context->update;
    update->SuppressOutput = xf_suppress_output;

    rdpInput *input = peer->context->input;
    input->SynchronizeEvent = xf_input_synchronize_event;
    input->MouseEvent = xf_input_mouse_event;
    input->ExtendedMouseEvent = xf_input_extended_mouse_event;
    input->KeyboardEvent = xf_input_keyboard_event;
    input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

    HANDLE handles[BACKEND_MAX_FDS];
    int count = (int)peer->GetEventHandles(peer, handles, BACKEND_MAX_FDS);
    if (!count) {
        backend_log(ERROR, "failed to invoke freerdp peer GetEventHandles");
        goto init_failed;
    }

    PWtsApiFunctionTable func_table = FreeRDP_InitWtsApi();
    WTSRegisterWtsApiFunctionTable(func_table);
    client->vcm = WTSOpenServerA((LPSTR)client);
    if (client->vcm) {
        handles[count++] = WTSVirtualChannelManagerGetEventHandle(client->vcm);
    }
    else {
        backend_log(ERROR, "failed to invoke freerdp peer WTSOpenServerA");
    }

    int i;
    for (i = 0; i < count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);

        client->sources[i] = backend->adapter->handle->server_dispatch_event_source(
            backend->adapter, fd, WL_EVENT_READABLE, backend_client_activity, client
        );
    }

    for (; i < BACKEND_MAX_FDS; ++i) {
        client->sources[i] = 0;
    }

    if (!backend_task_dispatch_init(client)) {
        backend_log(ERROR, "failed to invoke backend_task_dispatch_init");
        goto init_failed;
    }

    if (!backend->client) {
        backend->client = client;
    }

    wl_list_init(&client->event.clipboard_request);
    wl_list_init(&client->event.clipboard_response);
    return true;

init_failed:
    if (start) {
        peer->Close(peer);
    }
    return false;
}

BOOL wsland_backend_incoming_client(freerdp_listener *listener, freerdp_peer *peer) {
    struct wsland_backend *backend = listener->param4;

    if (!backend_client_init(backend, peer)) {
        backend_log(ERROR, "error initializing incoming client");
        return FALSE;
    }
    return TRUE;
}