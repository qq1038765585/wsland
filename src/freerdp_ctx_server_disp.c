// ReSharper disable All
#include <assert.h>

#include <wlr/util/log.h>

#include "wsland.h"

struct disp_schedule_monitor_layout_change_data {
    struct wsland_loop_task _base;
    DispServerContext *context;
    rdpMonitor *monitors;
    int count;
};


static float disp_get_client_scale_from_monitor(const rdpMonitor *config) {
    if (config->attributes.desktopScaleFactor == 0.0) {
        return 1.0f;
    }

    if (server.freerdp.enable_hi_dpi_support) {
        if (server.freerdp.debug_desktop_scaling_factor) {
            return (float)server.freerdp.debug_desktop_scaling_factor / 100.f;
        }
        else if (server.freerdp.enable_fractional_hi_dpi_support) {
            return (float)config->attributes.desktopScaleFactor / 100.0f;
        }
        else if (server.freerdp.enable_fractional_hi_dpi_roundup) {
            return (float)(int)((config->attributes.desktopScaleFactor + 50) / 100);
        }
        else {
            return (float)(int)(config->attributes.desktopScaleFactor / 100);
        }
    }
    else {
        return 1.0f;
    }
}

static int disp_get_output_scale_from_monitor(const rdpMonitor *config) {
    return (int)disp_get_client_scale_from_monitor(config);
}

static bool disp_monitor_sanity_check_layout(struct wsland_peer_context *peer_ctx, rdpMonitor *config, uint32_t count) {
    uint32_t primaryCount = 0;
    uint32_t i;

    /* dump client monitor topology */
    wlr_log(WLR_DEBUG, "%s:---INPUT---", __func__);
    for (i = 0; i < count; i++) {
        float client_scale = disp_get_client_scale_from_monitor(&config[i]);
        int scale = disp_get_output_scale_from_monitor(&config[i]);

        wlr_log(
            WLR_DEBUG, "	rdpMonitor[%d]: x:%d, y:%d, width:%d, height:%d, is_primary:%d",
            i, config[i].x, config[i].y, config[i].width, config[i].height, config[i].is_primary
        );
        wlr_log(
            WLR_DEBUG, "	rdpMonitor[%d]: physicalWidth:%d, physicalHeight:%d, orientation:%d",
            i, config[i].attributes.physicalWidth, config[i].attributes.physicalHeight, config[i].attributes.orientation
        );
        wlr_log(
            WLR_DEBUG, "	rdpMonitor[%d]: desktopScaleFactor:%d, deviceScaleFactor:%d",
            i, config[i].attributes.desktopScaleFactor, config[i].attributes.deviceScaleFactor
        );
        wlr_log(WLR_DEBUG, "	rdpMonitor[%d]: scale:%d, client scale :%3.2f", i, scale, client_scale);
    }

    for (i = 0; i < count; i++) {
        /* make sure there is only one primary and its position at client */
        if (config[i].is_primary) {
            /* count number of primary */
            if (++primaryCount > 1) {
                wlr_log(WLR_ERROR, "%s: RDP client reported unexpected primary count (%d)", __func__, primaryCount);
                return false;
            }
            /* primary must be at (0,0) in client space */
            if (config[i].x != 0 || config[i].y != 0) {
                wlr_log(
                    WLR_ERROR, "%s: RDP client reported primary is not at (0,0) but (%d,%d).",
                    __func__, config[i].x, config[i].y
                );
                return false;
            }
        }
    }
    return true;
}

static void disp_start_monitor_layout_change(freerdp_peer *peer, rdpMonitor *config, UINT32 monitorCount) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)peer->context;

    wlr_log(WLR_INFO, "todo disp_start_monitor_layout_change");
}

static bool handle_adjust_monitor_layout(struct rdp_freerdp_peer *peer, int monitor_count, rdpMonitor *monitors) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)peer->context;

    if (!disp_monitor_sanity_check_layout(peer_ctx, monitors, monitor_count)) {
        return true;
    }
    disp_start_monitor_layout_change(peer, monitors, monitor_count);
    return true;
}

