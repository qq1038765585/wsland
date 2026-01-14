// ReSharper disable All
#include <assert.h>
#include <cairo.h>
#include <drm/drm_fourcc.h>
#include <wlr/render/allocator.h>

#include <wlr/util/log.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_scene.h>

#include "wsland.h"

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)


bool wsland_freerdp_surface_cursor_update(struct wlr_cursor *cursor) {
    struct wlr_output *wlr_output = wlr_output_layout_output_at(server.output_layout, cursor->x, cursor->y);

    if (wlr_output && wlr_output->hardware_cursor && wlr_output->hardware_cursor->texture) {
        struct wsland_output *w_output = wlr_output->data;
        struct wlr_texture *cur_texture = wlr_output->hardware_cursor->texture;
        rdpUpdate *update = server.freerdp.peer_ctx->peer->update;

        if (!server.cursor_buffer || server.cursor_buffer->width != (int)cur_texture->width || server.cursor_buffer->height != (int)cur_texture->height) {
            if (server.cursor_buffer) {
                wlr_buffer_drop(server.cursor_buffer);
            }

            server.cursor_buffer = wlr_allocator_create_buffer(
                server.allocator,
                cur_texture->width,
                cur_texture->height,
                &(const struct wlr_drm_format){ .format = DRM_FORMAT_ABGR8888 }
            );
        }

        struct wsland_toplevel *toplevel;
        wl_list_for_each(toplevel, &w_output->toplevels, link) {
            if (wlr_box_contains_point(&toplevel->state.buffer_box, cursor->x, cursor->y)) {
                struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server.renderer, server.cursor_buffer, NULL);
                wlr_render_pass_add_texture(pass, &(const struct wlr_render_texture_options) {
                    .texture = cur_texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                    .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180
                });
                wlr_render_pass_submit(pass);

                int cursor_bpp = 4; /* Bytes Per Pixel. */
                void *ptr;
                size_t stride;
                uint32_t format;
                if (wlr_buffer_begin_data_ptr_access(server.cursor_buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &ptr, &format, &stride)) {
                    BYTE *data = ptr;

                    POINTER_LARGE_UPDATE pointerUpdate = {0};
                    pointerUpdate.xorBpp = cursor_bpp * 8; /* Bits Per Pixel. */
                    pointerUpdate.cacheIndex = 0;
                    pointerUpdate.hotSpotX = wlr_output->hardware_cursor->hotspot_x;
                    pointerUpdate.hotSpotY = wlr_output->hardware_cursor->hotspot_y;
                    pointerUpdate.width = server.cursor_buffer->width,
                    pointerUpdate.height = server.cursor_buffer->height,
                    pointerUpdate.lengthAndMask = 0;
                    pointerUpdate.lengthXorMask = cursor_bpp * server.cursor_buffer->width * server.cursor_buffer->height;
                    pointerUpdate.xorMaskData = data;
                    pointerUpdate.andMaskData = NULL;

                    update->BeginPaint(update->context);
                    update->pointer->PointerLarge(update->context, &pointerUpdate);
                    update->EndPaint(update->context);
                    wlr_buffer_end_data_ptr_access(server.cursor_buffer);
                }
                return true;
            }
        }
    }

    wlr_log(WLR_DEBUG, "hide cursor");
    return false;
}

