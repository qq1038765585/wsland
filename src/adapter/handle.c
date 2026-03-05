// ReSharper disable All
#include <assert.h>
#include <drm/drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_scene.h>
#include <cairo/cairo.h>
#include <wlr/render/pixman.h>

#include "wsland/adapter.h"
#include "wsland/utils/box.h"
#include "wsland/utils/log.h"

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

static uint32_t wsland_window_id = 0;
static uint32_t wsland_surface_id = 0;

typedef struct wsland_frame_context {
    int frame_id;
    bool need_end_frame;

    wsland_peer *peer;
    wsland_output *output;
    wsland_toplevel *toplevel;
    struct wlr_render_pass *pass;
} wsland_frame_context;

typedef struct wsland_node_data {
    struct wlr_render_pass *pass;

    wsland_toplevel *toplevel;
} wsland_node_data;

static void each_node_buffer(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    wsland_node_data *node_data = user_data;

    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface) {
        return;
    }

    wsland_surface *surface = scene_surface->surface->data;
    if (surface->dirty) {
        pixman_region32_t damage;
        pixman_region32_init(&damage);
        wlr_surface_get_effective_damage(surface->surface, &damage);
        pixman_region32_translate(&damage, sx, sy);
        pixman_region32_union(
            &node_data->toplevel->window_data->damage,
            &node_data->toplevel->window_data->damage,
            &damage
        );
        pixman_region32_fini(&damage);

        struct wlr_texture *texture = scene_buffer->texture;
        if (!texture) {
            struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
            texture = client_buffer->texture;
        }
        if (!texture) {
            return;
        }

        struct wlr_box dst_box = {
            0, 0, scene_buffer->dst_width, scene_buffer->dst_height
        };

        wlr_render_pass_add_rect(node_data->pass, &(const struct wlr_render_rect_options) {
            .box = dst_box, .color = { 0, 0, 0,  0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE
        });
        wlr_render_pass_add_texture(node_data->pass, &(const struct wlr_render_texture_options) {
            .texture = texture, .src_box = scene_buffer->src_box, .alpha = &scene_buffer->opacity,
            .dst_box = dst_box, .filter_mode = scene_buffer->filter_mode
        });
    }
}

static void each_node_detection(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    pixman_region32_t *region = user_data;

    pixman_region32_union(region, region, &scene_buffer->node.visible);
}

static void wsland_cursor_frame(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_cursor_frame);
    wsland_server *server = data;

    if (!adapter->freerdp->peer || !server->cache_cursor.dirty) {
        return;
    }

    {
        if (server->cache_cursor.buffer) {
            struct wlr_texture *texture = wlr_texture_from_buffer(
                server->renderer, server->cache_cursor.buffer
            );

            if (texture) {
                wsland_cursor_data cursor = {0};
                struct wlr_buffer *buffer = server->allocator->impl->create_buffer(
                    server->allocator, texture->width, texture->height,
                    &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888}
                );

                struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(server->renderer, buffer, NULL);
                wlr_render_pass_add_texture(pass, &(const struct wlr_render_texture_options) {
                    .texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                    .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180,
                });

                void *ptr;
                size_t stride;
                uint32_t format;
                if (wlr_render_pass_submit(pass) && wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &ptr, &format, &stride)) {
                    wlr_buffer_end_data_ptr_access(buffer);

                    cursor.data = ptr;
                    cursor.format = &format;
                    cursor.stride = &stride;
                    cursor.dirty = true;
                }

                cursor.width = server->cache_cursor.buffer->width;
                cursor.height = server->cache_cursor.buffer->height;
                cursor.hotspot_x = server->cache_cursor.hotspot_x;
                cursor.hotspot_y = server->cache_cursor.hotspot_y;

                {
                    if (!cursor.dirty) {
                        return;
                    }

                    int cursor_bpp = 4;
                    rdpUpdate *update = adapter->freerdp->peer->peer->context->update;

                    POINTER_LARGE_UPDATE pointerUpdate = {0};
                    pointerUpdate.xorBpp = cursor_bpp * 8;
                    pointerUpdate.cacheIndex = 0;
                    pointerUpdate.hotSpotX = cursor.hotspot_x;
                    pointerUpdate.hotSpotY = cursor.hotspot_y;
                    pointerUpdate.width = cursor.width,
                    pointerUpdate.height = cursor.height,
                    pointerUpdate.lengthXorMask = cursor_bpp * cursor.width * cursor.height;
                    pointerUpdate.xorMaskData = cursor.data;
                    pointerUpdate.lengthAndMask = 0;
                    pointerUpdate.andMaskData = NULL;

                    update->BeginPaint(update->context);
                    update->pointer->PointerLarge(update->context, &pointerUpdate);
                    update->EndPaint(update->context);
                }
                server->cache_cursor.dirty = false;

                wlr_texture_destroy(texture);
                wlr_buffer_drop(buffer);
            }
        }
    }
}

