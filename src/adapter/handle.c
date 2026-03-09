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
#include <wlr/render/pixman.h>
#include <cairo/cairo.h>

#include "wsland/adapter.h"
#include "wsland/utils/box.h"
#include "wsland/utils/log.h"

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

static uint32_t wsland_window_id = 0;
static uint32_t wsland_surface_id = 0;

static void scene_node_get_size(struct wlr_scene_node *node, int *width, int *height) {
    *width = 0;
    *height = 0;

    switch (node->type) {
    case WLR_SCENE_NODE_TREE:
        return;
    case WLR_SCENE_NODE_RECT:;
        struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
        *width = scene_rect->width;
        *height = scene_rect->height;
        break;
    case WLR_SCENE_NODE_BUFFER:;
        struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
        if (scene_buffer->dst_width > 0 && scene_buffer->dst_height > 0) {
            *width = scene_buffer->dst_width;
            *height = scene_buffer->dst_height;
        } else {
            *width = scene_buffer->buffer_width;
            *height = scene_buffer->buffer_height;
        }
        break;
    }
}

static void scene_node_opaque_region(struct wlr_scene_node *node, int x, int y, pixman_region32_t *opaque) {
    int width, height;
    scene_node_get_size(node, &width, &height);

    if (node->type == WLR_SCENE_NODE_RECT) {
        struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);
        if (scene_rect->color[3] != 1) {
            return;
        }
    } else if (node->type == WLR_SCENE_NODE_BUFFER) {
        struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);

        if (!scene_buffer->buffer) {
            return;
        }

        if (scene_buffer->opacity != 1) {
            return;
        }

        if (!scene_buffer->buffer_is_opaque) {
            pixman_region32_copy(opaque, &scene_buffer->opaque_region);
            pixman_region32_intersect_rect(opaque, opaque, 0, 0, width, height);
            pixman_region32_translate(opaque, x, y);
            return;
        }
    }

    pixman_region32_fini(opaque);
    pixman_region32_init_rect(opaque, x, y, width, height);
}

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener, void *data);
static void scene_buffer_set_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_texture *texture) {
    wl_list_remove(&scene_buffer->renderer_destroy.link);
    wlr_texture_destroy(scene_buffer->texture);
    scene_buffer->texture = texture;

    if (texture != NULL) {
        scene_buffer->renderer_destroy.notify = scene_buffer_handle_renderer_destroy;
        wl_signal_add(&texture->renderer->events.destroy, &scene_buffer->renderer_destroy);
    } else {
        wl_list_init(&scene_buffer->renderer_destroy.link);
    }
}

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener, void *data) {
    struct wlr_scene_buffer *scene_buffer = wl_container_of(listener, scene_buffer, renderer_destroy);
    scene_buffer_set_texture(scene_buffer, NULL);
}

static struct wlr_texture *scene_buffer_get_texture(struct wlr_scene_buffer *scene_buffer, struct wlr_renderer *renderer) {
    if (scene_buffer->buffer == NULL || scene_buffer->texture != NULL) {
        return scene_buffer->texture;
    }

    struct wlr_client_buffer *client_buffer = wlr_client_buffer_get(scene_buffer->buffer);
    if (client_buffer != NULL) {
        return client_buffer->texture;
    }

    struct wlr_texture *texture = wlr_texture_from_buffer(renderer, scene_buffer->buffer);
    if (texture != NULL && scene_buffer->own_buffer) {
        scene_buffer->own_buffer = false;
        wlr_buffer_unlock(scene_buffer->buffer);
    }
    scene_buffer_set_texture(scene_buffer, texture);
    return texture;
}

struct detection_node {
    struct wlr_scene_buffer *scene_buffer;
    int sx, sy;
};

struct detection_data {
    pixman_region32_t region;
    struct wlr_box pending;
    struct wl_array nodes;

    bool create, update, offset, resize, title, damage;

    wsland_output *output;
    wsland_adapter *adapter;
    wsland_toplevel *toplevel;
};

static void collect_detection(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    struct detection_data *data = user_data;

    if (!data->damage) {
        pixman_region32_t mix_damage;
        pixman_region32_init(&mix_damage);
        pixman_region32_intersect(&mix_damage, &scene_buffer->node.visible, &data->output->pending_commit_damage);
        if (pixman_region32_not_empty(&mix_damage)) {
            data->damage = true;
        }
        pixman_region32_fini(&mix_damage);
    }

    struct detection_node *node = wl_array_add(&data->nodes, sizeof(*node));
    node->scene_buffer = scene_buffer;
    node->sx = sx;
    node->sy = sy;

    pixman_region32_union(&data->region, &data->region, &scene_buffer->node.visible);
}

