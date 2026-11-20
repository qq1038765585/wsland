#include "common/logger.h"
#include "backend.h"
#include "adapter.h"


static void when_client_monitor_change(bool free_only, void *user_data) {
    struct backend_task_data *data = wl_container_of(user_data, data, task);

    if (!free_only) {
        struct backend_client *client = data->client;
        struct wsland_adapter *adapter = client->backend->adapter;

        client->flags &= ~BACKEND_OUTPUT_ENABLED;
        BOOL success = adapter->handle->backend_adjust_monitor_layout(adapter, data->change_data.count, data->change_data.monitors);

        if (success) {
            MONITOR_DEF *reset_monitor = malloc(sizeof(MONITOR_DEF) * data->change_data.count);
            for (int idx = 0; idx < data->change_data.count; ++idx) {
                rdpMonitor* m = &data->change_data.monitors[idx];

                reset_monitor[idx].left = m->x;
                reset_monitor[idx].top = m->y;
                reset_monitor[idx].right = m->width;
                reset_monitor[idx].bottom = m->height;
                reset_monitor[idx].flags = m->is_primary;
            }

            RDPGFX_RESET_GRAPHICS_PDU reset_graphics = {0};
            reset_graphics.monitorCount = data->change_data.count;
            reset_graphics.monitorDefArray = reset_monitor;
            reset_graphics.height = adapter->area.height;
            reset_graphics.width = adapter->area.width;
            data->client->rdpgfx_ctx->ResetGraphics(data->client->rdpgfx_ctx, &reset_graphics);
            free(reset_monitor);
        }
        client->flags |= BACKEND_OUTPUT_ENABLED;
    }
    free(data);
}

static unsigned int disp_client_monitor_layout_change(DispServerContext *context, const DISPLAY_CONTROL_MONITOR_LAYOUT_PDU *monitor_change) {
    struct backend_client *client = context->custom;
    struct wsc_monitor_change_data *change_data = malloc(sizeof(*change_data) + sizeof(rdpMonitor) * monitor_change->NumMonitors);
    change_data->monitors = (rdpMonitor *)(change_data + 1);
    change_data->count = (int)monitor_change->NumMonitors;

    for (int idx = 0; idx < change_data->count; idx++) {
        DISPLAY_CONTROL_MONITOR_LAYOUT *monitor = &monitor_change->Monitors[idx];

        change_data->monitors[idx].x = monitor->Left;
        change_data->monitors[idx].y = monitor->Top;
        change_data->monitors[idx].width = (int)monitor->Width;
        change_data->monitors[idx].height = (int)monitor->Height;
        change_data->monitors[idx].is_primary = !!(monitor->Flags & DISPLAY_CONTROL_MONITOR_PRIMARY);
        change_data->monitors[idx].attributes.physicalWidth = monitor->PhysicalWidth;
        change_data->monitors[idx].attributes.physicalHeight = monitor->PhysicalHeight;
        change_data->monitors[idx].attributes.orientation = monitor->Orientation;
        change_data->monitors[idx].attributes.desktopScaleFactor = monitor->DesktopScaleFactor;
        change_data->monitors[idx].attributes.deviceScaleFactor = monitor->DeviceScaleFactor;
        change_data->monitors[idx].orig_screen = 0;
    }

    DISPATCH(client, change_data, change_data, when_client_monitor_change);
    return CHANNEL_RC_OK;
}

bool backend_client_disp_init(struct backend_client *client) {
    DispServerContext *disp_ctx = disp_server_context_new(client->vcm);
    if (!disp_ctx) {
        return false;
    }
    client->disp_ctx = disp_ctx;

    disp_ctx->custom = client;
    disp_ctx->MaxNumMonitors = BACKEND_MAX_MONITOR;
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