static void wsland_adapter_output_frame(wsland_adapter *adapter, wsland_output *output) {
    {
        if (!adapter->freerdp->peer || !(adapter->freerdp->peer->flags & WSLAND_PEER_OUTPUT_ENABLED)) {
            return;
        }
        if (adapter->freerdp->peer->is_acknowledged_suspended) {
            return;
        }
    }

    RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
    wsland_frame_context ctx = {.output = output, .peer = adapter->freerdp->peer};

    wsland_toplevel *toplevel;
    wl_list_for_each(toplevel, &output->server->toplevels, server_link) {
        ctx.toplevel = toplevel;

        if (toplevel->window_data && toplevel->window_data->dirty) {
            toplevel->window_data->dirty = false;

            struct wlr_texture *texture = wlr_texture_from_buffer(
                ctx.toplevel->server->renderer, ctx.toplevel->window_data->buffer
            );

            pixman_region32_t damage;
            pixman_region32_init_rect(&damage, 0, 0, texture->width, texture->height);

            if (pixman_region32_not_empty(&damage)) {
                int buffer_bpp = 4; /* Bytes Per Pixel. */
                int width = damage.extents.x2 - damage.extents.x1;
                int height = damage.extents.y2 - damage.extents.y1;

                int damage_stride = width * 4;
                int damage_size = damage_stride * height;
                uint8_t *ptr = malloc(damage_size);

                if (wlr_texture_read_pixels(
                    texture, &(const struct wlr_texture_read_pixels_options){
                        .data = ptr, .format = DRM_FORMAT_ARGB8888, .stride = damage_stride,
                        .src_box = { damage.extents.x1, damage.extents.y1, width, height },
                    }
                )) {
                    if (!ctx.need_end_frame) {
                        adapter->freerdp->peer->is_acknowledged_suspended = true;
                        ctx.frame_id = ++ctx.peer->current_frame_id;

                        RDPGFX_START_FRAME_PDU start_frame = {0};
                        start_frame.frameId = ctx.frame_id;
                        gfx_ctx->StartFrame(gfx_ctx, &start_frame);
                        ctx.need_end_frame = true;
                    }

                    BYTE *data = ptr;
                    bool has_alpha = !ctx.toplevel->window_data->opaque;
                    toplevel->window_data->opaque = true;
                    int alpha_size;
                    BYTE *alpha;
                    {
                        int alpha_codec_header_size = 4;
                        if (has_alpha) {
                            alpha_size = alpha_codec_header_size + width * height;
                        }
                        else {
                            /* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
                            alpha_size = alpha_codec_header_size + 8;
                        }
                        alpha = malloc(alpha_size);

                        /* generate alpha only bitmap */
                        /* set up alpha codec header */
                        alpha[0] = 'L'; /* signature */
                        alpha[1] = 'A'; /* signature */
                        alpha[2] = has_alpha ? 0 : 1; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
                        alpha[3] = 0; /* compression */

                        if (has_alpha) {
                            BYTE *alpha_bits = &data[0];

                            for (int i = 0; i < height; i++, alpha_bits += width * buffer_bpp) {
                                BYTE *src_alpha_pixel = alpha_bits + 3; /* 3 = xxxA. */
                                BYTE *dst_alpha_pixel = &alpha[alpha_codec_header_size + i * width];

                                for (int j = 0; j < width; j++, src_alpha_pixel += buffer_bpp, dst_alpha_pixel++) {
                                    *dst_alpha_pixel = *src_alpha_pixel;
                                }
                            }
                        }
                        else {
                            int bitmap_size = width * height;

                            alpha[alpha_codec_header_size] = 0xFF; /* alpha value (opaque) */
                            if (bitmap_size < 0xFF) {
                                alpha[alpha_codec_header_size + 1] = (BYTE)bitmap_size;
                                alpha_size = alpha_codec_header_size + 2; /* alpha value + size in byte. */
                            }
                            else if (bitmap_size < 0xFFFF) {
                                alpha[alpha_codec_header_size + 1] = 0xFF;
                                *(short*)&alpha[alpha_codec_header_size + 2] = (short)bitmap_size;
                                alpha_size = alpha_codec_header_size + 4; /* alpha value + 1 + size in short. */
                            }
                            else {
                                alpha[alpha_codec_header_size + 1] = 0xFF;
                                *(short*)&alpha[alpha_codec_header_size + 2] = 0xFFFF;
                                *(int*)&alpha[alpha_codec_header_size + 4] = bitmap_size;
                                alpha_size = alpha_codec_header_size + 8; /* alpha value + 1 + 2 + size in int. */
                            }
                        }
                    }

                    RDPGFX_SURFACE_COMMAND surface_command = {0};
                    surface_command.surfaceId = ctx.toplevel->window_data->surface_id;
                    surface_command.format = PIXEL_FORMAT_BGRA32;
                    surface_command.left = damage.extents.x1;
                    surface_command.top = damage.extents.y1;
                    surface_command.right = damage.extents.x2;
                    surface_command.bottom = damage.extents.y2;
                    surface_command.width = width;
                    surface_command.height = height;
                    surface_command.contextId = 0;
                    surface_command.extra = NULL;

                    surface_command.codecId = RDPGFX_CODECID_ALPHA;
                    surface_command.length = alpha_size;
                    surface_command.data = &alpha[0];
                    gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);

                    surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
                    surface_command.length = damage_size;
                    surface_command.data = &data[0];
                    gfx_ctx->SurfaceCommand(gfx_ctx, &surface_command);
                    free(alpha);
                    free(ptr);
                }
            }
            pixman_region32_fini(&damage);
            wlr_texture_destroy(texture);

            pixman_region32_clear(&ctx.toplevel->window_data->damage);
        }
    }

    if (ctx.need_end_frame) {
        RDPGFX_END_FRAME_PDU endFrame = {0};
        endFrame.frameId = ctx.peer->current_frame_id;
        gfx_ctx->EndFrame(gfx_ctx, &endFrame);
        ctx.need_end_frame = false;
    }
}

