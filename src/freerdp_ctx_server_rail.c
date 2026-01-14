// ReSharper disable All
#define _GNU_SOURCE 1

#include <unistd.h>
#include <assert.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/interfaces/wlr_output.h>

#include "wsland.h"

static bool is_window_shadow_remoting_disabled(struct wsland_peer_context *client_ctx) {
    /* When shadow is not remoted, window geometry must be able to queried from shell to clip
       shadow area, and resize margin must be supported by client. When remoting window shadow,
       the shadow area is used as resize margin, but without it, window can't be resizable,
       thus window margin must be added by client side. */
    return (
        !server.freerdp.enable_window_shadow_remoting && server.freerdp.freerdp_shell_api
        && server.freerdp.freerdp_shell_api->get_window_geometry
        && (client_ctx->client_status_flags & TS_RAIL_CLIENTSTATUS_WINDOW_RESIZE_MARGIN_SUPPORTED)
    );
}

static UINT rail_client_handshake(RailServerContext *context, const RAIL_HANDSHAKE_ORDER *handshake) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;
    wlr_log(WLR_DEBUG, "freerdp rail client handshake build-number:%d", handshake->buildNumber);

    peer_ctx->handshake_completed = TRUE;
    return CHANNEL_RC_OK;
}

static UINT rail_client_status(RailServerContext *context, const RAIL_CLIENT_STATUS_ORDER *client_status) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;

    wlr_log(WLR_DEBUG, "Client: ClientStatus:0x%x", client_status->flags);
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_ALLOWLOCALMOVESIZE) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_ALLOWLOCALMOVESIZE");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_AUTORECONNECT) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_AUTORECONNECT");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_ZORDER_SYNC) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_ZORDER_SYNC");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_WINDOW_RESIZE_MARGIN_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_WINDOW_RESIZE_MARGIN_SUPPORTED");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_HIGH_DPI_ICONS_SUPPORTED");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_APPBAR_REMOTING_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_APPBAR_REMOTING_SUPPORTED");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_POWER_DISPLAY_REQUEST_SUPPORTED");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED");
    }
    if (client_status->flags & TS_RAIL_CLIENTSTATUS_BIDIRECTIONAL_CLOAK_SUPPORTED) {
        wlr_log(WLR_DEBUG, "     - TS_RAIL_CLIENTSTATUS_BIDIRECTIONAL_CLOAK_SUPPORTED");
    }

    peer_ctx->client_status_flags = client_status->flags;
    return CHANNEL_RC_OK;
}

static void wsland_freerdp_rail_client_exec_destroy(struct wl_listener *listener, void *data) {
    struct wsland_peer_context *client_ctx = wl_container_of(listener, client_ctx, client_exec_destroy_listener);
    wlr_log(WLR_DEBUG, "Client ExecOrder program terminated");

    wl_list_remove(&client_ctx->client_exec_destroy_listener.link);
    client_ctx->client_exec_destroy_listener.notify = NULL;
    client_ctx->client_exec = NULL;
}