static void wsland_window_update(struct detection_data *data) {
    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = data->toplevel->window_data->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    if (data->create) {
        window_order_info.fieldFlags |= WINDOW_ORDER_STATE_NEW;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
        window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
        window_state_order.extendedStyle = WS_EX_LAYERED;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
        window_state_order.ownerWindowId = data->toplevel->window_data->parent_id;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_OFFSET;
        window_state_order.clientOffsetX = 0;
        window_state_order.clientOffsetY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_CLIENT_DELTA;
        window_state_order.windowClientDeltaX = 0;
        window_state_order.windowClientDeltaY = 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VIS_OFFSET;
        window_state_order.visibleOffsetX = 0;
        window_state_order.visibleOffsetY = 0;
    }

    if (data->offset) {
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
        window_state_order.windowOffsetX = data->toplevel->window_data->current.x;
        window_state_order.windowOffsetY = data->toplevel->window_data->current.y;
    }

    if (data->resize) {
        bool has_content = !wlr_box_empty(&data->toplevel->window_data->current);
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
        window_state_order.showState = has_content ? WINDOW_SHOW : WINDOW_HIDE;
        window_state_order.TaskbarButton = has_content ? 0 : 1;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
        window_state_order.clientAreaWidth = data->toplevel->window_data->current.width;
        window_state_order.clientAreaHeight = data->toplevel->window_data->current.height;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
        window_state_order.windowWidth = data->toplevel->window_data->current.width;
        window_state_order.windowHeight = data->toplevel->window_data->current.height;

        RECTANGLE_16 window_rect = {
            .left = 0,
            .top = 0,
            .right = data->toplevel->window_data->current.width,
            .bottom = data->toplevel->window_data->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
        window_state_order.numWindowRects = 1;
        window_state_order.windowRects = &window_rect;

        RECTANGLE_16 window_vis = {
            .left = 0,
             .top = 0,
             .right = data->toplevel->window_data->current.width,
             .bottom = data->toplevel->window_data->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
        window_state_order.numVisibilityRects = 1;
        window_state_order.visibilityRects = &window_vis;
    }

    RAIL_UNICODE_STRING rail_window_title = {0, NULL};
    if (data->title) {
        if (utf8_string_to_rail_string(data->toplevel->toplevel->title, &rail_window_title)) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
            window_state_order.titleInfo = rail_window_title;
        }
    }

    struct rdp_update *update = data->adapter->freerdp->peer->peer->update;
    update->BeginPaint(update->context);
    if (data->create) {
        update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
    } else {
        update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    }
    update->EndPaint(update->context);

    if (data->title) {
        free(rail_window_title.string);
    }

    if (data->create) {
        RailServerContext *rail_ctx = data->adapter->freerdp->peer->ctx_server_rail;

        RAIL_MINMAXINFO_ORDER minmax_order = {0};
        minmax_order.windowId = data->toplevel->window_data->window_id;
        minmax_order.maxPosX = 0;
        minmax_order.maxPosY = 0;
        minmax_order.maxWidth = data->output->monitor.width;
        minmax_order.maxHeight = data->output->monitor.height;
        minmax_order.minTrackWidth = 0;
        minmax_order.minTrackHeight = 0;
        minmax_order.maxTrackWidth = data->output->monitor.width;
        minmax_order.maxTrackHeight = data->output->monitor.height;
        rail_ctx->ServerMinMaxInfo(rail_ctx, &minmax_order);
    }

    if (data->resize) {
        uint32_t prev_surface_id = data->toplevel->window_data->surface_id;
        RdpgfxServerContext *gfx_ctx = data->adapter->freerdp->peer->ctx_server_rdpgfx;

        RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
        create_surface.surfaceId = (uint16_t)++wsland_surface_id;
        create_surface.width = data->toplevel->window_data->current.width;
        create_surface.height = data->toplevel->window_data->current.height;
        create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
        if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
            data->toplevel->window_data->surface_id = create_surface.surfaceId;
        }

        RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
        map_surface_to_window.windowId = data->toplevel->window_data->window_id;
        map_surface_to_window.surfaceId = data->toplevel->window_data->surface_id;
        map_surface_to_window.mappedWidth = data->toplevel->window_data->current.width;
        map_surface_to_window.mappedHeight = data->toplevel->window_data->current.height;
        map_surface_to_window.targetWidth = data->toplevel->window_data->current.width;
        map_surface_to_window.targetHeight = data->toplevel->window_data->current.height;
        if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window)) {
            data->toplevel->window_data->scale_w = 100;
            data->toplevel->window_data->scale_h = 100;
        }

        if (prev_surface_id) {
            RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
            deleteSurface.surfaceId = (uint16_t)prev_surface_id;
            gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        }
    }
}

