#include <assert.h>
#include <stdlib.h>

#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>

#include "wsland/server.h"
#include "wsland/utils/log.h"

const struct wlr_pointer_impl wsland_pointer_impl = {
    .name = "wsland-pointer",
};

const struct wlr_keyboard_impl wsland_keyboard_impl = {
    .name = "wsland-keyboard",
};

wsland_server *wsland_server_create(wsland_config *config) {
    wsland_server *server = calloc(1, sizeof(*server));
    if (!server) {
        wsland_log(SERVER, ERROR, "calloc failed for wsland_server");
        goto create_failed;
    }

    server->config = config;
    server->display = wl_display_create();
    if (!server->display) {
        wsland_log(SERVER, ERROR, "failed to invoke wl_display_create");
        goto create_failed;
    }

    server->event_loop = wl_display_get_event_loop(server->display);
    if (!server->event_loop) {
        wsland_log(SERVER, ERROR, "failed to invoke wl_display_get_event_loop");
        goto create_failed;
    }

    setenv("WLR_BACKENDS", "headless", true);
    setenv("WLR_HEADLESS_OUTPUTS", "0", true);
    server->backend = wlr_headless_backend_create(server->event_loop);
    if (!server->backend) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_headless_backend_create");
        goto create_failed;
    }

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_renderer_autocreate");
        goto create_failed;
    }

    if (!wlr_renderer_init_wl_display(server->renderer, server->display)) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_renderer_init_wl_display");
        goto create_failed;
    }

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_allocator_autocreate");
        goto create_failed;
    }

    server->compositor = wlr_compositor_create(server->display, 5, server->renderer);
    if (!server->compositor) {
       wsland_log(SERVER, ERROR, "failed to invoke wlr_compositor_create");
        goto create_failed;
    }

    server->subcompositor = wlr_subcompositor_create(server->display);
    if (!server->subcompositor) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_subcompositor_create");
        goto create_failed;
    }

    server->data_device_manager = wlr_data_device_manager_create(server->display);
    if (!server->data_device_manager) {
        wsland_log(SERVER, ERROR, "failed to invoke wlr_data_device_manager_create");
        goto create_failed;
    }

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_output_layout_create");
        goto create_failed;
    }

    server->scene = wlr_scene_create();
    if (!server->scene) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_scene_create");
        goto create_failed;
    }

    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_layout) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_scene_attach_output_layout");
        goto create_failed;
    }

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_xdg_shell_create");
        goto create_failed;
    }

    server->cursor = wlr_cursor_create();
    if (!server->cursor) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_cursor_create");
        goto create_failed;
    }

    server->cursor_manager = wlr_xcursor_manager_create("default", 42);
    if (!server->cursor_manager) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_xcursor_manager_create");
        goto create_failed;
    }

    server->seat = wlr_seat_create(server->display, "seat0");
    if (!server->seat) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_seat_create");
        goto create_failed;
    }

    server->viewporter = wlr_viewporter_create(server->display);
    if (!server->viewporter) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_viewporter_create");
        goto create_failed;
    }

    server->server_decoration_manager = wlr_server_decoration_manager_create(server->display);
    if (!server->server_decoration_manager) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_server_decoration_manager_create");
        goto create_failed;
    }

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    if (!server->xdg_decoration_manager) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_xdg_decoration_manager_v1_create");
        goto create_failed;
    }

    server->handle = wsland_server_handle_init(server);
    if (!server->handle) {
        wsland_log(SERVER, ERROR, "failed to invoke wsland_server_handle_init");
        goto create_failed;
    }

    {
        // backend event
        server->events.new_output.notify = server->handle->server_new_output;
        wl_signal_add(&server->backend->events.new_output, &server->events.new_output);

        server->events.new_input.notify = server->handle->server_new_input;
        wl_signal_add(&server->backend->events.new_input, &server->events.new_input);

        server->events.new_surface.notify = server->handle->server_new_surface;
        wl_signal_add(&server->compositor->events.new_surface, &server->events.new_surface);

        // xdg shell event
        server->events.new_xdg_toplevel.notify = server->handle->server_new_xdg_toplevel;
        wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->events.new_xdg_toplevel);
        server->events.new_xdg_popup.notify = server->handle->server_new_xdg_popup;
        wl_signal_add(&server->xdg_shell->events.new_popup, &server->events.new_xdg_popup);

        // cursor event
        wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

        server->move.mode = WSLAND_CURSOR_PASSTHROUGH;
        server->events.cursor_motion.notify = server->handle->server_cursor_motion;
        wl_signal_add(&server->cursor->events.motion, &server->events.cursor_motion);
        server->events.cursor_motion_absolute.notify = server->handle->server_cursor_motion_absolute;
        wl_signal_add(&server->cursor->events.motion_absolute, &server->events.cursor_motion_absolute);
        server->events.cursor_button.notify = server->handle->server_cursor_button;
        wl_signal_add(&server->cursor->events.button, &server->events.cursor_button);
        server->events.cursor_axis.notify = server->handle->server_cursor_axis;
        wl_signal_add(&server->cursor->events.axis, &server->events.cursor_axis);
        server->events.cursor_frame.notify = server->handle->server_cursor_frame;
        wl_signal_add(&server->cursor->events.frame, &server->events.cursor_frame);

        // seat event
        server->events.request_cursor.notify = server->handle->seat_request_cursor;
        wl_signal_add(&server->seat->events.request_set_cursor, &server->events.request_cursor);
        server->events.request_set_selection.notify = server->handle->seat_request_set_selection;
        wl_signal_add(&server->seat->events.request_set_selection, &server->events.request_set_selection);
    }

    server->socket_name = wl_display_add_socket_auto(server->display);
    if (!server->socket_name) {
        wsland_log(SERVER, ERROR,  "failed to invoke wl_display_add_socket_auto");
        goto create_failed;
    }

    if (!wlr_backend_start(server->backend)) {
        wsland_log(SERVER, ERROR,  "failed to invoke wlr_backend_start");
        goto create_failed;
    }

    wl_list_init(&server->outputs);
    wl_list_init(&server->keyboards);
    wl_list_init(&server->toplevels);
    wl_signal_init(&server->events.wsland_surface_commit);
    wl_signal_init(&server->events.wsland_window_create);
    wl_signal_init(&server->events.wsland_window_commit);
    wl_signal_init(&server->events.wsland_window_destroy);

    wl_signal_init(&server->events.wsland_cursor_frame);
    wl_signal_init(&server->events.wsland_output_frame);
    return server;