static void rail_client_exec_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_EXEC_ORDER *exec = &data->exec;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    UINT result = RAIL_EXEC_E_FAIL;
    RAIL_EXEC_RESULT_ORDER orderResult = {0};
    char *remoteProgramAndArgs = exec->RemoteApplicationProgram;

    wlr_log(
        WLR_DEBUG, "Client ExecOrder:0x%08X, Program:%s, WorkingDir:%s, RemoteApplicationArguments:%s",
        (UINT)exec->flags,
        exec->RemoteApplicationProgram,
        exec->RemoteApplicationWorkingDir,
        exec->RemoteApplicationArguments
    );

    if (!free_only && exec->RemoteApplicationProgram) {
        if (!utf8_string_to_rail_string(exec->RemoteApplicationProgram, &orderResult.exeOrFile)) {
            goto send_result;
        }

        if (exec->RemoteApplicationArguments) {
            /* construct remote program path and arguments */
            remoteProgramAndArgs = malloc(strlen(exec->RemoteApplicationProgram) + strlen(exec->RemoteApplicationArguments) + 2); /* space between program and args + null terminate. */
            if (!remoteProgramAndArgs) {
                goto send_result;
            }
            sprintf(remoteProgramAndArgs, "%s %s", exec->RemoteApplicationProgram, exec->RemoteApplicationArguments);
        }

        /* TODO: server state machine, wait until activation complated */
        while (!peer_ctx->activation_rail_completed) {
            usleep(10000);
        }

        /* launch the process specified by RDP client. */
        wlr_log(WLR_DEBUG, "Client ExecOrder launching %s", remoteProgramAndArgs);
        if (api && api->request_launch_shell_process) {
            peer_ctx->client_exec = api->request_launch_shell_process(
                server.freerdp.rail_shell_context, remoteProgramAndArgs
            );
        }
        if (peer_ctx->client_exec) {
            assert(!peer_ctx->client_exec_destroy_listener.notify);
            peer_ctx->client_exec_destroy_listener.notify = wsland_freerdp_rail_client_exec_destroy;
            // wl_client_add_destroy_listener(peer_ctx->client_exec, &peer_ctx->client_exec_destroy_listener);
            result = RAIL_EXEC_S_OK;
        }
        else {
            wlr_log(WLR_ERROR, "%s: fail to launch shell process %s", __func__, remoteProgramAndArgs);
        }
    }

send_result:
    if (!free_only) {
        orderResult.flags = exec->flags;
        orderResult.execResult = result;
        orderResult.rawResult = 0;
        peer_ctx->ctx_server_rail->ServerExecResult(peer_ctx->ctx_server_rail, &orderResult);
    }

    free(orderResult.exeOrFile.string);
    if (remoteProgramAndArgs != exec->RemoteApplicationProgram) {
        free(remoteProgramAndArgs);
    }
    free(exec->RemoteApplicationProgram);
    free(exec->RemoteApplicationWorkingDir);
    free(exec->RemoteApplicationArguments);
    free(data);
}

static UINT rail_client_exec(RailServerContext *context, const RAIL_EXEC_ORDER *arg) {
    RAIL_EXEC_ORDER exec_order = {0};

    exec_order.flags = arg->flags;
    if (arg->RemoteApplicationProgram) {
        exec_order.RemoteApplicationProgram = malloc(strlen(arg->RemoteApplicationProgram) + 1);
        strcpy(
            exec_order.RemoteApplicationProgram,
            arg->RemoteApplicationProgram
        );
    }
    if (arg->RemoteApplicationWorkingDir) {
        exec_order.RemoteApplicationWorkingDir = malloc(strlen(arg->RemoteApplicationWorkingDir) + 1);
        strcpy(
            exec_order.RemoteApplicationWorkingDir,
            arg->RemoteApplicationWorkingDir
        );
    }
    if (arg->RemoteApplicationArguments) {
        exec_order.RemoteApplicationArguments = malloc(strlen(arg->RemoteApplicationArguments) + 1);
        strcpy(
            exec_order.RemoteApplicationArguments,
            arg->RemoteApplicationArguments
        );
    }
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, exec, &exec_order, rail_client_exec_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_activate_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_ACTIVATE_ORDER *activate = &data->activate;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    struct wsland_toplevel *surface = NULL;

    wlr_log(WLR_DEBUG, "Client: ClientActivate: WindowId:0x%x, enabled:%d", activate->windowId, activate->enabled);

    if (!free_only) {
        if (activate->windowId) {
            surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, activate->windowId);

            if (!surface) {
                wlr_log(WLR_ERROR, "Client: ClientActivate: WindowId:0x%x is not found.", activate->windowId);
            } else {
                wlr_xdg_toplevel_set_activated(surface->xdg_toplevel, activate->enabled);
            }
        }
    }
    /*if (!free_only && api && api->request_window_activate && server.freerdp.rail_shell_context) {
        if (activate->windowId && activate->enabled) {
            surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, activate->windowId);

            if (!surface) {
                wlr_log(WLR_ERROR, "Client: ClientActivate: WindowId:0x%x is not found.", activate->windowId);
            }
        }
        api->request_window_activate(
            server.freerdp.rail_shell_context, server.seat, surface->xdg_toplevel->base->surface
        );
    }*/
    free(data);
}