static void wsland_window_detection(wsland_output *output, wsland_adapter *adapter, wsland_toplevel *toplevel) {
    struct detection_data data = { .output = output, .adapter = adapter, .toplevel = toplevel };

    if (!toplevel->window_data) {
        toplevel->window_data = calloc(1, sizeof(*toplevel->window_data));
        if (!toplevel->window_data) {
            wsland_log(ADAPTER, ERROR, "calloc failed for toplevel window_data");
            return;
        }
        toplevel->window_data->window_id = ++wsland_window_id;
        if (toplevel->toplevel->parent) {
            struct wlr_scene_tree *parent_tree = toplevel->toplevel->parent->base->data;
            if (parent_tree) {
                wsland_toplevel *parent = parent_tree->node.data;
                if (parent) {
                    toplevel->window_data->parent_id = parent->window_data->window_id;
                }
            }
        }
        data.create = true;
    }

    wl_array_init(&data.nodes);
    pixman_region32_init(&data.region);
    wlr_scene_node_for_each_buffer(&toplevel->tree->node, collect_detection, &data);
    region_to_box(&data.region, &data.pending);
    pixman_region32_fini(&data.region);

    if (toplevel->window_data->current.width != data.pending.width || toplevel->window_data->current.height != data.pending.height) {
        {
            if (toplevel->window_data->buffer) {
                wlr_buffer_drop(toplevel->window_data->buffer);
            }
            if (data.pending.width > 0 || data.pending.height > 0) {
                    toplevel->window_data->buffer = wlr_allocator_create_buffer(
                    toplevel->server->allocator, data.pending.width, data.pending.height,
                    &(const struct wlr_drm_format) { .format = DRM_FORMAT_ABGR8888 }
                );
            }
        }
        toplevel->window_data->current.width = data.pending.width;
        toplevel->window_data->current.height = data.pending.height;
        data.resize = true;
        data.update = true;
    }

    if (toplevel->window_data->current.x != data.pending.x || toplevel->window_data->current.y != data.pending.y) {
        toplevel->window_data->current.x = data.pending.x;
        toplevel->window_data->current.y = data.pending.y;
        data.offset = true;
        data.update = true;
    }

    if (toplevel->toplevel->title) {
        if (!toplevel->window_data->title || strcmp(toplevel->toplevel->title, toplevel->window_data->title) != 0) {
            toplevel->window_data->title = strdup(toplevel->toplevel->title);
            data.title = true;
            data.update = true;
        }
    }

    if (data.update) {
        wsland_window_update(&data);
    }

    if (data.damage) {
        struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
            toplevel->server->renderer, toplevel->window_data->buffer, NULL
        );

        wlr_render_pass_add_rect(pass, &(const struct wlr_render_rect_options) {
            .box = { 0, 0, toplevel->window_data->current.width, toplevel->window_data->current.height },
            .color = { 0, 0, 0,  0 },
            .blend_mode = WLR_RENDER_BLEND_MODE_NONE
        });

        struct detection_node *node;
        wl_array_for_each(node, &data.nodes) {
            struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(node->scene_buffer);
            if (!scene_surface) {
                return;
            }

            pixman_region32_t render_region;
            pixman_region32_init(&render_region);
            pixman_region32_copy(&render_region, &node->scene_buffer->node.visible);
            pixman_region32_translate(
                &render_region,
                -toplevel->window_data->current.x,
                -toplevel->window_data->current.y
            );
            if (!pixman_region32_not_empty(&render_region)) {
                pixman_region32_fini(&render_region);
                return;
            }

            struct wlr_box dst_box = {0};
            region_to_box(&render_region, &dst_box);

            pixman_region32_t opaque;
            pixman_region32_init(&opaque);
            scene_node_opaque_region(&node->scene_buffer->node, dst_box.x, dst_box.y, &opaque);
            pixman_region32_subtract(&opaque, &render_region, &opaque);

            struct wlr_texture *texture = scene_buffer_get_texture(node->scene_buffer, toplevel->server->renderer);

            if (texture) {
                wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options) {
                    .texture = texture, .src_box = node->scene_buffer->src_box, .dst_box = dst_box,
                    .clip = &render_region, .alpha = &node->scene_buffer->opacity, .filter_mode = node->scene_buffer->filter_mode,
                    .blend_mode = pixman_region32_not_empty(&opaque) ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED : WLR_RENDER_BLEND_MODE_NONE,
                });
            }

            pixman_region32_fini(&render_region);
            pixman_region32_fini(&opaque);
        }

        if (wlr_render_pass_submit(pass)) {
            pixman_region32_init_rect(
                &toplevel->window_data->damage,
                toplevel->window_data->current.x,
                toplevel->window_data->current.y,
                toplevel->window_data->current.width,
                toplevel->window_data->current.height
            );
            toplevel->window_data->dirty = true;
        }
    }
    wl_array_release(&data.nodes);
}

