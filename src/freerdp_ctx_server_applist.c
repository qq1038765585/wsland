// ReSharper disable All
#define _GNU_SOURCE 1

#include <assert.h>
#include <unistd.h>

#include <wlr/util/log.h>

#include "wsland.h"

static void applist_client_caps_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RDPAPPLIST_CLIENT_CAPS_PDU *caps = &data->app_list_caps;
    struct wsland_peer_context *client_ctx = data->peer_ctx;
    const struct wsland_freerdp_shell_api *api = server.freerdp.freerdp_shell_api;
    char client_language_id[RDPAPPLIST_LANG_SIZE + 1] = {0}; /* +1 to ensure null-terminate. */
    wlr_log(WLR_DEBUG, "Client AppList caps version:%d", caps->version);

    if (free_only) {
        goto free;
    }

    if (!api || !api->start_app_list_update) {
        goto free;
    }

    strncpy(client_language_id, caps->clientLanguageId, RDPAPPLIST_LANG_SIZE);
    wlr_log(WLR_DEBUG, "Client AppList client language id: %s\n", client_language_id);

    client_ctx->is_applist_enabled = api->start_app_list_update(
        server.freerdp.rail_shell_context, client_language_id
    );

    free:
        free(data);
}

static UINT applist_client_caps(RdpAppListServerContext *context, const RDPAPPLIST_CLIENT_CAPS_PDU *arg) {
    WSLAND_FREERDP_DISPATCH_TO_DISPLAY_LOOP(context, app_list_caps, arg, applist_client_caps_callback);
    return CHANNEL_RC_OK;
}

bool wsland_freerdp_ctx_server_applist_init(struct wsland_peer_context *peer_ctx) {
    RdpAppListServerContext *applist_ctx = NULL;
    if (server.freerdp.rail_shell_name) {
        applist_ctx = rdpapplist_server_context_new(peer_ctx->vcm);
        if (!applist_ctx) {
            return false;
        }
        peer_ctx->ctx_server_applist = applist_ctx;

        applist_ctx->custom = peer_ctx;
        applist_ctx->ApplicationListClientCaps = applist_client_caps;
        if (applist_ctx->Open(applist_ctx) != CHANNEL_RC_OK) {
            return false;
        }

        RDPAPPLIST_SERVER_CAPS_PDU app_list_caps = {0};
        wlr_log(WLR_DEBUG, "Server AppList caps version:%d", RDPAPPLIST_CHANNEL_VERSION);
        app_list_caps.version = RDPAPPLIST_CHANNEL_VERSION;
        wlr_log(WLR_DEBUG, "    appListProviderName:%s", server.freerdp.rail_shell_name);
        if (!utf8_string_to_rail_string(server.freerdp.rail_shell_name, &app_list_caps.appListProviderName)) {
            return false;
        }

        /* assign unique id */
        char *s = getenv("WSLG_SERVICE_ID");
        if (!s) {
            s = server.freerdp.rail_shell_name;
        }
        wlr_log(WLR_DEBUG, "    appListProviderUniqueId:%s", s);
        if (!utf8_string_to_rail_string(s, &app_list_caps.appListProviderUniqueId)) {
            return false;
        }
        if (applist_ctx->ApplicationListCaps(applist_ctx, &app_list_caps) != CHANNEL_RC_OK) {
            return false;
        }
        free(app_list_caps.appListProviderName.string);
    }

    /* wait graphics channel (and optionally graphics redir channel) reponse from client */
    int waitRetry = 0;
    while (!peer_ctx->activation_graphics_completed || (peer_ctx->ctx_server_gfxredir && !peer_ctx->activation_graphics_redirection_completed)) {
        if (++waitRetry > 10000) {
            /* timeout after 100 sec. */
            return false;
        }
        usleep(10000); /* wait 0.01 sec. */
        peer_ctx->peer->CheckFileDescriptor(peer_ctx->peer);
        WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
    }

    return true;
}