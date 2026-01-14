// ReSharper disable All
#define _GNU_SOURCE 1

#include <unistd.h>

#include "wsland.h"

static void rail_drdynvc_destroy(struct wsland_peer_context *peer_ctx) {
    DrdynvcServerContext *vc_ctx = peer_ctx->ctx_server_drdynvc;

    if (vc_ctx) {
        vc_ctx->Stop(vc_ctx);
        drdynvc_server_context_free(vc_ctx);
    }
}

bool wsland_freerdp_ctx_server_drdynvc_init(struct wsland_peer_context *peer_ctx) {
    /* Open Dynamic virtual channel */
    DrdynvcServerContext *vc_ctx = drdynvc_server_context_new(peer_ctx->vcm);
    peer_ctx->ctx_server_drdynvc = drdynvc_server_context_new(peer_ctx->vcm);
    if (!vc_ctx) {
        return false;
    }
    if (vc_ctx->Start(vc_ctx) != CHANNEL_RC_OK) {
        drdynvc_server_context_free(vc_ctx);
        return false;
    }
    peer_ctx->ctx_server_drdynvc = vc_ctx;

    /* Force Dynamic virtual channel to exchange caps */
    if (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) == DRDYNVC_STATE_NONE) {
        int waitRetry = 0;

        peer_ctx->peer->activated = TRUE;
        /* Wait reply to arrive from client */
        while (WTSVirtualChannelManagerGetDrdynvcState(peer_ctx->vcm) != DRDYNVC_STATE_READY) {
            if (++waitRetry > 10000) {
                /* timeout after 100 sec. */
                rail_drdynvc_destroy(peer_ctx);
                return FALSE;
            }
            usleep(10000); /* wait 0.01 sec. */
            peer_ctx->peer->CheckFileDescriptor(peer_ctx->peer);
            WTSVirtualChannelManagerCheckFileDescriptor(peer_ctx->vcm);
        }
    }

    return true;
}