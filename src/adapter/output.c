#include <assert.h>

#include <wlr/interfaces/wlr_output.h>

#include "common/logger.h"
#include "backend.h"
#include "adapter.h"
#include "server.h"

static size_t last_output_num = 0;

static struct wsland_output * wsland_output_from_output(struct wlr_output *wlr_output) {
    assert(wlr_output_is_wsland(wlr_output));

    struct wsland_output *output = wl_container_of(wlr_output, output, output);
    return output;
}

static void output_update_refresh(struct wsland_output *output, int32_t refresh) {
    if (refresh <= 0) {
        refresh = WSLAND_DEFAULT_REFRESH;
    }

    output->frame_delay = 1000000 / refresh;
}

static bool output_test(struct wlr_output *wlr_output, const struct wlr_output_state *_) {
    struct wsland_output *output = wsland_output_from_output(wlr_output);
    struct wsland_adapter *adapter = output->server->adapter;

    return adapter->backend->client != NULL;
}

static bool output_commit(struct wlr_output *wlr_output, const struct wlr_output_state *_) {
    struct wsland_output *output = wsland_output_from_output(wlr_output);
    struct wsland_adapter *adapter = output->server->adapter;

    if (!output_test(wlr_output, _)) {
        return false;
    }

    if (adapter->backend->client->flags & BACKEND_OUTPUT_ENABLED) {
        adapter->handle->backend_output_frame(adapter);
    }

    wl_event_source_timer_update(output->frame_timer, output->frame_delay);
    return true;
}

static bool output_set_cursor(struct wlr_output *wlr_output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
    struct wsland_output *output = wsland_output_from_output(wlr_output);
    struct wsland_adapter *adapter = output->server->adapter;

    adapter->handle->backend_cursor_update(output, buffer, hotspot_x, hotspot_y);
    return true;
}

static bool output_move_cursor(struct wlr_output *wlr_output, int x, int y) {
    return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
    struct wsland_output *output = wsland_output_from_output(wlr_output);

    wl_event_source_remove(output->frame_timer);
    wlr_output_finish(&output->output);
    free(output);
}

static int signal_frame(void *data) {
    struct wsland_output *output = data;

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

void wsland_output_reset(void) {
    last_output_num = 0;
}

bool wlr_output_is_wsland(struct wlr_output *wlr_output) {
    return wlr_output->impl == &wsland_output_impl;
}

struct wsland_output *wsland_output_create(struct wsland_server *server, struct wlr_box output_area) {
    struct wsland_output *output = calloc(1, sizeof(*output));
    if (!output) {
        server_log(ERROR, "failed to allocate wsland_output");
        return NULL;
    }
    output->server = server;
    output->full_area = output_area;

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state, output_area.width, output_area.height, WSLAND_DEFAULT_REFRESH);
    output_update_refresh(output, WSLAND_DEFAULT_REFRESH);

    wlr_output_init(&output->output, server->backend, &wsland_output_impl, server->dispatch, &state);
    wlr_output_state_finish(&state);
    output->output.data = output;

    size_t output_num = ++last_output_num;

    char name[64];
    snprintf(name, sizeof(name), "wsland-%zu", output_num);
    wlr_output_set_name(&output->output, name);

    char description[128];
    snprintf(description, sizeof(description), "Wsland output %zu", output_num);
    wlr_output_set_description(&output->output, description);

    output->frame_timer = wl_event_loop_add_timer(server->dispatch, signal_frame, output);
    wl_signal_emit_mutable(&server->backend->events.new_output, output);
    return output;
}