static void collect_region(struct wlr_scene_buffer *buffer, int sx, int sy, void *user_data) {
    pixman_region32_t *region = user_data;
    pixman_region32_union(region, region, &buffer->node.visible);
}

static void wsland_window_motion(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_motion);
    wsland_toplevel *toplevel = data;

    if (!adapter->freerdp->peer || !toplevel->window_data) {
        return;
    }
    pixman_region32_t region;
    pixman_region32_init(&region);
    wlr_scene_node_for_each_buffer(&toplevel->tree->node, collect_region, &region);
    if (toplevel->window_data->current.x != region.extents.x1 || toplevel->window_data->current.y != region.extents.y1) {
        toplevel->window_data->current.x = region.extents.x1;
        toplevel->window_data->current.y = region.extents.y1;

        {
            WINDOW_ORDER_INFO window_order_info = {0};
            WINDOW_STATE_ORDER window_state_order = {0};

            window_order_info.windowId = toplevel->window_data->window_id;
            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
            window_state_order.windowOffsetX = toplevel->window_data->current.x;
            window_state_order.windowOffsetY = toplevel->window_data->current.y;

            struct rdp_update *update = adapter->freerdp->peer->peer->update;
            update->BeginPaint(update->context);
            update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
            update->EndPaint(update->context);
        }
    }
    pixman_region32_fini(&region);
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
    }
    RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
    bool need_end_frame = false;
    int frame_id = 0;

    wsland_toplevel *toplevel;
    wl_list_for_each(toplevel, &output->server->toplevels, server_link) {
        wsland_window_detection(output, adapter, toplevel);

        if (toplevel->window_data->dirty) {
            toplevel->window_data->dirty = false;

            if (adapter->server->move.mode == WSLAND_CURSOR_MOVE) {
                return;
            }

            struct wlr_texture *texture = wlr_texture_from_buffer(
                toplevel->server->renderer, toplevel->window_data->buffer
            );

            pixman_region32_t damage;
            pixman_region32_init(&damage);
            pixman_region32_copy(&damage, &toplevel->window_data->damage);
            // pixman_region32_intersect(&damage, &damage, &toplevel->window_data->damage);
            pixman_region32_translate(&damage, -toplevel->window_data->current.x, -toplevel->window_data->current.y);
            pixman_region32_fini(&toplevel->window_data->damage);

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
                    if (!need_end_frame) {
                        adapter->freerdp->peer->is_acknowledged_suspended = true;
                        frame_id = ++adapter->freerdp->peer->current_frame_id;

                        RDPGFX_START_FRAME_PDU start_frame = { .frameId = frame_id };
                        gfx_ctx->StartFrame(gfx_ctx, &start_frame);
                        need_end_frame = true;
                    }

                    BYTE *data = ptr;
                    bool has_alpha = true;
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
                    surface_command.surfaceId = toplevel->window_data->surface_id;
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
        }
    }

    if (need_end_frame) {
        RDPGFX_END_FRAME_PDU endFrame = { .frameId = frame_id };
        gfx_ctx->EndFrame(gfx_ctx, &endFrame);
        need_end_frame = false;
    }
}

static void wsland_output_frame(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_output_frame);
    wsland_output *output = data;

    wsland_adapter_output_frame(adapter, output);
}

wsland_adapter_handle wsland_adapter_handle_impl = {
    .wsland_window_motion = wsland_window_motion,
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