static UINT rail_client_activate(RailServerContext *context, const RAIL_ACTIVATE_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, activate, arg, rail_client_activate_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_syscommand_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_SYSCOMMAND_ORDER *syscommand = &data->sys_command;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    struct wsland_toplevel *surface;

    surface = NULL;
    if (!free_only) {
        surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, syscommand->windowId);
    }
    if (surface) {
        char *commandString = NULL;
        switch (syscommand->command) {
        case SC_SIZE:
            commandString = "SC_SIZE";
            break;
        case SC_MOVE:
            commandString = "SC_MOVE";
            break;
        case SC_MINIMIZE:
            commandString = "SC_MINIMIZE";
            if (api && api->request_window_minimize) {
                api->request_window_minimize(surface->xdg_toplevel->base->surface);
            }
            break;
        case SC_MAXIMIZE:
            commandString = "SC_MAXIMIZE";
            if (api && api->request_window_maximize) {
                api->request_window_maximize(surface->xdg_toplevel->base->surface);
            }
            break;
        case SC_CLOSE:
            commandString = "SC_CLOSE";
            if (api && api->request_window_close) {
                api->request_window_close(surface->xdg_toplevel->base->surface);
            }
            break;
        case SC_KEYMENU:
            commandString = "SC_KEYMENU";
            break;
        case SC_RESTORE:
            commandString = "SC_RESTORE";
            if (api && api->request_window_restore) {
                api->request_window_restore(surface->xdg_toplevel->base->surface);
            }
            break;
        case SC_DEFAULT:
            commandString = "SC_DEFAULT";
            break;
        default:
            commandString = "Unknown";
            break;
        }

        wlr_log(
            WLR_DEBUG,
            "Client: ClientSyscommand: WindowId:0x%x, surface:0x%p, command:%s (0x%x)",
            syscommand->windowId, (void *)surface, commandString,
            syscommand->command
        );

    Exit:
        free(data);
        return;
    }
    wlr_log(
        WLR_ERROR,
        "Client: ClientSyscommand: WindowId:0x%x is not found.",
        syscommand->windowId
    );
    goto Exit;
}

