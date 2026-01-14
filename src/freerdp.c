// ReSharper disable All
#define _GNU_SOURCE 1

#include <wlr/util/log.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_output.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/disp.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/freerdp.h>

#include <linux/input.h>
#include <linux/vm_sockets.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>

#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "wsland.h"


static pid_t rdp_get_tid(void) {
#ifdef SYS_gettid
    return syscall(SYS_gettid);
#else
    return gettid();
#endif
}

extern PWtsApiFunctionTable FreeRDP_InitWtsApi(void);


static void wlr_add_rdp_output(void *data) {
    struct wsland_peer_context *peer_ctx = data;
    server.freerdp.peer_ctx = peer_ctx;

    rdpSettings *sets = peer_ctx->peer->settings;
    if (!sets->MonitorCount || sets->MonitorCount > RDP_MAX_MONITOR) {
        sets->MonitorCount = 1;
        sets->MonitorDefArray[0].is_primary = true;
        sets->MonitorDefArray[0].x = 0;
        sets->MonitorDefArray[0].y = 0;
        sets->MonitorDefArray[0].width = sets->DesktopWidth;
        sets->MonitorDefArray[0].height = sets->DesktopHeight;
        sets->MonitorDefArray[0].attributes.orientation = sets->DesktopOrientation;
        sets->MonitorDefArray[0].attributes.physicalWidth = sets->DesktopPhysicalWidth;
        sets->MonitorDefArray[0].attributes.physicalHeight = sets->DesktopPhysicalHeight;
        sets->MonitorDefArray[0].attributes.desktopScaleFactor = sets->DesktopScaleFactor;
        sets->MonitorDefArray[0].attributes.deviceScaleFactor = sets->DeviceScaleFactor;
    }

    for (uint32_t index = 0; index < sets->MonitorCount; ++index) {
        rdpMonitor monitor = sets->MonitorDefArray[index];

        struct wlr_output *wlr_output = wlr_headless_add_output(
            server.backend, monitor.width, monitor.height
        );

        if (!wlr_output) {
            wlr_log(WLR_ERROR, "failed to invoke headless add output");
            continue;
        }

        struct wl_global *global = wlr_output->global;
        wlr_output->global = NULL;

        char name[13] = {0};
        sprintf(name, "rdp-%x", index);
        wlr_output_set_name(wlr_output, name);
        char desc[64] = {0};
        sprintf(desc, "wsland rdp output %x", index);
        wlr_output_set_description(wlr_output, desc);
        wlr_output->global = global;

        struct wsland_output *output = wlr_output->data;
        output->desktop_box = (struct wlr_box){ monitor.x, monitor.y, monitor.width, monitor.height };

        if (monitor.is_primary) {
            server.primary_output = output;
        }
    }

    peer_ctx->flags |= WSLAND_RDP_PEER_OUTPUT_ENABLED;

    if (server.command) {
        if (fork() == 0) {
            execl("/bin/sh", "/bin/sh", "-c", server.command, (void*)NULL);
        }
    }
}

static void wlr_rdp_add_keyboard(void *data) {
    struct wsland_peer_context *client_ctx = data;

    struct wlr_input_device device = {
        .type = WLR_INPUT_DEVICE_KEYBOARD,
        .name = "wsland-keyboard",
        .data = client_ctx
    };
    wl_signal_emit_mutable(&server.backend->events.new_input, &device);
}

static void wlr_rdp_add_pointer(void *data) {
    struct wsland_peer_context *client_ctx = data;

    struct wlr_input_device device = {
        .type = WLR_INPUT_DEVICE_POINTER,
        .name = "wsland-pointer",
        .data = client_ctx
    };
    wl_signal_emit_mutable(&server.backend->events.new_input, &device);
}

static BOOL xf_peer_adjust_monitor_layout(freerdp_peer *client) {
    return TRUE;
}

static BOOL xf_peer_capabilities(struct rdp_freerdp_peer *client) {
    return TRUE;
}

static BOOL xf_peer_post_connect(struct rdp_freerdp_peer *client) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)client->context;
    return TRUE;
}

static void xf_peer_disconnect(struct rdp_freerdp_peer *client) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)client->context;
}

