// ReSharper disable All
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/swapchain.h>

#include "wlr/interfaces/wlr_buffer.h"
#include "wsland/server.h"
#include "wsland/utils/log.h"

static const uint32_t SUPPORTED_OUTPUT_STATE = WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
    WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;

static size_t last_output_num = 0;

static wsland_output * wsland_output_from_output(struct wlr_output *wlr_output) {
    assert(wlr_output_is_wsland(wlr_output));

    wsland_output *output = wl_container_of(wlr_output, output, output);
    return output;
}

void output_update_refresh(wsland_output *output, int32_t refresh) {
    if (refresh <= 0) {
        refresh = WSLAND_DEFAULT_REFRESH;
    }

    output->frame_delay = 1000000 / refresh;
}

static bool output_test(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
    wsland_output *output = wsland_output_from_output(wlr_output);

    uint32_t unsupported = state->committed & ~SUPPORTED_OUTPUT_STATE;
    if (unsupported != 0) {
        wsland_log(SERVER, ERROR, "Unsupported output state fields: 0x%"PRIx32, unsupported);
        return false;
    }

    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        assert(state->mode_type == WLR_OUTPUT_STATE_MODE_CUSTOM);
    }

    if (state->committed & WLR_OUTPUT_STATE_LAYERS) {
        for (size_t i = 0; i < state->layers_len; i++) {
            state->layers[i].accepted = true;
        }
    }

    return true;
}

static bool output_commit(struct wlr_output *wlr_output, const struct wlr_output_state *state) {
    wsland_output *output = wsland_output_from_output(wlr_output);

    if (!output_test(wlr_output, state)) {
        return false;
    }

    if (state->committed & WLR_OUTPUT_STATE_MODE) {
        output_update_refresh(output, state->custom_mode.refresh);
    }

    bool enable = false;
    if (state->committed & WLR_OUTPUT_STATE_ENABLED) {
        enable = state->enabled;
    } else {
        enable = output->output.enabled;
    }

    if (enable) {
        wl_signal_emit(&state->buffer->events.destroy, state->buffer);
        wl_event_source_timer_update(output->frame_timer, output->frame_delay);
    }

    return true;
}

static bool output_set_cursor(struct wlr_output *wlr_output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
    wsland_output *output = wsland_output_from_output(wlr_output);

    output->server->wsland_cursor.buffer = buffer;
    output->server->wsland_cursor.b_hotspot_x = hotspot_x;
    output->server->wsland_cursor.b_hotspot_y = hotspot_y;
    output->server->wsland_cursor.dirty = true;

    wl_signal_emit(&output->server->events.wsland_cursor_frame, output->server);
    return true;
}

static bool output_move_cursor(struct wlr_output *wlr_output, int x, int y) {
    return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
    wsland_output *output = wsland_output_from_output(wlr_output);

    pixman_region32_fini(&output->pending_commit_damage);
    wlr_pointer_finish(&output->pointer);
    // wlr_output_destroy(wlr_output);

    wl_list_remove(&output->peer_link);
    wl_list_remove(&output->server_link);
    wl_event_source_remove(output->frame_timer);
    free(output);
}

static int signal_frame(void *data) {
    wsland_output *output = data;

    wlr_output_send_frame(&output->output);
    return 0;
}

static const struct wlr_output_impl wsland_output_impl = {
    .move_cursor = output_move_cursor,
    .set_cursor = output_set_cursor,
    .destroy = output_destroy,
    .commit = output_commit,
    .test = output_test,
};

bool wlr_output_is_wsland(struct wlr_output *wlr_output) {
    return wlr_output->impl == &wsland_output_impl;
}

wsland_output *wsland_output_create(wsland_server *server, int width, int height) {
    wsland_output *output = calloc(1, sizeof(*output));
    if (!output) {
        wsland_log(SERVER, ERROR, "failed to allocate wsland_output");
        return NULL;
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, width, height, WSLAND_DEFAULT_REFRESH);

    wlr_output_init(&output->output, server->backend, &wsland_output_impl, server->event_loop, &state);
    wlr_output_state_finish(&state);
    output->output.data = output;

    output_update_refresh(output, WSLAND_DEFAULT_REFRESH);

    size_t output_num = ++last_output_num;

    char name[64];
    snprintf(name, sizeof(name), "wsland-%zu", output_num);
    wlr_output_set_name(&output->output, name);

    char description[128];
    snprintf(description, sizeof(description), "Wsland output %zu", output_num);
    wlr_output_set_description(&output->output, description);

    output->frame_timer = wl_event_loop_add_timer(server->event_loop, signal_frame, output);

    wlr_pointer_init(&output->pointer, &wsland_pointer_impl, wsland_pointer_impl.name);
    output->pointer.output_name = strdup(output->output.name);

    pixman_region32_init(&output->pending_commit_damage);
    wl_signal_emit_mutable(&server->backend->events.new_output, output);
    wl_signal_emit_mutable(&server->backend->events.new_input, &output->pointer.base);
    return output;
}