bool wsland_freerdp_surface_create(struct wsland_toplevel *toplevel) {
    struct wlr_box geometry;
    wlr_surface_get_extents(toplevel->xdg_toplevel->base->surface, &geometry);

    { // freerdp surface create assert
        if (geometry.width < 0 || geometry.height < 0) {
            wlr_log(WLR_ERROR, "surface width and height are negative");
            return false;
        }

        if (!server.freerdp.peer_ctx) {
            wlr_log(WLR_ERROR, "freerdp client is not initialized");
            return false;
        }

        if (!server.freerdp.peer_ctx->peer->context) {
            wlr_log(WLR_ERROR, "freerdp client context is not initialized");
            return false;
        }

        if (!server.freerdp.peer_ctx->peer->settings->HiDefRemoteApp) {
            return false;
        }

        if (!server.freerdp.peer_ctx->activation_rail_completed) {
            wlr_log(WLR_ERROR, "freerdp rail client is not activated");
            return false;
        }

        if (!server.freerdp.peer_ctx->activation_graphics_completed) {
            wlr_log(WLR_ERROR, "freerdp rail client graphics channel is not activated");
            return false;
        }
    }

    { // freerdp surface create
        toplevel->state.window_id = wsland_freerdp_instance_new_window();

        RECTANGLE_16 window_rect = {
            .left = geometry.x, .top = geometry.y, .right = geometry.width, .bottom = geometry.height
        };
        RECTANGLE_16 window_vis = {
            .left = geometry.x, .top = geometry.y, .right = geometry.width, .bottom = geometry.height
        };

        WINDOW_ORDER_INFO window_order_info = {0};
        WINDOW_STATE_ORDER window_state_order = {0};

        window_order_info.windowId = toplevel->state.window_id;
        window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
        window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
        window_state_order.extendedStyle = WS_EX_LAYERED;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
        window_state_order.ownerWindowId = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
        window_state_order.showState = WINDOW_SHOW;
        window_state_order.TaskbarButton = 0;

        {
            RailServerContext *rail_ctx = server.freerdp.peer_ctx->ctx_server_rail;

            RAIL_MINMAXINFO_ORDER minmax_order = {0};
            minmax_order.windowId = toplevel->state.window_id;
            minmax_order.maxPosX = 0;
            minmax_order.maxPosY = 0;
            minmax_order.maxWidth = server.primary_output->base->width;
            minmax_order.maxHeight = server.primary_output->base->height;
            minmax_order.minTrackWidth = 0;
            minmax_order.minTrackHeight = 0;
            minmax_order.maxTrackWidth = server.primary_output->base->width;
            minmax_order.maxTrackHeight = server.primary_output->base->height;
            rail_ctx->ServerMinMaxInfo(rail_ctx, &minmax_order);
        }

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
        window_state_order.clientOffsetX = 0;
        window_state_order.clientOffsetY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
        window_state_order.clientAreaWidth = geometry.width;
        window_state_order.clientAreaHeight = geometry.height > 8 ? geometry.height - 8 : geometry.height;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
        window_state_order.windowOffsetX = geometry.x;
        window_state_order.windowOffsetY = geometry.y;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
        window_state_order.windowClientDeltaX = 0;
        window_state_order.windowClientDeltaY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
        window_state_order.windowWidth = geometry.width;
        window_state_order.windowHeight = geometry.height;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
        window_state_order.numWindowRects = 1;
        window_state_order.windowRects = &window_rect;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
        window_state_order.visibleOffsetX = 0;
        window_state_order.visibleOffsetY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
        window_state_order.numVisibilityRects = 1;
        window_state_order.visibilityRects = &window_vis;

        RAIL_UNICODE_STRING rail_window_title = { 0, NULL };
        if (utf8_string_to_rail_string(toplevel->xdg_toplevel->title, &rail_window_title)) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
            window_state_order.titleInfo = rail_window_title;
        }

        struct rdp_update *update = server.freerdp.peer_ctx->peer->update;
        update->BeginPaint(update->context);
        update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
        update->EndPaint(update->context);

        {
            toplevel->state.buffer_box = (struct wlr_box){
                .x = 0, .y = 0,
                .width = toplevel->xdg_toplevel->base->surface->current.width,
                .height = toplevel->xdg_toplevel->base->surface->current.height,
            };
            toplevel->state.surface_buffer = wlr_allocator_create_buffer(
            server.allocator, geometry.width, geometry.height, &(const struct wlr_drm_format){
                .format = DRM_FORMAT_ARGB8888
            }
        );
        }
        free(rail_window_title.string);
        return true;
    }
}