static void disp_force_recreate_iter(void *element, void *data) {
    struct wlr_surface *surface = element;
    struct wsland_surface *wsland_surface = surface->data;

    wsland_surface->force_recreate_surface = TRUE;
    wsland_surface->force_update_window_state = TRUE;
}

static void rail_sync_window_status(struct rdp_freerdp_peer *client) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)client->context;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    RailServerContext *rail_ctx = client_ctx->ctx_server_rail;
    bool anyWindowCreated = false;

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVESECURE,
            .setScreenSaveSecure = 0,
        };
        rail_ctx->ServerSysparam(rail_ctx, &sysParamOrder);
        client->DrainOutputBuffer(client);
    }

    {
        RAIL_SYSPARAM_ORDER sysParamOrder = {
            .param = SPI_SETSCREENSAVEACTIVE,
            .setScreenSaveActive = 0,
        };
        rail_ctx->ServerSysparam(rail_ctx, &sysParamOrder);
        client->DrainOutputBuffer(client);
    }

    {
        RAIL_ZORDER_SYNC zOrderSync = {
            .windowIdMarker = RDP_RAIL_MARKER_WINDOW_ID,
        };
        rail_ctx->ServerZOrderSync(rail_ctx, &zOrderSync);
        client->DrainOutputBuffer(client);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RDP_RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
            WINDOW_ORDER_FIELD_DESKTOP_HOOKED |
            WINDOW_ORDER_FIELD_DESKTOP_ARC_BEGAN,
        };
        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};

        client->update->window->MonitoredDesktop(
            client->update->context, &window_order_info, &monitored_desktop_order
        );
        client->DrainOutputBuffer(client);
    }

    {
        uint32_t windowsIdArray[1] = {0};
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RDP_RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
            WINDOW_ORDER_FIELD_DESKTOP_ZORDER |
            WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND,
        };
        windowsIdArray[0] = RDP_RAIL_MARKER_WINDOW_ID;
        MONITORED_DESKTOP_ORDER monitored_desktop_order = {
            .activeWindowId = RDP_RAIL_DESKTOP_WINDOW_ID,
            .numWindowIds = 1,
            .windowIds = (UINT*)&windowsIdArray,
        };

        client->update->window->MonitoredDesktop(
            client->update->context, &window_order_info, &monitored_desktop_order
        );
        client->DrainOutputBuffer(client);
    }

    {
        WINDOW_ORDER_INFO window_order_info = {
            .windowId = RDP_RAIL_MARKER_WINDOW_ID,
            .fieldFlags = WINDOW_ORDER_TYPE_DESKTOP |
            WINDOW_ORDER_FIELD_DESKTOP_ARC_COMPLETED,
        };
        MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};

        client->update->window->MonitoredDesktop(
            client->update->context, &window_order_info, &monitored_desktop_order
        );
        client->DrainOutputBuffer(client);
    }
    client_ctx->activation_rail_completed = true;
}

static bool rail_peer_init(struct rdp_freerdp_peer *peer) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)peer->context;

    peer->settings->DesktopResize = FALSE; /* Server must not ask client to resize */

    if (!wsland_freerdp_ctx_server_drdynvc_init(peer_ctx)) {
        goto failed;
    }

    if (!wsland_freerdp_ctx_server_rail_init(peer_ctx)) {
        goto failed;
    }

    if (!wsland_freerdp_ctx_server_disp_init(peer_ctx)) {
        goto failed;
    }

    if (!wsland_freerdp_ctx_server_rdpgfx_init(peer_ctx)) {
        goto failed;
    }

    if (server.freerdp.use_gfxredir) {
        if (!wsland_freerdp_ctx_server_gfxredir_init(peer_ctx)) {
            goto failed;
        }
    }

    if (!wsland_freerdp_ctx_server_applist_init(peer_ctx)) {
        goto failed;
    }

    /* subscribe idle/wake signal from compositor */
    /** todo
    client_ctx->idle_listener.notify = rdp_rail_idle_handler;
    wl_signal_add(&b->compositor->idle_signal, &client_ctx->idle_listener);
    client_ctx->wake_listener.notify = rdp_rail_wake_handler;
    wl_signal_add(&b->compositor->wake_signal, &client_ctx->wake_listener);*/

    peer_ctx->current_frame_id = 0;
    peer_ctx->acknowledged_frame_id = 0;
    return true;