static UINT rail_client_syscommand(RailServerContext *context, const RAIL_SYSCOMMAND_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_command, arg, rail_client_syscommand_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_sysmenu_callback(bool freeOnly, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_SYSMENU_ORDER *sysmenu = &data->sys_menu;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;

    wlr_log(
        WLR_DEBUG,
        "Client: ClientSyscommand: WindowId:0x%x, left:%d, top:%d",
        sysmenu->windowId, sysmenu->left, sysmenu->top
    );

    free(data);
}

static UINT rail_client_sysmenu(RailServerContext *context, const RAIL_SYSMENU_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_menu, arg, rail_client_sysmenu_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_sysparam_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_SYSPARAM_ORDER *sysparam = &data->sys_param;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;

    if (sysparam->params & SPI_MASK_SET_DRAG_FULL_WINDOWS) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: dragFullWindows:%d", sysparam->dragFullWindows);
    }

    if (sysparam->params & SPI_MASK_SET_KEYBOARD_CUES) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: keyboardCues:%d", sysparam->keyboardCues);
    }

    if (sysparam->params & SPI_MASK_SET_KEYBOARD_PREF) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: keyboardPref:%d", sysparam->keyboardPref);
    }

    if (sysparam->params & SPI_MASK_SET_MOUSE_BUTTON_SWAP) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: mouseButtonSwap:%d", sysparam->mouseButtonSwap);
        peer_ctx->mouse_button_swap = sysparam->mouseButtonSwap;
    }

    if (sysparam->params & SPI_MASK_SET_WORK_AREA) {
        wlr_log(
            WLR_DEBUG, "Client: ClientSysparam: workArea:(left:%u, top:%u, right:%u, bottom:%u)",
            sysparam->workArea.left,
            sysparam->workArea.top,
            sysparam->workArea.right,
            sysparam->workArea.bottom
        );
    }

    if (sysparam->params & SPI_MASK_DISPLAY_CHANGE) {
        wlr_log(
            WLR_DEBUG, "Client: ClientSysparam: displayChange:(left:%u, top:%u, right:%u, bottom:%u)",
            sysparam->displayChange.left,
            sysparam->displayChange.top,
            sysparam->displayChange.right,
            sysparam->displayChange.bottom
        );
    }

    if (sysparam->params & SPI_MASK_TASKBAR_POS) {
        wlr_log(
            WLR_DEBUG, "Client: ClientSysparam: taskbarPos:(left:%u, top:%u, right:%u, bottom:%u)",
            sysparam->taskbarPos.left,
            sysparam->taskbarPos.top,
            sysparam->taskbarPos.right,
            sysparam->taskbarPos.bottom
        );
    }

    if (sysparam->params & SPI_MASK_SET_HIGH_CONTRAST) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: highContrast");
    }

    if (sysparam->params & SPI_MASK_SET_CARET_WIDTH) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: caretWidth:%d", sysparam->caretWidth);
    }

    if (sysparam->params & SPI_MASK_SET_STICKY_KEYS) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: stickyKeys:%d", sysparam->stickyKeys);
    }

    if (sysparam->params & SPI_MASK_SET_TOGGLE_KEYS) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: toggleKeys:%d", sysparam->toggleKeys);
    }

    if (sysparam->params & SPI_MASK_SET_FILTER_KEYS) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: filterKeys");
    }

    if (sysparam->params & SPI_MASK_SET_SCREEN_SAVE_ACTIVE) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: setScreenSaveActive:%d", sysparam->setScreenSaveActive);
    }

    if (sysparam->params & SPI_MASK_SET_SET_SCREEN_SAVE_SECURE) {
        wlr_log(WLR_DEBUG, "Client: ClientSysparam: setScreenSaveSecure:%d", sysparam->setScreenSaveSecure);
    }

    if (!free_only) {
        if (sysparam->params & SPI_MASK_SET_WORK_AREA) {
            RECTANGLE_16 area = sysparam->workArea;

            struct wlr_output *wlr_output = wlr_output_layout_output_at(
                server.output_layout, area.left, area.top
            );

            if (wlr_output) {
                struct wlr_output_layout_output *layout_output = wlr_output_layout_get(
                    server.output_layout, wlr_output
                );

                wlr_log(
                    WLR_DEBUG, "Translated workarea:(%d,%d)-(%d,%d) at %s:(%d,%d)-(%d,%d)",
                    area.left, area.top, area.right, area.bottom,
                    wlr_output->name,
                    layout_output->x, layout_output->y,
                    layout_output->x + wlr_output->width,
                    layout_output->y + wlr_output->height
                );

                struct wsland_output *output = wlr_output->data;
                output->work_box = (struct wlr_box){
                    area.left, area.top, area.right - area.left, area.bottom - area.top
                };
            }
            else {
                wlr_log(WLR_ERROR, "Client: ClientSysparam: workArea isn't belonging to an output");
            }
        }
    }

    free(data);
}