bool wsland_freerdp_surface_update(struct wsland_toplevel *toplevel) {
    { // freerdp surface update assert
        if (!server.freerdp.peer_ctx) {
            return false;
        }
    }

    { // gfx surface buffer update
        RdpgfxServerContext *gfx_ctx = server.freerdp.peer_ctx->ctx_server_rdpgfx;

        struct wlr_box gemotry;
        wlr_surface_get_extents(toplevel->xdg_toplevel->base->surface, &gemotry);

        bool is_size_change = false;
        {

            if (!wlr_box_equal(&toplevel->state.buffer_box, &gemotry)) {
                is_size_change = true;
            }
        }

        if (is_size_change || toplevel->state.force_recreate_surface || (toplevel->state.surface_buffer && toplevel->state.surface_id == 0)) {

            uint32_t prev_surface_id = toplevel->state.surface_id;
            toplevel->state.surface_id = wsland_freerdp_instance_new_surface();

            RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
            create_surface.surfaceId = (uint16_t)wsland_freerdp_instance_new_surface();
            create_surface.width = toplevel->state.buffer_box.width;
            create_surface.height = toplevel->state.buffer_box.height;
            create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
            if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
                toplevel->state.surface_id = create_surface.surfaceId;
                toplevel->state.buffer_box = gemotry;
            }

            RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
            map_surface_to_window.surfaceId = toplevel->state.surface_id;
            map_surface_to_window.windowId = toplevel->state.window_id;
            map_surface_to_window.mappedWidth = gemotry.width;
            map_surface_to_window.mappedHeight = gemotry.height;
            map_surface_to_window.targetWidth = gemotry.width;
            map_surface_to_window.targetHeight = gemotry.height;
            if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window)) {
                toplevel->state.scale_width = 100;
                toplevel->state.scale_height = 100;
            }

            if (prev_surface_id) {
                RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
                deleteSurface.surfaceId = (uint16_t)prev_surface_id;
                gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
            }
        }
    }

    return true;
}

bool wsland_freerdp_surface_destroy(struct wsland_toplevel *toplevel) {
    WINDOW_ORDER_INFO window_order_info = {0};
    window_order_info.windowId = toplevel->state.window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

    if (!server.freerdp.peer_ctx) {
        return false;
    }

    struct rdp_update *update = server.freerdp.peer_ctx->peer->update;
    update->BeginPaint(update->context);
    update->window->WindowDelete(update->context, &window_order_info);
    update->EndPaint(update->context);

    if (toplevel->state.surface_id) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
        deleteSurface.surfaceId = (uint16_t)toplevel->state.surface_id;

        RdpgfxServerContext *gfx_ctx = server.freerdp.peer_ctx->ctx_server_rdpgfx;
        gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        toplevel->state.surface_id = 0;
    }
    toplevel->state.window_id = 0;

    return true;
}

struct render_data {
    struct wsland_toplevel *toplevel;
    struct wlr_render_pass *pass;
};

static void scene_each_buffer(struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data) {
    struct render_data *data = user_data;

    struct wlr_texture *texture = wlr_texture_from_buffer(server.renderer, buffer->buffer);

    wlr_render_pass_add_texture(data->pass, &(const struct wlr_render_texture_options) {
        .texture = texture, .src_box = buffer->src_box, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
        .filter_mode = buffer->filter_mode
    });
    wlr_buffer_unlock(buffer->buffer);
}