failed:
    if (peer_ctx->ctx_server_drdynvc) {
        peer_ctx->ctx_server_drdynvc->Stop(peer_ctx->ctx_server_drdynvc);
    }
    if (peer_ctx->ctx_server_disp) {
        peer_ctx->ctx_server_disp->Close(peer_ctx->ctx_server_disp);
    }
    if (peer_ctx->ctx_server_rdpgfx) {
        peer_ctx->ctx_server_rdpgfx->Close(peer_ctx->ctx_server_rdpgfx);
    }
    if (peer_ctx->ctx_server_gfxredir) {
        peer_ctx->ctx_server_gfxredir->Close(peer_ctx->ctx_server_gfxredir);
    }
    if (peer_ctx->ctx_server_applist) {
        peer_ctx->ctx_server_applist->Close(peer_ctx->ctx_server_applist);
    }
    return false;
}

static BOOL xf_peer_activate(struct rdp_freerdp_peer *peer) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)peer->context;
    struct rdp_settings *settings = peer->settings;

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

    rail_sync_window_status(peer);

    if (peer_ctx->flags & WSLAND_RDP_PEER_ACTIVATED) {
        return TRUE;
    }

    if (!wl_event_loop_add_idle(server.event_loop, wlr_add_rdp_output, peer_ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add output");
        return FALSE;
    }

    if (!wl_event_loop_add_idle(server.event_loop, wlr_rdp_add_pointer, peer_ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add pointer");
        return FALSE;
    }

    if (!wl_event_loop_add_idle(server.event_loop, wlr_rdp_add_keyboard, peer_ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add keyboard");
        return FALSE;
    }

    {
        // Use wlroots' software cursors instead of remote cursors
        POINTER_SYSTEM_UPDATE pointer_system;
        rdpPointerUpdate *pointer = peer->update->pointer;
        pointer_system.type = SYSPTR_NULL;
        pointer->PointerSystem(peer->context, &pointer_system);
    }

    peer_ctx->flags |= WSLAND_RDP_PEER_ACTIVATED;
    return TRUE;
}

static int xf_suppress_output(struct rdp_context *context, BYTE allow, const RECTANGLE_16 *area) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)context;

    if (allow) {
        client_ctx->flags |= WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    else {
        client_ctx->flags &= ~WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    return true;
}

static int xf_input_synchronize_event(struct rdp_input *input, UINT32 flags) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)input->context;

    // wlr_output_update_needs_frame(context->output->base);
    // wlr_output_damage_whole(&context->output->base);
    return true;
}

static int64_t timespec_to_msec(const struct timespec *a) {
    return (int64_t)a->tv_sec * 1000 + a->tv_nsec / 1000000;
}

static int xf_input_mouse_event(struct rdp_input *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)input->context;
    struct wlr_pointer *pointer = &client_ctx->pointer->pointer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(client_ctx->flags & WSLAND_RDP_PEER_POINTER_ENABLE)) {
        return true;
    }

    bool frame = false;
    if (flags & PTR_FLAGS_MOVE) {
        struct wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout, x, y);
        if (wlr_output) {
            struct wsland_output *w_output = wlr_output->data;

            struct wlr_pointer_motion_absolute_event event = {0};
            event.pointer = pointer;
            event.time_msec = timespec_to_msec(&now);
            event.x = x;
            event.y = y;
            wl_signal_emit_mutable(&pointer->events.motion_absolute, &event);
            frame = true;
        }
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
        event.pointer = pointer;
        event.time_msec = timespec_to_msec(&now);
        event.button = button;
        event.state = (flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        wl_signal_emit_mutable(&pointer->events.button, &event);
        frame = true;
    }

    if (flags & PTR_FLAGS_WHEEL) {
        double value = -(flags & 0xFF) / 120.0;
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) {
            value = -value;
        }

        struct wlr_pointer_axis_event event = {0};
        event.pointer = pointer;
        event.time_msec = timespec_to_msec(&now);
        event.source = WL_POINTER_AXIS_SOURCE_WHEEL;
        event.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
        event.delta = value;
        event.delta_discrete = (int32_t)value;
        wl_signal_emit_mutable(&pointer->events.axis, &event);
        frame = true;
    }

    if (frame) {
        wl_signal_emit_mutable(&pointer->events.frame, pointer);
    }

    return true;
}

