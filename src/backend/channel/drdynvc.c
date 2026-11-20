#include <unistd.h>
#include <freerdp/channels/drdynvc.h>

#include "backend.h"


static void backend_client_drdynvc_destroy(struct backend_client *client) {
    DrdynvcServerContext *vc_ctx = client->drdynvc_ctx;

    if (vc_ctx) {
        vc_ctx->Stop(vc_ctx);
        drdynvc_server_context_free(vc_ctx);
    }
}

bool backend_client_drdynvc_init(struct backend_client *client) { /* Open Dynamic virtual channel */
    client->drdynvc_ctx = drdynvc_server_context_new(client->vcm);
    if (!client->drdynvc_ctx) {
        return false;
    }
    if (client->drdynvc_ctx->Start(client->drdynvc_ctx) != CHANNEL_RC_OK) {
        drdynvc_server_context_free(client->drdynvc_ctx);
        return false;
    }

    /* Force Dynamic virtual channel to exchange caps */
    if (WTSVirtualChannelManagerGetDrdynvcState(client->vcm) == DRDYNVC_STATE_NONE) {
        client->peer->activated = TRUE;

        int waitRetry = 0; /* Wait reply to arrive from client */
        while (WTSVirtualChannelManagerGetDrdynvcState(client->vcm) != DRDYNVC_STATE_READY) {
            if (++waitRetry > 10000) { /* timeout after 100 sec. */
                backend_client_drdynvc_destroy(client);
                return FALSE;
            }
            usleep(10000); /* wait 0.01 sec. */
            client->peer->CheckFileDescriptor(client->peer);
            if (WTSVirtualChannelManagerIsChannelJoined(client->vcm, DRDYNVC_SVC_CHANNEL_NAME)) {
                WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
            }
        }
    }

    return true;
}