static UINT rail_rail_client_sysparam(RailServerContext *context, const RAIL_SYSPARAM_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, sys_param, arg, rail_client_sysparam_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_get_appid_req_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_GET_APPID_REQ_ORDER *getAppidReq = &data->get_appid_req;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    char appId[520] = {0};
    char imageName[520] = {0};
    pid_t pid;
    size_t i;
    unsigned short *p;
    struct wsland_toplevel *surface;

    wlr_log(WLR_DEBUG, "Client: ClientGetAppidReq: WindowId:0x%x", getAppidReq->windowId);

    if (!free_only && api && api->get_window_app_id) {
        surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, getAppidReq->windowId);
        if (!surface) {
            wlr_log(WLR_ERROR, "Client: ClientGetAppidReq: WindowId:0x%x is not found", getAppidReq->windowId);
            goto Exit;
        }

        pid = api->get_window_app_id(
            server.freerdp.rail_shell_context, surface->xdg_toplevel->base->surface,
            &appId[0], sizeof(appId), &imageName[0], sizeof(imageName)
        );
        if (appId[0] == '\0') {
            wlr_log(WLR_ERROR, "Client: ClientGetAppidReq: WindowId:0x%x does not have appId, or not top level window", getAppidReq->windowId);
            goto Exit;
        }

        wlr_log(WLR_DEBUG, "Client: ClientGetAppidReq: pid:%d appId:%s WindowId:0x%x", (uint32_t)pid, appId, getAppidReq->windowId);
        wlr_log(WLR_DEBUG, "Client: ClientGetAppidReq: pid:%d imageName:%s", (uint32_t)pid, imageName);

        /* Reply with RAIL_GET_APPID_RESP_EX when pid/imageName is valid and client supports it */
        if ((pid >= 0) && (imageName[0] != '\0') &&
            peer_ctx->client_status_flags & TS_RAIL_CLIENTSTATUS_GET_APPID_RESPONSE_EX_SUPPORTED) {
            RAIL_GET_APPID_RESP_EX getAppIdRespEx = {0};

            getAppIdRespEx.windowID = getAppidReq->windowId;
            for (i = 0, p = &getAppIdRespEx.applicationID[0]; i < strlen(appId); i++, p++) {
                *p = (unsigned short)appId[i];
            }
            getAppIdRespEx.processId = (uint32_t)pid;
            for (i = 0, p = &getAppIdRespEx.processImageName[0]; i < strlen(imageName); i++, p++) {
                *p = (unsigned short)imageName[i];
            }
            peer_ctx->ctx_server_rail->ServerGetAppidRespEx(peer_ctx->ctx_server_rail, &getAppIdRespEx);
        }
        else {
            RAIL_GET_APPID_RESP_ORDER getAppIdResp = {0};

            getAppIdResp.windowId = getAppidReq->windowId;
            for (i = 0, p = &getAppIdResp.applicationId[0]; i < strlen(appId); i++, p++) {
                *p = (unsigned short)appId[i];
            }
            peer_ctx->ctx_server_rail->ServerGetAppidResp(peer_ctx->ctx_server_rail, &getAppIdResp);
        }
    }

Exit:
    free(data);
}

static UINT rail_client_get_appid_req(RailServerContext *context, const RAIL_GET_APPID_REQ_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, get_appid_req, arg, rail_client_get_appid_req_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_window_move_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_WINDOW_MOVE_ORDER *windowMove = &data->window_move;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    struct wsland_toplevel *wlr_surface;
    pixman_rectangle32_t windowMoveRect;
    struct wlr_box geometry;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;

    wlr_log(
        WLR_DEBUG, "Client: WindowMove: WindowId:0x%x at (%d,%d) %dx%d",
        windowMove->windowId,
        windowMove->left,
        windowMove->top,
        windowMove->right - windowMove->left,
        windowMove->bottom - windowMove->top
    );

    wlr_surface = NULL;
    if (!free_only) {
        wlr_surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, windowMove->windowId);
    }

    if (wlr_surface) {
        struct wsland_surface *wsland_surface = wlr_surface->xdg_toplevel->base->surface->data;
        if (api && api->request_window_move) {
            windowMoveRect.x = windowMove->left;
            windowMoveRect.y = windowMove->top;
            windowMoveRect.width = windowMove->right - windowMove->left;
            windowMoveRect.height = windowMove->bottom - windowMove->top;
            if (!wsland_surface->is_window_snapped) {
                /* WindowMove PDU include window resize margin */
                /* [MS-RDPERP] - v20200304 - 3.2.5.1.6 Processing Window Information Orders
                    However, the Client Window Move PDU (section 2.2.2.7.4) and Client Window Snap PDU
                    (section 2.2.2.7.5) do include resize margins in the window boundaries. */
                windowMoveRect.x += wsland_surface->window_margin_left;
                windowMoveRect.y += wsland_surface->window_margin_top;
                windowMoveRect.width -= wsland_surface->window_margin_left + wsland_surface->window_margin_right;
                windowMoveRect.height -= wsland_surface->window_margin_top + wsland_surface->window_margin_bottom;
            }


            /* todo to_weston_coordinate(client_ctx,
                         &windowMoveRect.x,
                         &windowMoveRect.y,
                         &windowMoveRect.width,
                         &windowMoveRect.height);*/
            if (is_window_shadow_remoting_disabled(peer_ctx) || wsland_surface->is_window_snapped) {
                /* offset window shadow area */
                /* window_geometry here is last commited geometry */
                api->get_window_geometry(wlr_surface->xdg_toplevel->base->surface, &geometry);
                windowMoveRect.x -= geometry.x;
                windowMoveRect.y -= geometry.y;
                windowMoveRect.width += (wlr_surface->xdg_toplevel->current.width - geometry.width);
                windowMoveRect.height += (wlr_surface->xdg_toplevel->current.height - geometry.height);
            }
            api->request_window_move(
                wlr_surface->xdg_toplevel->base->surface, windowMoveRect.x, windowMoveRect.y, windowMoveRect.width, windowMoveRect.height
            );
            wsland_surface->force_update_window_state = true;

            struct wlr_output *wlr_output = wlr_output_layout_output_at(
                server.output_layout, windowMoveRect.x, windowMoveRect.y
            );
            wlr_output_update_needs_frame(wlr_output);
        }
    }

    free(data);
}