static void wsland_output_frame(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_output_frame);
    wsland_output *output = data;

    wsland_adapter_output_frame(adapter, output);
}

static void wsland_surface_commit(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_surface_commit);
    wsland_surface *surface = data;

    if (surface->surface->mapped) {
        surface->dirty = true;
    }
}

static struct wlr_texture *scene_buffer_get_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_renderer *renderer) {
    if (scene_buffer->buffer == NULL || scene_buffer->texture != NULL) {
        return scene_buffer->texture;
    }

    struct wlr_client_buffer *client_buffer =
        wlr_client_buffer_get(scene_buffer->buffer);
    if (client_buffer != NULL) {
        return client_buffer->texture;
    }

    struct wlr_texture *texture =
        wlr_texture_from_buffer(renderer, scene_buffer->buffer);
    if (texture != NULL && scene_buffer->own_buffer) {
        scene_buffer->own_buffer = false;
        wlr_buffer_unlock(scene_buffer->buffer);
    }
    return texture;
}

static void wsland_window_detection(wsland_toplevel *toplevel, bool *update, bool *offset, bool *resize, bool *title) {
    pixman_region32_t region;
    pixman_region32_init(&region);
    wlr_scene_node_for_each_buffer(&toplevel->tree->node, each_node_detection, &region);
    struct wlr_box pending = region_to_box(&region);
    pixman_region32_fini(&region);

    if (toplevel->window_data->current.width != pending.width || toplevel->window_data->current.height != pending.height) {
        {
            if (toplevel->window_data->buffer) {
                wlr_buffer_drop(toplevel->window_data->buffer);
            }
            if (pending.width > 0 || pending.height > 0) {
                    toplevel->window_data->buffer = wlr_allocator_create_buffer(
                    toplevel->server->allocator, pending.width, pending.height,
                    &(const struct wlr_drm_format) { .format = DRM_FORMAT_ABGR8888 }
                );
            }
        }
        toplevel->window_data->current.width = pending.width;
        toplevel->window_data->current.height = pending.height;
        *resize = true;
        *update = true;
    }

    if (toplevel->window_data->current.x != pending.x || toplevel->window_data->current.y != pending.y) {
        toplevel->window_data->current.x = pending.x;
        toplevel->window_data->current.y = pending.y;
        *offset = true;
        *update = true;
    }

    if (toplevel->toplevel->title) {
        if (!toplevel->window_data->title || strcmp(toplevel->toplevel->title, toplevel->window_data->title) != 0) {
            toplevel->window_data->title = strdup(toplevel->toplevel->title);
            *title = true;
            *update = true;
        }
    }
}

