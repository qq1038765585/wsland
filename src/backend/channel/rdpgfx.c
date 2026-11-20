#include "common/logger.h"
#include "backend.h"
#include "adapter.h"


static UINT rdpgfx_client_caps_advertise(RdpgfxServerContext *context, const RDPGFX_CAPS_ADVERTISE_PDU *capsAdvertise) {
    struct backend_client *client = context->custom;
    RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;

    backend_log(DEBUG, "Client: GrfxCaps count:0x%x", capsAdvertise->capsSetCount);
    for (int i = 0; i < capsAdvertise->capsSetCount; i++) {
        RDPGFX_CAPSET *capsSet = &capsAdvertise->capsSets[i];

        backend_log(DEBUG, "Client: GrfxCaps[%d] version:0x%x length:%d flags:0x%x", i, capsSet->version, capsSet->length, capsSet->flags);
        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_8");
            break;
        case RDPGFX_CAPVERSION_81:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_81");
            break;
        case RDPGFX_CAPVERSION_10:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_10");
            break;
        case RDPGFX_CAPVERSION_101:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_101");
            break;
        case RDPGFX_CAPVERSION_102:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_102");
            break;
        case RDPGFX_CAPVERSION_103:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_103");
            break;
        case RDPGFX_CAPVERSION_104:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_104");
            break;
        case RDPGFX_CAPVERSION_105:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_105");
            break;
        case RDPGFX_CAPVERSION_106:
            backend_log(DEBUG, "	Version : RDPGFX_CAPVERSION_106");
            break;

            default: backend_log(DEBUG, "Miss cap");
        }

        if (capsSet->flags & RDPGFX_CAPS_FLAG_THINCLIENT) {
            backend_log(DEBUG, "     - RDPGFX_CAPS_FLAG_THINCLIENT");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_SMALL_CACHE) {
            backend_log(DEBUG, "     - RDPGFX_CAPS_FLAG_SMALL_CACHE");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC420_ENABLED) {
            backend_log(DEBUG, "     - RDPGFX_CAPS_FLAG_AVC420_ENABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_DISABLED) {
            backend_log(DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_DISABLED");
        }
        if (capsSet->flags & RDPGFX_CAPS_FLAG_AVC_THINCLIENT) {
            backend_log(DEBUG, "     - RDPGFX_CAPS_FLAG_AVC_THINCLIENT");
        }

        switch (capsSet->version) {
        case RDPGFX_CAPVERSION_8: // NOLINT(*-branch-clone)
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
            backend_log(DEBUG, "	Version : UNKNOWN(%d)", capsSet->version);
        }
    }

    RDPGFX_CAPS_CONFIRM_PDU capsConfirm = {0};

    capsConfirm.capsSet = capsAdvertise->capsSets; /* TODO: choose right one.*/
    gfx_ctx->CapsConfirm(gfx_ctx, &capsConfirm);

    client->activation_graphics_completed = true;
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_cache_import_offer(RdpgfxServerContext *context, const RDPGFX_CACHE_IMPORT_OFFER_PDU *cacheImportOffer) {
    backend_log(DEBUG, "Client: GrfxCacheImportOffer");
    return CHANNEL_RC_OK;
}

static UINT rdpgfx_client_frame_acknowledge(RdpgfxServerContext *context, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge) {
    struct backend_client *client = context->custom;
    struct wsland_adapter *adapter = client->backend->adapter;

    adapter->handle->backend_rdpgfx_frame_acknowledge(client, frameAcknowledge);
    return CHANNEL_RC_OK;
}

bool backend_client_rdpgfx_init(struct backend_client *client) {
    RdpgfxServerContext *gfx_ctx = rdpgfx_server_context_new(client->vcm);
    if (!gfx_ctx) {
        return false;
    }
    client->rdpgfx_ctx = gfx_ctx;

    gfx_ctx->custom = client;
    gfx_ctx->CapsAdvertise = rdpgfx_client_caps_advertise;
    gfx_ctx->CacheImportOffer = rdpgfx_client_cache_import_offer;
    gfx_ctx->FrameAcknowledge = rdpgfx_client_frame_acknowledge;
    if (!gfx_ctx->Open(gfx_ctx)) {
        return false;
    }

    return true;
}