static UINT rail_client_window_move(RailServerContext *context, const RAIL_WINDOW_MOVE_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, window_move, arg, rail_client_window_move_callback);
    return CHANNEL_RC_OK;
}

static void rail_client_snap_arrange_callback(bool freeOnly, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_SNAP_ARRANGE *snap = &data->snap_arrange;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    struct wsland_toplevel *surface;
    pixman_rectangle32_t snap_rect;
    struct wlr_box geometry;

    wlr_log(
        WLR_DEBUG, "Client: SnapArrange: WindowId:0x%x at (%d, %d) %dx%d",
        snap->windowId,
        snap->left,
        snap->top,
        snap->right - snap->left,
        snap->bottom - snap->top
    );

    surface = NULL;
    if (!freeOnly) {
        surface = wsland_freerdp_instance_loop_wsland_surface(peer_ctx, snap->windowId);
    }

    if (surface) {
        struct wsland_surface *wsland_surface = surface->xdg_toplevel->base->surface->data;
        if (api && api->request_window_snap) {
            snap_rect.x = snap->left;
            snap_rect.y = snap->top;
            snap_rect.width = snap->right - snap->left;
            snap_rect.height = snap->bottom - snap->top;
            /*todo to_weston_coordinate(client_ctx,
                         &snap_rect.x,
                         &snap_rect.y,
                         &snap_rect.width,
                         &snap_rect.height);*/

            /* offset window shadow area as there is no shadow when snapped */
            /* window_geometry here is last commited geometry */
            api->get_window_geometry(surface->xdg_toplevel->base->surface, &geometry);
            snap_rect.x -= geometry.x;
            snap_rect.y -= geometry.y;
            snap_rect.width += (surface->xdg_toplevel->current.width - geometry.width);
            snap_rect.height += (surface->xdg_toplevel->current.height - geometry.height);
            api->request_window_snap(
                surface->xdg_toplevel->base->surface, snap_rect.x, snap_rect.y, snap_rect.width, snap_rect.height
            );
            wsland_surface->force_update_window_state = true;

            struct wlr_output *wlr_output = wlr_output_layout_output_at(
                server.output_layout, snap_rect.x, snap_rect.y
            );
            wlr_output_update_needs_frame(wlr_output);
        }
    }

    free(data);
}

static UINT rail_client_snap_arrange(RailServerContext *context, const RAIL_SNAP_ARRANGE *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, snap_arrange, arg, rail_client_snap_arrange_callback);
    return CHANNEL_RC_OK;
}

static UINT rail_client_langbar_info(RailServerContext *context, const RAIL_LANGBAR_INFO_ORDER *langbarInfo) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;

    wlr_log(WLR_DEBUG, "Client: LangbarInfo: LanguageBarStatus:%d", langbarInfo->languageBarStatus);
    return CHANNEL_RC_OK;
}