static void wsland_window_create(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_create);
    wsland_toplevel *toplevel = data;

    if (!adapter->freerdp->peer) {
        wsland_log(ADAPTER, ERROR, "invoke failed for wsland_window_create, not peer connected");
        return;
    }

    if (toplevel->window_data) {
        wsland_log(ADAPTER, ERROR, "invoke failed for wsland_window_create, wsland window create already");
        return;
    }

    toplevel->window_data = calloc(1, sizeof(*toplevel->window_data));
    if (!toplevel->window_data) {
        wsland_log(ADAPTER, ERROR, "calloc failed for wsland_window_create");
        return;
    }
    toplevel->window_data->window_id = ++wsland_window_id;

    {
        bool need_update = false;
        bool update_offset = false;
        bool update_resize = false;
        bool update_title = false;
        wsland_window_detection(toplevel, &need_update, &update_offset, &update_resize, &update_title);
        pixman_region32_init_rect(
            &toplevel->window_data->damage,
            toplevel->window_data->current.x,
            toplevel->window_data->current.y,
            toplevel->window_data->current.width,
            toplevel->window_data->current.height
        );

        WINDOW_ORDER_INFO window_order_info = {0};
        WINDOW_STATE_ORDER window_state_order = {0};

        window_order_info.windowId = toplevel->window_data->window_id;
        window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
        window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
        window_state_order.extendedStyle = WS_EX_LAYERED;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
        window_state_order.ownerWindowId = toplevel->window_data->parent_id;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
        window_state_order.clientOffsetX = 0;
        window_state_order.clientOffsetY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
        window_state_order.windowClientDeltaX = 0;
        window_state_order.windowClientDeltaY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
        window_state_order.visibleOffsetX = 0;
        window_state_order.visibleOffsetY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
        window_state_order.windowOffsetX = toplevel->window_data->current.x;
        window_state_order.windowOffsetY = toplevel->window_data->current.y;

        bool has_content = !wlr_box_empty(&toplevel->window_data->current);
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
        window_state_order.showState = has_content ? WINDOW_SHOW : WINDOW_HIDE;
        window_state_order.TaskbarButton = has_content ? 0 : 1;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
        window_state_order.clientAreaWidth = toplevel->window_data->current.width;
        window_state_order.clientAreaHeight = toplevel->window_data->current.height;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
        window_state_order.windowWidth = toplevel->window_data->current.width;
        window_state_order.windowHeight = toplevel->window_data->current.height;

        RECTANGLE_16 window_rect = {
            .left = toplevel->window_data->current.x,
            .top = toplevel->window_data->current.y,
            .right = toplevel->window_data->current.width,
            .bottom = toplevel->window_data->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
        window_state_order.numWindowRects = 1;
        window_state_order.windowRects = &window_rect;

        RECTANGLE_16 window_vis = {
            .left = toplevel->window_data->current.x,
             .top = toplevel->window_data->current.y,
             .right = toplevel->window_data->current.width,
             .bottom = toplevel->window_data->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
        window_state_order.numVisibilityRects = 1;
        window_state_order.visibilityRects = &window_vis;

        RAIL_UNICODE_STRING rail_window_title = {0, NULL};
        if (update_title) {
            if (utf8_string_to_rail_string(toplevel->toplevel->title, &rail_window_title)) {
                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
                window_state_order.titleInfo = rail_window_title;
            }
        }

        struct rdp_update *update = adapter->freerdp->peer->peer->update;
        update->BeginPaint(update->context);
        update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
        update->EndPaint(update->context);

        if (update_title) {
            free(rail_window_title.string);
        }

        if (update) {
            struct wlr_output *wlr_output = wlr_output_layout_output_at(
                toplevel->server->output_layout,
                toplevel->window_data->current.x < 0 ? 0 : toplevel->window_data->current.x,
                toplevel->window_data->current.y < 0 ? 0 : toplevel->window_data->current.y
            );

            if (wlr_output) {
                wsland_output *output = wlr_output->data;

                if (output) {
                    RailServerContext *rail_ctx = adapter->freerdp->peer->ctx_server_rail;

                    RAIL_MINMAXINFO_ORDER minmax_order = {0};
                    minmax_order.windowId = toplevel->window_data->window_id;
                    minmax_order.maxPosX = 0;
                    minmax_order.maxPosY = 0;
                    minmax_order.maxWidth = output->monitor.width;
                    minmax_order.maxHeight = output->monitor.height;
                    minmax_order.minTrackWidth = 0;
                    minmax_order.minTrackHeight = 0;
                    minmax_order.maxTrackWidth = output->monitor.width;
                    minmax_order.maxTrackHeight = output->monitor.height;
                    rail_ctx->ServerMinMaxInfo(rail_ctx, &minmax_order);
                }
            }
        }

        if (update_resize) {
            uint32_t prev_surface_id = toplevel->window_data->surface_id;
            RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;

            RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
            create_surface.surfaceId = (uint16_t)++wsland_surface_id;
            create_surface.width = toplevel->window_data->current.width;
            create_surface.height = toplevel->window_data->current.height;
            create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
            if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
                toplevel->window_data->surface_id = create_surface.surfaceId;
            }

            RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
            map_surface_to_window.windowId = toplevel->window_data->window_id;
            map_surface_to_window.surfaceId = toplevel->window_data->surface_id;
            map_surface_to_window.mappedWidth = toplevel->window_data->current.width;
            map_surface_to_window.mappedHeight = toplevel->window_data->current.height;
            map_surface_to_window.targetWidth = toplevel->window_data->current.width;
            map_surface_to_window.targetHeight = toplevel->window_data->current.height;
            if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window)) {
                toplevel->window_data->scale_w = 100;
                toplevel->window_data->scale_h = 100;
            }

            if (prev_surface_id) {
                RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
                deleteSurface.surfaceId = (uint16_t)prev_surface_id;
                gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
            }
        }
    }
}

