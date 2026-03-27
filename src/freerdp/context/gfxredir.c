// ReSharper disable All
#include "wsland/freerdp.h"


static UINT gfxredir_client_graphics_redirection_legacy_caps(GfxRedirServerContext *context, const GFXREDIR_LEGACY_CAPS_PDU *redirection_caps) {
    if (redirection_caps->version != GFXREDIR_CHANNEL_VERSION_LEGACY) {
        return ERROR_INTERNAL_ERROR;
    }
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_graphics_redirection_caps_advertise(GfxRedirServerContext *context, const GFXREDIR_CAPS_ADVERTISE_PDU *redirection_caps) {
    wsland_peer *peer = context->custom;
    const GFXREDIR_CAPS_HEADER *current = (const GFXREDIR_CAPS_HEADER *)redirection_caps->caps;

    uint32_t length = redirection_caps->length;
    while (length <= redirection_caps->length && length >= sizeof(GFXREDIR_CAPS_HEADER)) {
        length -= current->length;
        current = (const GFXREDIR_CAPS_HEADER *)((BYTE *)current + current->length);
    }

    uint32_t selected_version = 0;
    const GFXREDIR_CAPS_HEADER *selected = NULL;
    current = (const GFXREDIR_CAPS_HEADER *)redirection_caps->caps;
    length = redirection_caps->length;
    while (length <= redirection_caps->length && length >= sizeof(GFXREDIR_CAPS_HEADER)) {
        if (current->signature != GFXREDIR_CAPS_SIGNATURE) {
            return ERROR_INVALID_DATA;
        }

        if (current->version >= selected_version) {
            selected = current;
            selected_version = current->version;
        }

        length -= selected->length;
        current = (const GFXREDIR_CAPS_HEADER *)((BYTE *)current + current->length);
    }

    if (selected) {
        GFXREDIR_CAPS_CONFIRM_PDU confirm_pdu = {0};
        confirm_pdu.version = selected->version;
        confirm_pdu.length = selected->length;
        confirm_pdu.capsData = (const BYTE *)(selected + 1);
        peer->ctx_server_gfxredir->GraphicsRedirectionCapsConfirm(peer->ctx_server_gfxredir, &confirm_pdu);
    }

    peer->activation_graphics_redirection_completed = true;
    return CHANNEL_RC_OK;
}

static UINT gfxredir_client_present_buffer_ack(GfxRedirServerContext *context, const GFXREDIR_PRESENT_BUFFER_ACK_PDU *present_ack) {
    wsland_peer *peer = context->custom;

    peer->handle->gfxredir_frame_acknowledge(peer, present_ack->windowId);
    peer->acknowledged_frame_id = present_ack->presentId;
    return CHANNEL_RC_OK;
}

bool ctx_gfxredir_init(wsland_peer *peer) {
    GfxRedirServerContext *redir_ctx = gfxredir_server_context_new(peer->vcm);
    if (!redir_ctx) {
        return false;
    }
    peer->ctx_server_gfxredir = redir_ctx;
    redir_ctx->custom = peer;
    redir_ctx->GraphicsRedirectionLegacyCaps = gfxredir_client_graphics_redirection_legacy_caps;
    redir_ctx->GraphicsRedirectionCapsAdvertise = gfxredir_client_graphics_redirection_caps_advertise;
    redir_ctx->PresentBufferAck = gfxredir_client_present_buffer_ack;
    if (redir_ctx->Open(redir_ctx) != CHANNEL_RC_OK) {
        return false;
    }

    return true;
}