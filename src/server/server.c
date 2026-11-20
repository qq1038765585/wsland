#include <unistd.h>
#include <sys/un.h>
#include <sys/socket.h>
#include <bits/socket.h>

#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_cursor.h>

#include <wlr/xwayland/server.h>
#include <wlr/xwayland/xwayland.h>

#include "common/config.h"
#include "common/logger.h"
#include "adapter.h"
#include "server.h"



struct wsland_server *wsland_server_create(struct wsland_config *config, struct wsland_adapter *adapter) {
    setenv("WLR_BACKENDS", "headless", true);
    setenv("WLR_HEADLESS_OUTPUTS", "0", true);

    struct wsland_server *server = calloc(1, sizeof(*server));
    if (!server) {
        server_log(ERROR, "calloc failed for wsland_server");
        goto create_failed;
    }

    server->display = wl_display_create();
    if (!server->display) {
        server_log(ERROR, "failed to invoke wl_display_create");
        goto create_failed;
    }

    server->dispatch = wl_display_get_event_loop(server->display);
    if (!server->dispatch) {
        server_log(ERROR, "failed to invoke wl_display_get_event_loop");
        goto create_failed;
    }

    server->backend = wlr_headless_backend_create(server->dispatch);
    if (!server->backend) {
        server_log(ERROR, "failed to invoke wlr_headless_backend_create");
        goto create_failed;
    }

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) {
        server_log(ERROR, "failed to invoke wlr_renderer_autocreate");
        goto create_failed;
    }

    if (!wlr_renderer_init_wl_display(server->renderer, server->display)) {
        server_log(ERROR, "failed to invoke wlr_renderer_init_wl_display");
        goto create_failed;
    }

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) {
        server_log(ERROR, "failed to invoke wlr_allocator_autocreate");
        goto create_failed;
    }

    server->compositor = wlr_compositor_create(server->display, 5, server->renderer);
    if (!server->compositor) {
       server_log(ERROR, "failed to invoke wlr_compositor_create");
        goto create_failed;
    }

    server->subcompositor = wlr_subcompositor_create(server->display);
    if (!server->subcompositor) {
        server_log(ERROR, "failed to invoke wlr_subcompositor_create");
        goto create_failed;
    }

    server->data_device_manager = wlr_data_device_manager_create(server->display);
    if (!server->data_device_manager) {
        server_log(ERROR, "failed to invoke wlr_data_device_manager_create");
        goto create_failed;
    }

    server->data_control_manager = wlr_data_control_manager_v1_create(server->display);
    if (!server->data_control_manager) {
        server_log(ERROR, "failed to invoke wlr_data_control_manager_v1_create");
        goto create_failed;
    }

    server->primary_selection_device_manager = wlr_primary_selection_v1_device_manager_create(server->display);
    if (!server->primary_selection_device_manager) {
        server_log(ERROR, "failed to invoke wlr_primary_selection_v1_device_manager_create");
        goto create_failed;
    }

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) {
        server_log(ERROR,  "failed to invoke wlr_output_layout_create");
        goto create_failed;
    }

    server->xdg_output_manager_v1 = wlr_xdg_output_manager_v1_create(server->display, server->output_layout);
    if (!server->xdg_output_manager_v1) {
        server_log(ERROR,  "failed to invoke wlr_xdg_output_manager_v1_create");
        goto create_failed;
    }

    server->output_manager_v1 = wlr_output_manager_v1_create(server->display);
    if (!server->output_manager_v1) {
        server_log(ERROR,  "failed to invoke wlr_output_manager_v1_create");
        goto create_failed;
    }

    server->scene = wlr_scene_create();
    if (!server->scene) {
        server_log(ERROR,  "failed to invoke wlr_scene_create");
        goto create_failed;
    }
    server->scene_bottom = wlr_scene_tree_create(&server->scene->tree);
    server->scene_apps = wlr_scene_tree_create(&server->scene->tree);
    server->scene_float = wlr_scene_tree_create(&server->scene->tree);
    server->scene_top = wlr_scene_tree_create(&server->scene->tree);
    server->scene_overlay = wlr_scene_tree_create(&server->scene->tree);
    server->scene_grab = wlr_scene_tree_create(&server->scene->tree);

    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_layout) {
        server_log(ERROR,  "failed to invoke wlr_scene_attach_output_layout");
        goto create_failed;
    }

    server->xdg_shell = wlr_xdg_shell_create(server->display, 3);
    if (!server->xdg_shell) {
        server_log(ERROR,  "failed to invoke wlr_xdg_shell_create");
        goto create_failed;
    }

    server->layer_shell = wlr_layer_shell_v1_create(server->display, 3);
    if (!server->layer_shell) {
        server_log(ERROR,  "failed to invoke wlr_layer_shell_v1_create");
        goto create_failed;
    }

    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
    if (!server->pointer_constraints) {
        server_log(ERROR,  "failed to invoke wlr_pointer_constraints_v1_create");
        goto create_failed;
    }

    server->relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server->display);
    if (!server->relative_pointer_manager) {
        server_log(ERROR,  "failed to invoke wlr_relative_pointer_manager_v1_create");
        goto create_failed;
    }

    server->cursor = wlr_cursor_create();
    if (!server->cursor) {
        server_log(ERROR,  "failed to invoke wlr_cursor_create");
        goto create_failed;
    }

    server->cursor_manager = wlr_xcursor_manager_create("default", 42);
    if (!server->cursor_manager) {
        server_log(ERROR,  "failed to invoke wlr_xcursor_manager_create");
        goto create_failed;
    }

    server->cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (!server->cursor_shape_manager) {
        server_log(ERROR,  "failed to invoke wlr_cursor_shape_manager_v1_create");
        goto create_failed;
    }

    server->seat = wlr_seat_create(server->display, "seat0");
    if (!server->seat) {
        server_log(ERROR,  "failed to invoke wlr_seat_create");
        goto create_failed;
    }

    server->viewporter = wlr_viewporter_create(server->display);
    if (!server->viewporter) {
        server_log(ERROR,  "failed to invoke wlr_viewporter_create");
        goto create_failed;
    }

    server->fractional_scale_manager = wlr_fractional_scale_manager_v1_create(server->display, 1);
    if (!server->fractional_scale_manager) {
        server_log(ERROR,  "failed to invoke wlr_fractional_scale_manager_v1_create");
        goto create_failed;
    }

    server->server_decoration_manager = wlr_server_decoration_manager_create(server->display);
    if (!server->server_decoration_manager) {
        server_log(ERROR,  "failed to invoke wlr_server_decoration_manager_create");
        goto create_failed;
    }

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    if (!server->xdg_decoration_manager) {
        server_log(ERROR,  "failed to invoke wlr_xdg_decoration_manager_v1_create");
        goto create_failed;
    }

    server->virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server->display);
    if (!server->virtual_pointer_manager) {
        server_log(ERROR,  "failed to invoke wlr_virtual_pointer_manager_v1_create");
        goto create_failed;
    }

    wsland_server_service_init(server);
    if (!server->service) {
        server_log(ERROR,  "failed to invoke wsland_server_service_init");
        goto create_failed;
    }

    {
        // backend event
        LISTEN(&server->backend->events.new_output, &server->events.backend_new_output, server->service->backend_new_output);
        LISTEN(&server->backend->events.new_input, &server->events.backend_new_input, server->service->backend_new_input);

        // cursor config
        server->grab.mode = WSLAND_CURSOR_PASSTHROUGH;
        wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

        // cursor event
        LISTEN(&server->cursor->events.axis, &server->events.cursor_axis, server->service->cursor_axis);
        LISTEN(&server->cursor->events.frame, &server->events.cursor_frame, server->service->cursor_frame);
        LISTEN(&server->cursor->events.button, &server->events.cursor_button, server->service->cursor_button);
        LISTEN(&server->cursor->events.motion, &server->events.cursor_motion, server->service->cursor_motion);
        LISTEN(&server->cursor->events.motion_absolute, &server->events.cursor_motion_absolute, server->service->cursor_motion_absolute);

        // cursor event
        LISTEN(&server->pointer_constraints->events.new_constraint, &server->events.new_constraint, server->service->pointer_new_constraint);
        LISTEN(&server->cursor_shape_manager->events.request_set_shape, &server->events.request_set_shape, server->service->cursor_set_shape);
        LISTEN(&server->virtual_pointer_manager->events.new_virtual_pointer, &server->events.new_virtual_pointer, server->service->virtual_new_pointer);

        // seat event
        LISTEN(&server->seat->events.request_set_cursor, &server->events.request_cursor, server->service->seat_set_cursor);
        LISTEN(&server->seat->events.set_selection, &server->events.backend_set_selection, server->service->set_backend_selection);
        LISTEN(&server->seat->events.request_set_selection, &server->events.request_set_selection, server->service->seat_set_selection);
        LISTEN(&server->seat->events.request_start_drag, &server->events.request_start_drag, server->service->seat_request_start_drag);
        dnd_event_init(server);

        // xdg shell event
        LISTEN(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->events.new_toplevel_decoration, server->service->new_toplevel_decoration);
        LISTEN(&server->server_decoration_manager->events.new_decoration, &server->events.new_server_decoration, server->service->new_server_decoration);
        wayland_event_init(server);
        layer_event_init(server);
    }

    if (config->socket && wl_display_add_socket(server->display, config->socket) == 0) {
        server->socket_name = config->socket;
    }
    if (!server->socket_name) {
        server->socket_name = wl_display_add_socket_auto(server->display);
    }
    if (!server->socket_name) {
        server_log(ERROR,  "failed to invoke wl_display_add_socket_auto");
        goto create_failed;
    }

    {
        server->xwayland = wlr_xwayland_create(server->display, server->compositor, true);
        if (!server->xwayland) {
            server_log(ERROR,  "failed to invoke wlr_xwayland_create");
            goto create_failed;
        }

        // xwayland event
        xwayland_event_init(server);
    }

    if (!wlr_backend_start(server->backend)) {
        server_log(ERROR,  "failed to invoke wlr_backend_start");
        goto create_failed;
    }

    wl_list_init(&server->outputs);
    wl_list_init(&server->windows);
    wl_list_init(&server->keyboards);
    server->adapter = adapter;
    adapter->server = server;
    return server;

