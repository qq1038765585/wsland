// ReSharper disable All
#include <assert.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>

#include "wsland.h"

static UINT gfxredir_client_graphics_redirection_legacy_caps(GfxRedirServerContext *context, const GFXREDIR_LEGACY_CAPS_PDU *redirectionCaps) {
    struct wsland_peer_context *peer_ctx = context->custom;

    wlr_log(WLR_DEBUG, "Client: gfxredir_caps: version:%d", redirectionCaps->version);
    /* This is legacy caps callback, version must be 1 */
    if (redirectionCaps->version != GFXREDIR_CHANNEL_VERSION_LEGACY) {
        wlr_log(WLR_ERROR, "Client: gfxredir_caps: invalid version:%d", redirectionCaps->version);
        return ERROR_INTERNAL_ERROR;
    }

    /* Legacy version 1 client is not supported, so don't set 'activationGraphicsRedirectionCompleted'. */
    wlr_log(WLR_ERROR, "Client: gfxredir_caps: version 1 is not supported.");
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_graphics_redirection_caps_advertise(GfxRedirServerContext *context, const GFXREDIR_CAPS_ADVERTISE_PDU *redirectionCaps) {
    struct wsland_peer_context *peer_ctx = context->custom;
    const GFXREDIR_CAPS_HEADER *current = (const GFXREDIR_CAPS_HEADER*)redirectionCaps->caps;
    const GFXREDIR_CAPS_V2_0_PDU *capsV2 = NULL;

    /* dump client caps */
    uint32_t i = 0;
    uint32_t length = redirectionCaps->length;

    wlr_log(WLR_DEBUG, "Client: gfxredir_caps: length:%d", redirectionCaps->length);
    while (length <= redirectionCaps->length &&
        length >= sizeof(GFXREDIR_CAPS_HEADER)) {
        wlr_log(WLR_DEBUG, "Client: gfxredir_caps[%d]: signature:0x%x", i, current->signature);
        wlr_log(WLR_DEBUG, "Client: gfxredir_caps[%d]: version:0x%x", i, current->version);
        wlr_log(WLR_DEBUG, "Client: gfxredir_caps[%d]: length:%d", i, current->length);
        if (current->version == GFXREDIR_CAPS_VERSION2_0) {
            capsV2 = (GFXREDIR_CAPS_V2_0_PDU*)current;
            wlr_log(WLR_DEBUG, "Client: gfxredir_caps[%d]: supportedFeatures:0x%x", i, capsV2->supportedFeatures);
        }
        i++;
        length -= current->length;
        current = (const GFXREDIR_CAPS_HEADER*)((BYTE*)current + current->length);
    }

    /* select client caps */
    const GFXREDIR_CAPS_HEADER *selected = NULL;
    uint32_t selectedVersion = 0;

    current = (const GFXREDIR_CAPS_HEADER*)redirectionCaps->caps;
    length = redirectionCaps->length;
    while (length <= redirectionCaps->length && length >= sizeof(GFXREDIR_CAPS_HEADER)) {
        if (current->signature != GFXREDIR_CAPS_SIGNATURE) {
            return ERROR_INVALID_DATA;
        }
        /* Choose >= ver. 2_0 */
        if (current->version >= selectedVersion) {
            selected = current;
            selectedVersion = current->version;
        }
        length -= current->length;
        current = (const GFXREDIR_CAPS_HEADER*)((BYTE*)current + current->length);
    }

    /* reply selected caps */
    if (selected) {
        GFXREDIR_CAPS_CONFIRM_PDU confirmPdu = {0};

        wlr_log(WLR_DEBUG, "Client: gfxredir selected caps: version:0x%x", selected->version);

        confirmPdu.version = selected->version; /* return the version of selected caps */
        confirmPdu.length = selected->length; /* must return same length as selected caps from advertised */
        confirmPdu.capsData = (const BYTE*)(selected + 1); /* return caps data in selected caps */

        context->GraphicsRedirectionCapsConfirm(context, &confirmPdu);
    }

    /* ready to use graphics redirection channel */
    peer_ctx->activation_graphics_redirection_completed = true;
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_present_buffer_ack(GfxRedirServerContext *context, const GFXREDIR_PRESENT_BUFFER_ACK_PDU *presentAck) {
    struct wsland_peer_context *client_ctx = context->custom;
    struct wsland_toplevel *surface;

    wlr_log(
        WLR_DEBUG,
        "Client: gfxredir_present_buffer_ack: windowId:0x%lx",
        presentAck->windowId
    );
    wlr_log(
        WLR_DEBUG,
        "Client: gfxredir_present_buffer_ack: presentId:0x%lx",
        presentAck->presentId
    );

    client_ctx->acknowledged_frame_id = (uint32_t)presentAck->presentId;
    /* when accessing ID outside of wayland display loop thread, aquire lock */
    surface = wsland_freerdp_instance_loop_wsland_surface(client_ctx, presentAck->windowId);
    if (surface) {
        struct wsland_surface *wsland_surface = surface->xdg_toplevel->base->surface->data;
        wsland_surface->is_update_pending = FALSE;
    }
    else {
        wlr_log(
            WLR_ERROR,
            "Client: PresentBufferAck: WindowId:0x%lx is not found",
            presentAck->windowId
        );
    }
    return CHANNEL_RC_OK;
}

bool wsland_freerdp_ctx_server_gfxredir_init(struct wsland_peer_context *peer_ctx) {
    GfxRedirServerContext *redir_ctx = gfxredir_server_context_new(peer_ctx->vcm);
    if (!redir_ctx) {
        return false;
    }
    peer_ctx->ctx_server_gfxredir = redir_ctx;
    redir_ctx->custom = peer_ctx;
    redir_ctx->GraphicsRedirectionLegacyCaps = gfxredir_client_graphics_redirection_legacy_caps;
    redir_ctx->GraphicsRedirectionCapsAdvertise = gfxredir_client_graphics_redirection_caps_advertise;
    redir_ctx->PresentBufferAck = gfxredir_client_present_buffer_ack;
    if (redir_ctx->Open(redir_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    return true;
}