static int xf_input_extended_mouse_event(struct rdp_input *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)input->context;
    struct wlr_pointer *pointer = &client_ctx->pointer->pointer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(client_ctx->flags & WSLAND_RDP_PEER_POINTER_ENABLE)) {
        return true;
    }

    struct wlr_pointer_motion_absolute_event event = {0};
    event.pointer = pointer;
    event.time_msec = timespec_to_msec(&now);
    event.x = x / (double)client_ctx->output->base->width;
    event.y = y / (double)client_ctx->output->base->height;
    wl_signal_emit_mutable(&pointer->events.motion_absolute, &event);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
    return true;
}

static int xf_input_keyboard_event(struct rdp_input *input, UINT16 flags, UINT16 code) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)input->context;
    struct wlr_keyboard *keyboard = &client_ctx->keyboard->keyboard;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(client_ctx->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE)) {
        return true;
    }

    bool notify = false;
    enum wl_keyboard_key_state state = {0};
    if ((flags & KBD_FLAGS_DOWN)) {
        state = WL_KEYBOARD_KEY_STATE_PRESSED;
        notify = true;
    }
    else if ((flags & KBD_FLAGS_RELEASE)) {
        state = WL_KEYBOARD_KEY_STATE_RELEASED;
        notify = true;
    }

    if (notify) {
        uint32_t full_code = code;
        if (flags & KBD_FLAGS_EXTENDED) {
            full_code |= KBD_FLAGS_EXTENDED;
        }

        uint32_t vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
        if (flags & KBD_FLAGS_EXTENDED) {
            vk_code |= KBDEXT;
        }

        uint32_t scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);
        struct wlr_keyboard_key_event event = {0};
        event.time_msec = timespec_to_msec(&now);
        event.keycode = scan_code - 8;
        event.state = state;
        event.update_state = true;
        wlr_keyboard_notify_key(keyboard, &event);
    }

    return true;
}

static int xf_input_unicode_keyboard_event(struct rdp_input *input, UINT16 flags, UINT16 code) {
    struct wsland_peer_context *context = (struct wsland_peer_context*)input->context;
    wlr_log(WLR_DEBUG, "Unhandled RDP unicode keyboard event " "(flags:0x%X code:0x%X)", flags, code);

    if (!(context->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE)) {
        return true;
    }

    return true;
}

static int rdp_client_activity(int fd, uint32_t mask, void *data) {
    struct rdp_freerdp_peer *client = (struct rdp_freerdp_peer*)data;
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)client->context;

    if (!client->CheckFileDescriptor(client)) {
        wlr_log(WLR_ERROR, "Unable to check client file descriptor for %p", (void *) client);
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
    }

    if (client_ctx && client_ctx->vcm) {
        if (!WTSVirtualChannelManagerCheckFileDescriptor(client_ctx->vcm)) {
            wlr_log(WLR_ERROR, "failed to check freerdp wts vc file descriptor for %p", data);
            freerdp_peer_context_free(client);
            freerdp_peer_free(client);
            exit(0);
        }
    }

    return 0;
}

static int rdp_peer_context_new(struct rdp_freerdp_peer *client, struct wsland_peer_context *client_ctx) {
    client_ctx->peer = client;

    client_ctx->loop_task_event_source_fd = -1;
    client_ctx->loop_task_event_source = NULL;
    wl_list_init(&client_ctx->loop_task_list);
    return true;
}

