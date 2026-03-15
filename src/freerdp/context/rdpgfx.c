// ReSharper disable All
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"


static UINT rdpgfx_client_caps_advertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise) {
    wsland_peer *peer = context->custom;
    RdpgfxServerContext *gfx_ctx = peer->ctx_server_rdpgfx;

    wsland_log(FREERDP, DEBUG, "Client: GrfxCaps count:0x%x", capsAdvertise->capsSetCount);
    for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
        RDPGFX_CAPSET *capsSet = &(capsAdvertise->capsSets[i]);

        wsland_log(FREERDP, DEBUG, "Client: GrfxCaps[%d] version:0x%x length:%d flags:0x%x", i, capsSet->version, capsSet->length, capsSet->flags);
        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_8");
            break;
        case RDPGFX_CAPVERSION_81:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_81");
            break;
        case RDPGFX_CAPVERSION_10:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_10");
            break;
        case RDPGFX_CAPVERSION_101:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_101");
            break;
        case RDPGFX_CAPVERSION_102:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_102");
            break;
        case RDPGFX_CAPVERSION_103:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_103");
            break;
        case RDPGFX_CAPVERSION_104:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_104");
            break;
        case RDPGFX_CAPVERSION_105:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_105");
            break;
        case RDPGFX_CAPVERSION_106:
            wsland_log(FREERDP, DEBUG, "	Version : RDPGFX_CAPVERSION_106");
            break;
        default: wsland_log(FREERDP, DEBUG, "Miss cap");
        }

        if (capsSet->flags & RDPGFX_CAPS_FLAG_THINCLIENT) {
            wsland_log(FREERDP, DEBUG, "     - RDPGFX_CAPS_FLAG_THINCLIENT");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) {
            wsland_log(FREERDP, DEBUG, "     - RDPGFX_CAPS_FLAG_SMALL_CACHE");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
            wsland_log(FREERDP, DEBUG, "     - RDPGFX_CAPS_FLAG_AVC420_ENABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
            wsland_log(FREERDP, DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_DISABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT) {
            wsland_log(FREERDP, DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_THINCLIENT");
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

    RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {0};

    capsConfirm.capsSet = capsAdvertise->capsSets; /* TODO: choose right one.*/
    gfx_ctx->CapsConfirm(gfx_ctx, &capsConfirm);

    peer->activation_graphics_completed = TRUE;
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_cache_import_offer(RdpgfxServerContext *context, const RDPGFX_CACHE_IMPORT_OFFER_PDU *cacheImportOffer) {
    wsland_peer *peer = context->custom;

    wsland_log(FREERDP, DEBUG, "Client: GrfxCacheImportOffer");
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_frame_acknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge) {
    DISPATCH(context, frame_acknowledge, frameAcknowledge, peer->handle->rdpgfx_frame_acknowledge);
    return CHANNEL_RC_OK;
}

bool ctx_rdpgfx_init(wsland_peer *peer) {
    RdpgfxServerContext *gfx_ctx = rdpgfx_server_context_new(peer->vcm);
    if (!gfx_ctx) {
        return false;
    }
    peer->ctx_server_rdpgfx = gfx_ctx;

    gfx_ctx->custom = peer;
    gfx_ctx->CapsAdvertise = rdpgfx_client_caps_advertise;
    gfx_ctx->CacheImportOffer = rdpgfx_client_cache_import_offer;
    gfx_ctx->FrameAcknowledge = rdpgfx_client_frame_acknowledge;
    if (!gfx_ctx->Open(gfx_ctx)) {
        return false;
    }

    return true;
}