create_failed:
    return NULL;
}

static void wslg_notify(void) {
    struct sockaddr_un addr = {0};

    char *socket_path = getenv("WSLGD_NOTIFY_SOCKET");
    if (!socket_path) {
        return;
    }

    int socket_fd = socket(PF_LOCAL, SOCK_SEQPACKET, 0);
    if (socket_fd < 0) {
        return;
    }

    addr.sun_family = AF_LOCAL;
    socklen_t name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path) + 1;
    socklen_t size = offsetof(struct sockaddr_un, sun_path) + name_size;

    int fd = connect(socket_fd, (struct sockaddr *)&addr, size);
    if (fd < 0) {
        goto close_socket_fd;
    }

    close(fd);

    close_socket_fd:
        close(socket_fd);
}

void wsland_server_running(struct wsland_server *server) {
    wslg_notify();

    setenv("DISPLAY", server->xwayland->display_name, true);
    setenv("WAYLAND_DISPLAY", server->socket_name, true);
    server_log(INFO, "running wayland compositor [ DISPLAY=%s, WAYLAND_DISPLAY=%s ]", server->xwayland->display_name, server->socket_name);
    wl_display_run(server->display);
}

void wsland_server_destroy(struct wsland_server *server) {
    if (server) {
        wl_display_destroy_clients(server->display);

        wl_list_remove(&server->events.layer_new_surface.link);
        wl_list_remove(&server->events.wayland_new_toplevel.link);
        wl_list_remove(&server->events.xwayland_new_toplevel.link);
        wl_list_remove(&server->events.xwayland_ready.link);

        wl_list_remove(&server->events.cursor_axis.link);
        wl_list_remove(&server->events.cursor_frame.link);
        wl_list_remove(&server->events.cursor_motion.link);
        wl_list_remove(&server->events.cursor_button.link);
        wl_list_remove(&server->events.cursor_motion_absolute.link);

        wl_list_remove(&server->events.backend_new_output.link);
        wl_list_remove(&server->events.backend_new_input.link);
        wl_list_remove(&server->events.request_cursor.link);
        wl_list_remove(&server->events.request_set_shape.link);
        wl_list_remove(&server->events.request_set_selection.link);
        wl_list_remove(&server->events.new_toplevel_decoration.link);
        wl_list_remove(&server->events.new_server_decoration.link);
        wl_list_remove(&server->events.new_virtual_pointer.link);

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

        if (server->dispatch) {
            wl_event_loop_destroy(server->dispatch);
        }

        if (server->display) {
            wl_display_terminate(server->display);
        }

        free(server);
    }
}