create_failed:
    return NULL;
}

void wsland_server_running(wsland_server *server) {
    setenv("WAYLAND_DISPLAY", server->socket_name, true);
    wsland_log(SERVER, INFO, "running wayland compositor [ WAYLAND_DISPLAY=%s ]", server->socket_name);
    wl_display_run(server->display);
}

void wsland_server_destroy(wsland_server *server) {
    if (server) {
        assert(wl_list_empty(&server->events.wsland_window_create.listener_list));
        assert(wl_list_empty(&server->events.wsland_window_commit.listener_list));
        assert(wl_list_empty(&server->events.wsland_window_destroy.listener_list));

        assert(wl_list_empty(&server->events.wsland_cursor_frame.listener_list));
        assert(wl_list_empty(&server->events.wsland_output_frame.listener_list));

        wl_display_destroy_clients(server->display);

        wl_list_remove(&server->events.new_surface.link);
        wl_list_remove(&server->events.new_xdg_toplevel.link);
        wl_list_remove(&server->events.new_xdg_popup.link);

        wl_list_remove(&server->events.cursor_motion.link);
        wl_list_remove(&server->events.cursor_motion_absolute.link);
        wl_list_remove(&server->events.cursor_button.link);
        wl_list_remove(&server->events.cursor_axis.link);
        wl_list_remove(&server->events.cursor_frame.link);

        wl_list_remove(&server->events.new_input.link);
        wl_list_remove(&server->events.request_cursor.link);
        wl_list_remove(&server->events.request_set_selection.link);

        wl_list_remove(&server->events.new_output.link);

        wlr_scene_node_destroy(&server->scene->tree.node);
        wlr_xcursor_manager_destroy(server->cursor_manager);
        wlr_cursor_destroy(server->cursor);

        if (server->allocator) {
            wlr_allocator_destroy(server->allocator);
        }

        if (server->renderer) {
            wlr_renderer_destroy(server->renderer);
        }

        if (server->backend) {
            wlr_backend_destroy(server->backend);
        }

        if (server->event_loop) {
            wl_event_loop_destroy(server->event_loop);
        }

        if (server->display) {
            wl_display_terminate(server->display);
        }

        free(server);
    }
}