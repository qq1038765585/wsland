#include <unistd.h>

#include "common/logger.h"
#include "backend.h"
#include "adapter.h"


static UINT rail_client_handshake(RailServerContext *context, const RAIL_HANDSHAKE_ORDER *handshake) {
    struct backend_client *client = context->custom;
    backend_log(DEBUG, "freerdp rail client handshake build-number:%d", handshake->buildNumber);

    client->handshake_completed = TRUE;
    return CHANNEL_RC_OK;
}

static UINT rail_client_status(RailServerContext *context, const RAIL_CLIENT_STATUS_ORDER *client_status) {
    backend_log(INFO, "rail_client_status");
    return CHANNEL_RC_OK;
}

static UINT rail_client_exec(RailServerContext *context, const RAIL_EXEC_ORDER *arg) {
    backend_log(INFO, "rail_client_exec");
    return CHANNEL_RC_OK;
}

static UINT rail_client_activate(RailServerContext *context, const RAIL_ACTIVATE_ORDER *arg) {
    struct backend_client *client = context->custom;
    struct wsland_adapter *adapter = client->backend->adapter;

    DISPATCH(client, activate, arg, adapter->handle->backend_rail_client_activate);
    return CHANNEL_RC_OK;
}

static UINT rail_client_syscommand(RailServerContext *context, const RAIL_SYSCOMMAND_ORDER *arg) {
    struct backend_client *client = context->custom;
    struct wsland_adapter *adapter = client->backend->adapter;

    DISPATCH(client, syscommand, arg, adapter->handle->backend_rail_client_syscommand);
    return CHANNEL_RC_OK;
}

static UINT rail_client_sysmenu(RailServerContext *context, const RAIL_SYSMENU_ORDER *arg) {
    backend_log(INFO, "rail_client_sysmenu");
    return CHANNEL_RC_OK;
}

static UINT rail_rail_client_sysparam(RailServerContext *context, const RAIL_SYSPARAM_ORDER *arg) {
    struct backend_client *client = context->custom;
    struct wsland_adapter *adapter = client->backend->adapter;

    DISPATCH(client, sysparam, arg, adapter->handle->backend_rail_client_sysparam);
    return CHANNEL_RC_OK;
}

static UINT rail_client_get_appid_req(RailServerContext *context, const RAIL_GET_APPID_REQ_ORDER *arg) {
    struct backend_client *client = context->custom;
    struct wsland_adapter *adapter = client->backend->adapter;

    DISPATCH(client, appid, arg, adapter->handle->backend_rail_client_appid_response);
    return CHANNEL_RC_OK;
}

static UINT rail_client_window_move(RailServerContext *context, const RAIL_WINDOW_MOVE_ORDER *arg) {
    backend_log(INFO, "rail_client_window_move");
    return CHANNEL_RC_OK;
}

static UINT rail_client_snap_arrange(RailServerContext *context, const RAIL_SNAP_ARRANGE *arg) {
    backend_log(INFO, "rail_client_snap_arrange");
    return CHANNEL_RC_OK;
}

static UINT rail_client_langbar_info(RailServerContext *context, const RAIL_LANGBAR_INFO_ORDER *arg) {
    backend_log(INFO, "rail_client_langbar_info");
    return CHANNEL_RC_OK;
}

static UINT rail_client_language_ime_info(RailServerContext *context, const RAIL_LANGUAGEIME_INFO_ORDER *arg) {
    backend_log(INFO, "rail_client_language_ime_info");
    return CHANNEL_RC_OK;
}

static UINT rail_client_compartment_info(RailServerContext *context, const RAIL_COMPARTMENT_INFO_ORDER *arg) {
    backend_log(INFO, "rail_client_compartment_info");
    return CHANNEL_RC_OK;
}

bool backend_client_rail_init(struct backend_client *client) {
    RailServerContext *rail_ctx = rail_server_context_new(client->vcm);
    if (!rail_ctx) {
        return false;
    }
    client->rail_ctx = rail_ctx;

    rail_ctx->custom = client;
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

    uint32_t level = freerdp_settings_get_uint32(client->peer->context->settings, FreeRDP_RemoteApplicationSupportLevel);
    if (level & RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED) {
        RAIL_HANDSHAKE_EX_ORDER handshakeEx = {0};
        uint32_t railHandshakeFlags = TS_RAIL_ORDER_HANDSHAKEEX_FLAGS_HIDEF | TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_EXTENDED_SPI_SUPPORTED;

        if (client->backend->enable_window_snap_arrange) {
            railHandshakeFlags |= TS_RAIL_ORDER_HANDSHAKE_EX_FLAGS_SNAP_ARRANGE_SUPPORTED;
        }
        handshakeEx.buildNumber = 0;
        handshakeEx.railHandshakeFlags = railHandshakeFlags;
        if (rail_ctx->ServerHandshakeEx(rail_ctx, &handshakeEx) != CHANNEL_RC_OK) {
            return false;
        }
        client->peer->DrainOutputBuffer(client->peer);
    }
    else {
        RAIL_HANDSHAKE_ORDER handshake = {0};

        handshake.buildNumber = 0;
        if (rail_ctx->ServerHandshake(rail_ctx, &handshake) != CHANNEL_RC_OK) {
            return false;
        }
        client->peer->DrainOutputBuffer(client->peer);
    }

    uint waitRetry = 0;
    while (!client->handshake_completed) {
        if (++waitRetry > 10000) { /* timeout after 100 sec. */
            return false;
        }
        usleep(10000); /* wait 0.01 sec. */
        client->peer->CheckFileDescriptor(client->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
    }
    return true;
}