static void disp_monitor_layout_change_callback(bool free_only, void *dataIn) {
    struct disp_schedule_monitor_layout_change_data *data = wl_container_of(dataIn, data, _base);
    DispServerContext *context = data->context;
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;
    RDPGFX_RESET_GRAPHICS_PDU reset_graphics = {0};
    MONITOR_DEF *reset_monitor_def = NULL;

    if (free_only) {
        goto out;
    }

    /* Skip reset graphics on failure */
    if (!handle_adjust_monitor_layout(peer_ctx->peer, data->count, data->monitors)) {
        goto out;
    }

    reset_monitor_def = malloc(sizeof(MONITOR_DEF) * data->count);

    for (int i = 0; i < data->count; i++) {
        reset_monitor_def[i].left = data->monitors[i].x;
        reset_monitor_def[i].top = data->monitors[i].y;
        reset_monitor_def[i].right = data->monitors[i].width;
        reset_monitor_def[i].bottom = data->monitors[i].height;
        reset_monitor_def[i].flags = data->monitors[i].is_primary;
    }

    /* tell client the server updated the monitor layout */
    reset_graphics.width = peer_ctx->desktop_width;
    reset_graphics.height = peer_ctx->desktop_height;
    reset_graphics.monitorCount = data->count;
    reset_graphics.monitorDefArray = reset_monitor_def;
    peer_ctx->ctx_server_rdpgfx->ResetGraphics(peer_ctx->ctx_server_rdpgfx, &reset_graphics);

    /* force recreate all surface and redraw. */
    // wsland_id_manager_for_each(&client_ctx->window_id, disp_force_recreate_iter, NULL);
    /* todo weston_compositor_damage_all(b->compositor);*/
    out:
        free(reset_monitor_def);
    free(data);
    return;
}

static unsigned int disp_client_monitor_layout_change(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *display_control) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;
    struct rdp_settings *settings = peer_ctx->peer->context->settings;
    struct disp_schedule_monitor_layout_change_data *data;
    unsigned int i;

    wlr_log(WLR_DEBUG, "Client: DisplayLayoutChange: monitor count:0x%x", display_control->NumMonitors);
    assert(settings->HiDefRemoteApp);

    data = malloc(sizeof(*data) + (sizeof(rdpMonitor) * display_control->NumMonitors));

    data->context = context;
    data->monitors = (rdpMonitor*)(data + 1);
    data->count = display_control->NumMonitors;
    for (i = 0; i < display_control->NumMonitors; i++) {
        DISPLAY_CONTROL_MONITOR_LAYOUT *ml = &display_control->Monitors[i];

        data->monitors[i].x = ml->Left;
        data->monitors[i].y = ml->Top;
        data->monitors[i].width = ml->Width;
        data->monitors[i].height = ml->Height;
        data->monitors[i].is_primary = !!(ml->Flags & DISPLAY_CONTROL_MONITOR_PRIMARY);
        data->monitors[i].attributes.physicalWidth = ml->PhysicalWidth;
        data->monitors[i].attributes.physicalHeight = ml->PhysicalHeight;
        data->monitors[i].attributes.orientation = ml->Orientation;
        data->monitors[i].attributes.desktopScaleFactor = ml->DesktopScaleFactor;
        data->monitors[i].attributes.deviceScaleFactor = ml->DeviceScaleFactor;
        data->monitors[i].orig_screen = 0;
    }

    wsland_freerdp_dispatch_to_display_loop(peer_ctx, disp_monitor_layout_change_callback, &data->_base);
    return CHANNEL_RC_OK;
}

bool wsland_freerdp_ctx_server_disp_init(struct wsland_peer_context *peer_ctx) {
    DispServerContext *disp_ctx = disp_server_context_new(peer_ctx->vcm);
    if (!disp_ctx) {
        return false;
    }
    peer_ctx->ctx_server_disp = disp_ctx;

    disp_ctx->custom = peer_ctx;
    disp_ctx->MaxNumMonitors = RDP_MAX_MONITOR;
    disp_ctx->MaxMonitorAreaFactorA = DISPLAY_CONTROL_MAX_MONITOR_WIDTH;
    disp_ctx->MaxMonitorAreaFactorB = DISPLAY_CONTROL_MAX_MONITOR_HEIGHT;
    disp_ctx->DispMonitorLayout = disp_client_monitor_layout_change;
    if (disp_ctx->Open(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    if (disp_ctx->DisplayControlCaps(disp_ctx) != CHANNEL_RC_OK) {
        return false;
    }
    return true;
}