static void rdp_peer_context_free(struct rdp_freerdp_peer *client, struct wsland_peer_context *client_ctx) {
    if (!client_ctx) {
        return;
    }

    for (int i = 0; i < MAX_FREERDP_FDS; ++i) {
        if (client_ctx->events[i]) {
            wl_event_source_remove(client_ctx->events[i]);
        }
    }
    if (client_ctx->flags & WSLAND_RDP_PEER_OUTPUT_ENABLED) {
        client_ctx->flags &= WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    if (client_ctx->flags & WSLAND_RDP_PEER_POINTER_ENABLE) {
        wlr_pointer_finish(&client_ctx->pointer->pointer);
    }
    if (client_ctx->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE) {
        wlr_keyboard_finish(&client_ctx->keyboard->keyboard);
    }

    server.freerdp.peer_ctx = 0;
}

static int rdp_peer_init(struct rdp_freerdp_peer *peer) {
    peer->ContextSize = sizeof(struct wsland_peer_context);
    peer->ContextNew = (psPeerContextNew)rdp_peer_context_new;
    peer->ContextFree = (psPeerContextFree)rdp_peer_context_free;
    freerdp_peer_context_new(peer);

    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)peer->context;

    if (server.freerdp.tls_key_content && server.freerdp.tls_cert_content) {
        peer->settings->CertificateContent = strdup(server.freerdp.tls_cert_content);
        peer->settings->PrivateKeyContent = strdup(server.freerdp.tls_key_content);
        peer->settings->RdpKeyContent = strdup(server.freerdp.tls_key_content);
    }
    else {
        peer->settings->TlsSecurity = FALSE;
    }
    peer->settings->NlaSecurity = FALSE;

    if (!peer->Initialize(peer)) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer Initialize");
        goto err_init;
    }

    peer->settings->ColorDepth = 32;
    peer->settings->NSCodec = FALSE;
    peer->settings->RemoteFxCodec = FALSE;
    peer->settings->OsMajorType = OSMAJORTYPE_UNIX;
    peer->settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
    peer->settings->FrameMarkerCommandEnabled = TRUE;
    peer->settings->SurfaceFrameMarkerEnabled = TRUE;
    peer->settings->RefreshRect = TRUE;

    UINT32 remote_application_level = RAIL_LEVEL_SUPPORTED |
        RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED |
        RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED;
    peer->settings->RemoteApplicationSupportLevel = remote_application_level;
    peer->settings->SupportGraphicsPipeline = TRUE;
    peer->settings->SupportMonitorLayoutPdu = TRUE;
    peer->settings->RemoteApplicationMode = TRUE;

    peer->AdjustMonitorsLayout = xf_peer_adjust_monitor_layout;
    peer->Capabilities = xf_peer_capabilities;
    peer->PostConnect = xf_peer_post_connect;
    peer->Disconnect = xf_peer_disconnect;
    peer->Activate = xf_peer_activate;

    peer->update->SuppressOutput = (pSuppressOutput)xf_suppress_output;

    peer->input->SynchronizeEvent = xf_input_synchronize_event;
    peer->input->MouseEvent = xf_input_mouse_event;
    peer->input->ExtendedMouseEvent = xf_input_extended_mouse_event;
    peer->input->KeyboardEvent = xf_input_keyboard_event;
    peer->input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

    HANDLE handles[MAX_FREERDP_FDS + 1]; /* +1 for virtual channel */
    int handle_count = peer->GetEventHandles(peer, handles, MAX_FREERDP_FDS);
    if (!handle_count) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer GetEventHandles");
        goto err_init;
    }

    PWtsApiFunctionTable func_table = FreeRDP_InitWtsApi();
    WTSRegisterWtsApiFunctionTable(func_table);
    peer_ctx->vcm = WTSOpenServerA((LPSTR)peer_ctx);
    if (peer_ctx->vcm) {
        handles[handle_count++] = WTSVirtualChannelManagerGetEventHandle(peer_ctx->vcm);
    }
    else {
        wlr_log(WLR_ERROR, "failed to invoke freerdp peer WTSOpenServerA");
    }

    int i;
    for (i = 0; i < handle_count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);
        peer_ctx->events[i] = wl_event_loop_add_fd(
            server.event_loop, fd, WL_EVENT_READABLE, rdp_client_activity, peer
        );
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        peer_ctx->events[i] = 0;
    }

    wsland_freerdp_dispatch_task_init(peer_ctx);
    return 0;