static UINT rail_client_language_ime_info(RailServerContext *context, const RAIL_LANGUAGEIME_INFO_ORDER *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(
        context, language_ime_info, arg, wsland_freerdp_rail_client_language_ime_info_callback
    );
    return CHANNEL_RC_OK;
}

static UINT rail_client_compartment_info(RailServerContext *context, const RAIL_COMPARTMENT_INFO_ORDER *compartment_info) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;

    wlr_log(WLR_DEBUG, "Client: CompartmentInfo: ImeStatus: %s", compartment_info->ImeState ? "OPEN" : "CLOSED");
    wlr_log(WLR_DEBUG, "Client: CompartmentInfo: ImeConvMode: 0x%x", compartment_info->ImeConvMode);
    wlr_log(WLR_DEBUG, "Client: CompartmentInfo: ImeSentenceMode: 0x%x", compartment_info->ImeSentenceMode);
    wlr_log(WLR_DEBUG, "Client: CompartmentInfo: KanaMode: %s", compartment_info->KanaMode ? "ON" : "OFF");
    return CHANNEL_RC_OK;
}

bool wsland_freerdp_ctx_server_rail_init(struct wsland_peer_context *peer_ctx) {

    RailServerContext *rail_ctx = rail_server_context_new(peer_ctx->vcm);
    if (!rail_ctx) {
        return false;
    }
    peer_ctx->ctx_server_rail = rail_ctx;

    rail_ctx->custom = peer_ctx;
    rail_ctx->ClientHandshake = rail_client_handshake;
    rail_ctx->ClientClientStatus = rail_client_status;
    rail_ctx->ClientExec = rail_client_exec;
    rail_ctx->ClientActivate = rail_client_activate;
    rail_ctx->ClientSyscommand = rail_client_syscommand;
    rail_ctx->ClientSysmenu = rail_client_sysmenu;
    rail_ctx->ClientSysparam = rail_rail_client_sysparam;
    rail_ctx->ClientGetAppidReq = rail_client_get_appid_req;
    rail_ctx->ClientWindowMove = rail_client_window_move;
    rail_ctx->ClientSnapArrange = rail_client_snap_arrange;
    rail_ctx->ClientLangbarInfo = rail_client_langbar_info;
    rail_ctx->ClientLanguageImeInfo = rail_client_language_ime_info;
    rail_ctx->ClientCompartmentInfo = rail_client_compartment_info;
    if (rail_ctx->Start(rail_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    if (peer_ctx->peer->settings->RemoteApplicationSupportLevel & RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED) {
        RAIL_HANDSHAKE_EX_ORDER handshakeEx = {0};
        uint32_t railHandshakeFlags = TS_RAIL_ORDER_HANDSHAKEEX_FLAGS_HIDEF | TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_EXTENDED_SPI_SUPPORTED;

        if (server.freerdp.enable_window_snap_arrange) {
            railHandshakeFlags |= TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_SNAP_ARRANGE_SUPPORTED;
        }
        handshakeEx.buildNumber = 0;
        handshakeEx.railHandshakeFlags = railHandshakeFlags;
        if (rail_ctx->ServerHandshakeEx(rail_ctx, &handshakeEx) != CHANNEL_RC_OK) {
            return false;
        }
        peer_ctx->peer->DrainOutputBuffer(peer_ctx->peer);
    }
    else {
        RAIL_HANDSHAKE_ORDER handshake = {0};

        handshake.buildNumber = 0;
        if (rail_ctx->ServerHandshake(rail_ctx, &handshake) != CHANNEL_RC_OK) {
            return false;
        }
        peer_ctx->peer->DrainOutputBuffer(peer_ctx->peer);
    }

    uint waitRetry = 0;
    while (!peer_ctx->handshake_completed) {
        if (++waitRetry > 10000) { /* timeout after 100 sec. */
            return false;
        }
        usleep(10000); /* wait 0.01 sec. */
        peer_ctx->peer->CheckFileDescriptor(peer_ctx->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
    }
    return true;
}