static void wsland_window_commit(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_commit);
    wsland_toplevel *toplevel = data;

    if (!adapter->freerdp->peer) {
        wsland_log(ADAPTER, ERROR, "invoke failed for wsland_window_commit, not peer connected");
        return;
    }

    if (!toplevel->window_data) {
        wsland_log(ADAPTER, ERROR, "invoke failed for wsland_window_commit, not create wsland window");
        return;
    }

    {
        bool need_update = false;
        bool update_offset = false;
        bool update_resize = false;
        bool update_title = false;
        wsland_window_detection(toplevel, &need_update, &update_offset, &update_resize, &update_title);

        if (update_resize) {
            pixman_region32_init_rect(
                &toplevel->window_data->damage,
                toplevel->window_data->current.x,
                toplevel->window_data->current.y,
                toplevel->window_data->current.width,
                toplevel->window_data->current.height
            );
        }

        if (need_update) {
            WINDOW_ORDER_INFO window_order_info = {0};
            WINDOW_STATE_ORDER window_state_order = {0};

            window_order_info.windowId = toplevel->window_data->window_id;
            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

            if (update_offset) {
                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
                window_state_order.windowOffsetX = toplevel->window_data->current.x;
                window_state_order.windowOffsetY = toplevel->window_data->current.y;
            }

            if (update_resize) {
                bool has_content = !wlr_box_empty(&toplevel->window_data->current);
                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
                window_state_order.showState = has_content ? WINDOW_SHOW : WINDOW_HIDE;
                window_state_order.TaskbarButton = has_content ? 0 : 1;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
                window_state_order.clientAreaWidth = toplevel->window_data->current.width;
                window_state_order.clientAreaHeight = toplevel->window_data->current.height;

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
                window_state_order.windowWidth = toplevel->window_data->current.width;
                window_state_order.windowHeight = toplevel->window_data->current.height;

                RECTANGLE_16 window_rect = {
                    .left = toplevel->window_data->current.x,
                    .top = toplevel->window_data->current.y,
                    .right = toplevel->window_data->current.width,
                    .bottom = toplevel->window_data->current.height
                };

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
                window_state_order.numWindowRects = 1;
                window_state_order.windowRects = &window_rect;

                RECTANGLE_16 window_vis = {
                    .left = toplevel->window_data->current.x,
                     .top = toplevel->window_data->current.y,
                     .right = toplevel->window_data->current.width,
                     .bottom = toplevel->window_data->current.height
                };

                window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
                window_state_order.numVisibilityRects = 1;
                window_state_order.visibilityRects = &window_vis;
            }

            RAIL_UNICODE_STRING rail_window_title = {0, NULL};
            if (update_title) {
                if (utf8_string_to_rail_string(toplevel->toplevel->title, &rail_window_title)) {
                    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
                    window_state_order.titleInfo = rail_window_title;
                }
            }

            struct rdp_update *update = adapter->freerdp->peer->peer->update;
            update->BeginPaint(update->context);
            update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
            update->EndPaint(update->context);

            if (update_title) {
                free(rail_window_title.string);
            }

            if (update_resize) {
                uint32_t prev_surface_id = toplevel->window_data->surface_id;
                RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;

                RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
                create_surface.surfaceId = (uint16_t)++wsland_surface_id;
                create_surface.width = toplevel->window_data->current.width;
                create_surface.height = toplevel->window_data->current.height;
                create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
                if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
                    toplevel->window_data->surface_id = create_surface.surfaceId;
                }

                RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
                map_surface_to_window.windowId = toplevel->window_data->window_id;
                map_surface_to_window.surfaceId = toplevel->window_data->surface_id;
                map_surface_to_window.mappedWidth = toplevel->window_data->current.width;
                map_surface_to_window.mappedHeight = toplevel->window_data->current.height;
                map_surface_to_window.targetWidth = toplevel->window_data->current.width;
                map_surface_to_window.targetHeight = toplevel->window_data->current.height;
                if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window)) {
                    toplevel->window_data->scale_w = 100;
                    toplevel->window_data->scale_h = 100;
                }

                if (prev_surface_id) {
                    RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
                    deleteSurface.surfaceId = (uint16_t)prev_surface_id;
                    gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
                }
            }
        }
    }

    if (toplevel->server->move.mode != WSLAND_CURSOR_MOVE) {
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(toplevel->server->renderer, toplevel->window_data->buffer, NULL);
        wsland_node_data node_data = { .toplevel = toplevel, .pass = pass };
        wlr_scene_node_for_each_buffer(&toplevel->tree->node, each_node_buffer, &node_data);

        if (wlr_render_pass_submit(pass) && pixman_region32_not_empty(&toplevel->window_data->damage)) {
            toplevel->window_data->dirty = true;
        }
    }
}