err_init:
    peer->Close(peer);
    return -1;
}

static int rdp_incoming_peer(struct rdp_freerdp_listener *listener, struct rdp_freerdp_peer *client) {
    if (rdp_peer_init(client) < 0) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp rdp_peer_init");
        return false;
    }
    return true;
}

static int rdp_listener_activity(int fd, uint32_t mask, void *data) {
    freerdp_listener *listener = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    if (!listener->CheckFileDescriptor(listener)) {
        wlr_log(WLR_ERROR, "failed to check freerdp file descriptor");
        return -1;
    }
    return 0;
}

static int create_vsock_fd(int port) {
    struct sockaddr_vm socket_address;

    int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0) {
        wlr_log(WLR_INFO, "fail to create vsocket");
        return -1;
    }

    const int bufferSize = 65536;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wlr_log(WLR_INFO, "fail to setsockopt SO_SNDBUF");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wlr_log(WLR_INFO, "fail to setsockopt SO_RCVBUF");
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));

    socket_address.svm_family = AF_VSOCK;
    socket_address.svm_cid = VMADDR_CID_ANY;
    socket_address.svm_port = port;

    socklen_t socket_addr_size = sizeof(socket_address);

    if (bind(socket_fd, (const struct sockaddr*)&socket_address, socket_addr_size) < 0) {
        wlr_log(WLR_INFO, "fail to bind socket to address socket");
        close(socket_fd);
        return -2;
    }

    int status = listen(socket_fd, 1);

    if (status != 0) {
        wlr_log(WLR_INFO, "fail to listen on socket");
        close(socket_fd);
        return -4;
    }
    return socket_fd;
}

static int use_vsock_fd(int port) {
    char *fd_str = getenv("USE_VSOCK");
    if (!fd_str) {
        return -1;
    }

    int fd;
    if (strlen(fd_str) != 0) {
        fd = atoi(fd_str);
        wlr_log(WLR_INFO, "using external fd for incoming connections: %d", fd);
        if (fd == 0) {
            fd = -1;
        }
    }
    else {
        fd = create_vsock_fd(port);
        wlr_log(WLR_INFO, "created vsock for external connections: %d", fd);
    }
    return fd;
}

bool wsland_freerdp_init(void) {
    server.freerdp.freerdp_shell_api = wsland_freerdp_shell_api_init();
    server.freerdp.listener = freerdp_listener_new();
    if (!server.freerdp.listener) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp_listener_new");
        return false;
    }
    server.freerdp.listener->PeerAccepted = rdp_incoming_peer;

    int vosck_fd = use_vsock_fd(server.freerdp.port);
    if (vosck_fd < 0) {
        wsland_freerdp_tls_generate();
    }

    if (vosck_fd > 0) {
        if (!server.freerdp.listener->OpenFromSocket(server.freerdp.listener, vosck_fd)) {
            wlr_log(WLR_ERROR, "failed to invoke freerdp OpenFromSocket [ fd: %d ]", vosck_fd);
            return false;
        }
    }
    else {
        if (!server.freerdp.listener->Open(server.freerdp.listener, server.freerdp.address, server.freerdp.port)) {
            wlr_log(WLR_ERROR, "failed to invoke freerdp Open");
            return false;
        }
    }

    int rcount = 0;
    void *rfds[MAX_FREERDP_FDS];
    if (!server.freerdp.listener->GetFileDescriptor(server.freerdp.listener, rfds, &rcount)) {
        wlr_log(WLR_ERROR, "failed to invoke freerdp GetFileDescriptor");
        return false;
    }

    int i;
    for (i = 0; i < rcount; ++i) {
        int fd = (int)(long)(rfds[i]);
        server.freerdp.listener_events[i] = wl_event_loop_add_fd(
            server.event_loop, fd, WL_EVENT_READABLE, rdp_listener_activity, server.freerdp.listener
        );
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        server.freerdp.listener_events[i] = 0;
    }

    return true;
}

void wsland_freerdp_destroy(void) {
    if (server.freerdp.listener) {
        freerdp_listener_free(server.freerdp.listener);
    }
}
