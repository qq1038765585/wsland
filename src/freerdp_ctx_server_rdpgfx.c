// ReSharper disable All
#include <assert.h>

#include <wlr/util/log.h>

#include "wsland.h"

static UINT rdpgfx_client_caps_advertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;
    RdpgfxServerContext *gfx_ctx = peer_ctx->ctx_server_rdpgfx;

    wlr_log(WLR_DEBUG, "Client: GrfxCaps count:0x%x", capsAdvertise->capsSetCount);
    for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
        RDPGFX_CAPSET *capsSet = &(capsAdvertise->capsSets[i]);

        wlr_log(WLR_DEBUG, "Client: GrfxCaps[%d] version:0x%x length:%d flags:0x%x", i, capsSet->version, capsSet->length, capsSet->flags);
        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_8");
            break;
        case RDPGFX_CAPVERSION_81:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_81");
            break;
        case RDPGFX_CAPVERSION_10:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_10");
            break;
        case RDPGFX_CAPVERSION_101:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_101");
            break;
        case RDPGFX_CAPVERSION_102:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_102");
            break;
        case RDPGFX_CAPVERSION_103:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_103");
            break;
        case RDPGFX_CAPVERSION_104:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_104");
            break;
        case RDPGFX_CAPVERSION_105:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_105");
            break;
        case RDPGFX_CAPVERSION_106:
            wlr_log(WLR_DEBUG, "	Version : RDPGFX_CAPVERSION_106");
            break;
        }

        if (capsSet->flags & RDPGFX_CAPS_FLAG_THINCLIENT) {
            wlr_log(WLR_DEBUG, "     - RDPGFX_CAPS_FLAG_THINCLIENT");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) {
            wlr_log(WLR_DEBUG, "     - RDPGFX_CAPS_FLAG_SMALL_CACHE");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
            wlr_log(WLR_DEBUG, "     - RDPGFX_CAPS_FLAG_AVC420_ENABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
            wlr_log(WLR_DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_DISABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT) {
            wlr_log(WLR_DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_THINCLIENT");
        }

        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8:
            {
                /*RDPGFX_CAPSET_VERSION8 *caps8 = (RDPGFX_CAPSET_VERSION8 *)capsSet;*/
                break;
            }
        case RDPGFX_CAPVERSION_81:
            {
                /*RDPGFX_CAPSET_VERSION81 *caps81 = (RDPGFX_CAPSET_VERSION81 *)capsSet;*/
                break;
            }
        case RDPGFX_CAPVERSION_10:
        case RDPGFX_CAPVERSION_101:
        case RDPGFX_CAPVERSION_102:
        case RDPGFX_CAPVERSION_103:
        case RDPGFX_CAPVERSION_104:
        case RDPGFX_CAPVERSION_105:
        case RDPGFX_CAPVERSION_106:
            {
                /*RDPGFX_CAPSET_VERSION10 *caps10 = (RDPGFX_CAPSET_VERSION10 *)capsSet;*/
                break;
            }
        default:
            wlr_log(WLR_ERROR, "	Version : UNKNOWN(%d)", capsSet->version);
        }
    }

    /* send caps confirm */
    RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {0};

    capsConfirm.capsSet = capsAdvertise->capsSets; /* TODO: choose right one.*/
    gfx_ctx->CapsConfirm(gfx_ctx, &capsConfirm);

    /* ready to use graphics channel */
    peer_ctx->activation_graphics_completed = TRUE;
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_cache_import_offer(RdpgfxServerContext *context, const RDPGFX_CACHE_IMPORT_OFFER_PDU *cacheImportOffer) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;

    wlr_log(WLR_DEBUG, "Client: GrfxCacheImportOffer");
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_frame_acknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge) {
    struct wsland_peer_context *peer_ctx = (struct wsland_peer_context*)context->custom;
    wlr_log(
        WLR_DEBUG, "Client: GrfxFrameAcknowledge(queueDepth = 0x%x, frameId = 0x%x, decodedFrame = %d)\n",
        frameAcknowledge->queueDepth, frameAcknowledge->frameId, frameAcknowledge->totalFramesDecoded
    );
    peer_ctx->acknowledged_frame_id = frameAcknowledge->frameId;
    peer_ctx->is_acknowledged_suspended = (frameAcknowledge->queueDepth == 0xffffffff);
    return CHANNEL_RC_OK;
}

bool wsland_freerdp_ctx_server_rdpgfx_init(struct wsland_peer_context *peer_ctx) {
    RdpgfxServerContext *gfx_ctx = rdpgfx_server_context_new(peer_ctx->vcm);
    if (!gfx_ctx) {
        return false;
    }
    peer_ctx->ctx_server_rdpgfx = gfx_ctx;

    gfx_ctx->custom = peer_ctx;
    gfx_ctx->CapsAdvertise = rdpgfx_client_caps_advertise;
    gfx_ctx->CacheImportOffer = rdpgfx_client_cache_import_offer;
    gfx_ctx->FrameAcknowledge = rdpgfx_client_frame_acknowledge;
    if (!gfx_ctx->Open(gfx_ctx)) {
        return false;
    }

    return true;
}