void wsland_freerdp_surface_output(struct wsland_output *output) {
    RdpgfxServerContext *gfx_ctx = server.freerdp.peer_ctx->ctx_server_rdpgfx;

    RDPGFX_START_FRAME_PDU start_frame = {0};
    start_frame.frameId = ++server.freerdp.peer_ctx->current_frame_id;
    gfx_ctx->StartFrame(gfx_ctx, &start_frame);

    struct wsland_toplevel *toplevel;
    wl_list_for_each(toplevel, &output->toplevels, link) {
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server.renderer, toplevel->state.surface_buffer, NULL);

        wlr_scene_node_for_each_buffer(&toplevel->scene_tree->node, scene_each_buffer, &(struct render_data) {
            .toplevel = toplevel, .pass = pass
        });
        wlr_render_pass_submit(pass);

        void *ptr;
        size_t stride;
        uint32_t format;
        if (wlr_buffer_begin_data_ptr_access(toplevel->state.surface_buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &ptr, &format, &stride)) {
            BYTE *data = ptr;
            bool has_alpha = !wlr_buffer_is_opaque(toplevel->state.surface_buffer);

            int alpha_codec_header_size = 4;
            int buffer_bpp = 4; /* Bytes Per Pixel. */
            int alpha_size;
            BYTE *alpha;
            {
                if (has_alpha)
                    alpha_size = alpha_codec_header_size + toplevel->state.buffer_box.width * toplevel->state.buffer_box.height;
                else { /* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
                    alpha_size = alpha_codec_header_size + 8;
                }
                alpha = malloc(alpha_size);

                /* generate alpha only bitmap */
                /* set up alpha codec header */
                alpha[0] = 'L';	/* signature */
                alpha[1] = 'A';	/* signature */
                alpha[2] = has_alpha ? 0 : 1; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
                alpha[3] = 0; /* compression */

                if (has_alpha) {
                    BYTE *alpha_bits = &data[0];

                    for (int i = 0; i < toplevel->state.buffer_box.height; i++, alpha_bits+=(toplevel->state.buffer_box.width*buffer_bpp)) {
                        BYTE *src_alpha_pixel = alpha_bits + 3; /* 3 = xxxA. */
                        BYTE *dst_alpha_pixel = &alpha[alpha_codec_header_size + (i * toplevel->state.buffer_box.width)];

                        for (int j = 0; j < toplevel->state.buffer_box.width; j++, src_alpha_pixel += buffer_bpp, dst_alpha_pixel++) {
                            *dst_alpha_pixel = *src_alpha_pixel;
                        }
                    }
                } else {
                    /* whether buffer has alpha or not, always use alpha to avoid mstsc bug */
                    /* CLEARCODEC_ALPHA_RLE_SEGMENT */
                    int bitmapSize = toplevel->state.buffer_box.width * toplevel->state.buffer_box.height;

                    alpha[alpha_codec_header_size] = 0xFF; /* alpha value (opaque) */
                    if (bitmapSize < 0xFF) {
                        alpha[alpha_codec_header_size + 1] = (BYTE)bitmapSize;
                        alpha_size = alpha_codec_header_size + 2; /* alpha value + size in byte. */
                    } else if (bitmapSize < 0xFFFF) {
                        alpha[alpha_codec_header_size+1] = 0xFF;
                        *(short*)&(alpha[alpha_codec_header_size+2]) = (short)bitmapSize;
                        alpha_size = alpha_codec_header_size+4; /* alpha value + 1 + size in short. */
                    } else {
                        alpha[alpha_codec_header_size+1] = 0xFF;
                        *(short*)&(alpha[alpha_codec_header_size+2]) = 0xFFFF;
                        *(int*)&(alpha[alpha_codec_header_size+4]) = bitmapSize;
                        alpha_size = alpha_codec_header_size+8; /* alpha value + 1 + 2 + size in int. */
                    }
                }
            }

            RDPGFX_SURFACE_COMMAND surface_command = {0};
            surface_command.surfaceId = toplevel->state.surface_id;
            surface_command.format = PIXEL_FORMAT_BGRA32;
            surface_command.left = toplevel->state.buffer_box.x;
            surface_command.top = toplevel->state.buffer_box.y;
            surface_command.right = toplevel->state.buffer_box.width + toplevel->state.buffer_box.x;
            surface_command.bottom = toplevel->state.buffer_box.height + toplevel->state.buffer_box.y;
            surface_command.width = toplevel->state.buffer_box.width;
            surface_command.height = toplevel->state.buffer_box.height;
            surface_command.contextId = 0;
            surface_command.extra = NULL;

            surface_command.codecId = RDPGFX_CODECID_ALPHA;
            surface_command.length = alpha_size;
            surface_command.data = &alpha[0];
            gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);

            surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
            surface_command.length = toplevel->state.buffer_box.width * toplevel->state.buffer_box.height * buffer_bpp;
            surface_command.data = &data[0];
            gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);
            free(alpha);

            pixman_region32_clear(&toplevel->state.surface_damage);
            wlr_buffer_end_data_ptr_access(toplevel->state.surface_buffer);
        }
    }

    RDPGFX_END_FRAME_PDU end_frame = {0};
    end_frame.frameId = start_frame.frameId;
    gfx_ctx->EndFrame(gfx_ctx, &end_frame);
}