static void wsland_window_destroy(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_destroy);
    wsland_toplevel *toplevel = data;

    if (!adapter->freerdp->peer || !toplevel->window_data) {
        goto destroy_window_data;
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    window_order_info.windowId = toplevel->window_data->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

    struct rdp_update *update = adapter->freerdp->peer->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowDelete(update->context, &window_order_info);
    update->EndPaint(update->context);

    if (toplevel->window_data->surface_id) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
        deleteSurface.surfaceId = (uint16_t)toplevel->window_data->surface_id;

        RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
        gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        toplevel->window_data->surface_id = 0;
    }
    toplevel->window_data->window_id = 0;

    goto destroy_window_data;
    return;

destroy_window_data:
    if (toplevel->window_data) {
        wlr_buffer_drop(toplevel->window_data->buffer);
        free(toplevel->window_data->title);
        free(toplevel->window_data);
        toplevel->window_data = NULL;
    }
}

wsland_adapter_handle wsland_adapter_handle_impl = {
    .wsland_surface_commit = wsland_surface_commit,
    .wsland_window_create = wsland_window_create,
    .wsland_window_commit = wsland_window_commit,
    .wsland_window_destroy = wsland_window_destroy,

    .wsland_cursor_frame = wsland_cursor_frame,
    .wsland_output_frame = wsland_output_frame,
};

wsland_adapter_handle *wsland_adapter_handle_init(wsland_adapter *adapter) {
    return &wsland_adapter_handle_impl;
}

struct wl_event_loop *wsland_adapter_fetch_event_loop(wsland_adapter *adapter) {
    return adapter->server->event_loop;
}