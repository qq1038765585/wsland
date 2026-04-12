// ReSharper disable All
#define _DEFAULT_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <drm/drm_fourcc.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/render/pixman.h>
#include <cairo/cairo.h>

#include "wsland/adapter.h"
#include "wsland/utils/box.h"
#include "wsland/utils/log.h"

#define RAIL_MARKER_WINDOW_ID  0xFFFFFFFE
#define RAIL_DESKTOP_WINDOW_ID 0xFFFFFFFF

#define RAIL_WINDOW_FULLSCREEN_STYLE (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_GROUP | WS_TABSTOP)
#define RAIL_WINDOW_NORMAL_STYLE (RAIL_WINDOW_FULLSCREEN_STYLE | WS_THICKFRAME | WS_CAPTION)

static uint32_t wsland_window_id = 0;
static uint32_t wsland_surface_id = 0;
static uint32_t wsland_buffer_id = 0;
static uint32_t wsland_pool_id = 0;

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
        LISTEN(&texture->renderer->events.destroy, &scene_buffer->renderer_destroy, scene_buffer_handle_renderer_destroy);
    } else {
        wl_list_init(&scene_buffer->renderer_destroy.link);
    }
}

static void scene_buffer_handle_renderer_destroy(struct wl_listener *listener, void *data) {
    struct wlr_scene_buffer *scene_buffer = wl_container_of(listener, scene_buffer, renderer_destroy);
    scene_buffer_set_texture(scene_buffer, NULL);
}

struct frame_node {
    wsland_window *window;
};

struct detection_node {
    struct wlr_scene_buffer *buffer;
    struct wlr_box geometry;
};

struct detection_data {
    struct wlr_box pending;
    pixman_region32_t region;
    struct wl_array render_nodes;

    bool create, update, offset, resize, parent, title;

    wsland_output *output;
    wsland_adapter *adapter;
    wsland_window *window;
};

static void collect_region(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    pixman_region32_t *region = user_data;

    int width, height;
    scene_node_get_size(&scene_buffer->node, &width, &height);
    pixman_region32_union_rect(region, region, sx, sy ,width, height);
}

static void collect_detection(struct wlr_scene_buffer *scene_buffer, int sx, int sy, void *user_data) {
    struct detection_data *data = user_data;

    if (!scene_buffer->own_buffer) {
        return;
    }

    int width, height;
    scene_node_get_size(&scene_buffer->node, &width, &height);

    struct detection_node *node = wl_array_add(&data->render_nodes, sizeof(*node));
    node->geometry = (struct wlr_box){ sx, sy, width, height };
    node->buffer = scene_buffer;

    pixman_region32_t region;
    pixman_region32_init_rect(&region, sx, sy, width, height);
    pixman_region32_union(&data->region, &data->region, &region);
    pixman_region32_fini(&region);
}

static void wsland_window_buffer_destroy(wsland_adapter *adapter, wsland_window *window) {
    GfxRedirServerContext *redir_ctx = adapter->freerdp->peer->ctx_server_gfxredir;

    if (window->buffer_id) {
        GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer_pdu = {0};
        destroy_buffer_pdu.bufferId = window->buffer_id;
        redir_ctx->DestroyBuffer(redir_ctx, &destroy_buffer_pdu);
        window->buffer_id = 0;
    }

    if (window->pool_id) {
        GFXREDIR_CLOSE_POOL_PDU close_pool_pdu = {0};
        close_pool_pdu.poolId = window->pool_id;
        redir_ctx->ClosePool(redir_ctx, &close_pool_pdu);
        window->pool_id = 0;
    }

    wsland_free_shared_memory(adapter->freerdp, window->window_buffer);
    window->window_buffer = NULL;
}

static void wsland_window_update(struct detection_data *data) {
    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = data->window->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    if (data->create) {
        window_order_info.fieldFlags |= WINDOW_ORDER_STATE_NEW;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
        window_state_order.style = RAIL_WINDOW_NORMAL_STYLE;
        window_state_order.extendedStyle = WS_EX_LAYERED;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
        window_state_order.ownerWindowId = data->window->parent_id;

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

    if (data->parent) {
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
        window_state_order.ownerWindowId = data->window->parent_id;
    }

    if (data->offset) {
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
        window_state_order.windowOffsetX = data->window->current.x;
        window_state_order.windowOffsetY = data->window->current.y;
    }

    if (data->resize) {
        bool has_content = !wlr_box_empty(&data->window->current);
        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
        window_state_order.showState = has_content ? WINDOW_SHOW : WINDOW_HIDE;
        window_state_order.TaskbarButton = data->window->parent_id ? 1 : 0;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
        window_state_order.clientAreaWidth = data->window->current.width;
        window_state_order.clientAreaHeight = data->window->current.height;

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
        window_state_order.windowWidth = data->window->current.width;
        window_state_order.windowHeight = data->window->current.height;

        RECTANGLE_16 window_rect = {
            .left = 0,
            .top = 0,
            .right = data->window->current.width,
            .bottom = data->window->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
        window_state_order.numWindowRects = 1;
        window_state_order.windowRects = &window_rect;

        RECTANGLE_16 window_vis = {
            .left = 0,
             .top = 0,
             .right = data->window->current.width,
             .bottom = data->window->current.height
        };

        window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
        window_state_order.numVisibilityRects = 1;
        window_state_order.visibilityRects = &window_vis;
    }

    RAIL_UNICODE_STRING rail_window_title = {0, NULL};
    if (data->title) {
        if (utf8_string_to_rail_string(data->window->title, &rail_window_title)) {
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
        minmax_order.windowId = data->window->window_id;
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
        if (data->adapter->freerdp->use_gfxredir) {
            GfxRedirServerContext *redir_ctx = data->adapter->freerdp->peer->ctx_server_gfxredir;

            if (data->window->window_buffer) {
                wsland_window_buffer_destroy(data->adapter, data->window);
                data->window->shared_memory.name[0] = '\0';
            }

            int buffer_bpp = 4;
            int width = data->window->current.width;
            int height = data->window->current.height;
            long page_size = sysconf(_SC_PAGESIZE);
            data->window->shared_memory.size = width * height * buffer_bpp + (page_size - 1) & ~(page_size - 1);
            if (wsland_allocate_shared_memory(data->adapter->freerdp, &data->window->shared_memory)) {
                uint16_t section_name[WSLAND_SHARED_MEMORY_NAME_SIZE + 1];

                for (uint32_t idx = 0; idx < WSLAND_SHARED_MEMORY_NAME_SIZE; idx++) {
                    section_name[idx] = (uint16_t)data->window->shared_memory.name[idx];
                }
                section_name[WSLAND_SHARED_MEMORY_NAME_SIZE] = 0;

                GFXREDIR_OPEN_POOL_PDU open_pool_pdu = {0};
                open_pool_pdu.poolId = ++wsland_pool_id;
                open_pool_pdu.poolSize = data->window->shared_memory.size;
                open_pool_pdu.sectionNameLength = WSLAND_SHARED_MEMORY_NAME_SIZE + 1;
                open_pool_pdu.sectionName = section_name;

                if (redir_ctx->OpenPool(redir_ctx, &open_pool_pdu) == 0) {
                    GFXREDIR_CREATE_BUFFER_PDU create_buffer_pdu = {0};
                    create_buffer_pdu.poolId = open_pool_pdu.poolId;
                    create_buffer_pdu.bufferId = ++wsland_buffer_id;
                    create_buffer_pdu.stride = width * buffer_bpp;
                    create_buffer_pdu.height = height;
                    create_buffer_pdu.width = width;
                    create_buffer_pdu.offset = 0;
                    create_buffer_pdu.format = GFXREDIR_BUFFER_PIXEL_FORMAT_ARGB_8888;
                    if (redir_ctx->CreateBuffer(redir_ctx, &create_buffer_pdu) == 0) {
                        data->window->window_buffer = data->window->shared_memory.addr;
                        data->window->buffer_id = create_buffer_pdu.bufferId;
                        data->window->pool_id = create_buffer_pdu.poolId;
                        data->window->buffer_height = height;
                        data->window->buffer_width = width;
                    }
                }

                if (!data->window->window_buffer) {
                    wsland_window_buffer_destroy(data->adapter, data->window);
                }
            }
        } else {
            uint32_t prev_surface_id = data->window->surface_id;
            RdpgfxServerContext *gfx_ctx = data->adapter->freerdp->peer->ctx_server_rdpgfx;

            RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
            create_surface.surfaceId = (uint16_t)++wsland_surface_id;
            create_surface.width = data->window->current.width;
            create_surface.height = data->window->current.height;
            create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
            if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
                data->window->surface_id = create_surface.surfaceId;
            }

            RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
            map_surface_to_window.windowId = data->window->window_id;
            map_surface_to_window.surfaceId = data->window->surface_id;
            map_surface_to_window.mappedWidth = data->window->current.width;
            map_surface_to_window.mappedHeight = data->window->current.height;
            map_surface_to_window.targetWidth = data->window->current.width;
            map_surface_to_window.targetHeight = data->window->current.height;
            if (gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window) == 0) {
                data->window->scale_w = 100;
                data->window->scale_h = 100;
            }

            if (prev_surface_id) {
                RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
                deleteSurface.surfaceId = (uint16_t)prev_surface_id;
                gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
            }
        }
    }
}

static void wsland_window_detection(wsland_output *output, wsland_adapter *adapter, wsland_window *window) {
    struct detection_data data = { .output = output, .adapter = adapter, .window = window };

    if (!window->window_id) {
        window->window_id = ++wsland_window_id;
        data.create = true;
    }

    wl_array_init(&data.render_nodes);
    pixman_region32_init(&data.region);
    window->damage = (struct wlr_box){0};
    wlr_scene_node_for_each_buffer(&window->tree->node, collect_detection, &data);
    region_to_box(&data.region, &data.pending);
    pixman_region32_intersect(&data.region, &data.region, &output->pending_commit_damage);

    if (window->current.width != data.pending.width || window->current.height != data.pending.height) {
        if (window->texture) {
            wlr_buffer_drop(window->buffer);
            wlr_texture_destroy(window->texture);
            window->texture = NULL;
            window->buffer = NULL;
        }

        if (data.pending.width > 0 && data.pending.height > 0) {
            window->buffer = wlr_allocator_create_buffer(
                window->server->allocator, data.pending.width, data.pending.height,
                &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888 }
            );
            window->texture = wlr_texture_from_buffer(window->server->renderer, window->buffer);

            pixman_region32_clear(&data.region);
            pixman_region32_union_rect(
                &data.region, &data.region,
                data.pending.x, data.pending.y,
                data.pending.width, data.pending.height
            );
            window->current.width = data.pending.width;
            window->current.height = data.pending.height;
            data.resize = true;
            data.update = true;
        }
    }
    pixman_region32_translate(&data.region, -data.pending.x, -data.pending.y);

    if (window->current.x != data.pending.x || window->current.y != data.pending.y) {
        window->current.x = data.pending.x;
        window->current.y = data.pending.y;
        data.offset = true;
        data.update = true;
    }

    if (window->handle) {
        char *title;
        if ((title = window->handle->fetch_title(window)) != NULL) {
            if (!window->title || strcmp(window->title, title) != 0) {
                if (window->title) {
                    free(window->title);
                    window->title = NULL;
                }

                window->title = strdup(title);
                data.title = true;
                data.update = true;
            }
        }

        wsland_window *parent;
        if ((parent = window->handle->fetch_parent(window)) != NULL && parent->window_id != window->parent_id) {
            window->parent_id = parent->window_id;
            data.parent = true;
            data.update = true;
        }
    }

    if (data.update) {
        wsland_window_update(&data);
    }

    if (window->texture != NULL && pixman_region32_not_empty(&data.region)) {
        struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(window->server->renderer, window->buffer, NULL);

        if (render_pass) {
            struct wlr_box damage_box = {0};
            region_to_box(&data.region, &damage_box);

            wlr_render_pass_add_rect(render_pass, &(const struct wlr_render_rect_options) {
                .box = { 0, 0, window->current.width, window->current.height },
                .color = { 0, 0, 0,  0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE
            });

            struct detection_node *render_node;
            wl_array_for_each(render_node, &data.render_nodes) {
                pixman_region32_t render_region;
                pixman_region32_init_rect(
                    &render_region,
                    render_node->geometry.x, render_node->geometry.y,
                    render_node->geometry.width, render_node->geometry.height
                );
                pixman_region32_translate(&render_region, -window->current.x, -window->current.y);
                if (pixman_region32_empty(&render_region)) {
                    pixman_region32_fini(&render_region);
                    continue;
                }

                struct wlr_box dst_box = {0};
                region_to_box(&render_region, &dst_box);

                pixman_region32_t opaque;
                pixman_region32_init(&opaque);
                scene_node_opaque_region(&render_node->buffer->node, dst_box.x, dst_box.y, &opaque);
                pixman_region32_subtract(&opaque, &render_region, &opaque);

                struct wlr_texture *texture = wlr_texture_from_buffer(
                    window->server->renderer, render_node->buffer->buffer
                );
                if (texture) {
                    wlr_render_pass_add_texture(render_pass, &(struct wlr_render_texture_options) {
                        .texture = texture, .src_box = render_node->buffer->src_box, .dst_box = dst_box,
                        .clip = &render_region, .alpha = &render_node->buffer->opacity, .filter_mode = render_node->buffer->filter_mode,
                        .blend_mode = pixman_region32_not_empty(&opaque) ? WLR_RENDER_BLEND_MODE_PREMULTIPLIED : WLR_RENDER_BLEND_MODE_NONE,
                    });
                }
                pixman_region32_fini(&render_region);
                pixman_region32_fini(&opaque);
                wlr_texture_destroy(texture);
            }

            if (wlr_render_pass_submit(render_pass)) {
                window->buffer_opaque = wlr_buffer_is_opaque(window->buffer);
                region_to_box(&data.region, &window->damage);
            }
        }
    }
    wl_array_release(&data.render_nodes);
    pixman_region32_fini(&data.region);
}

static void wsland_window_motion(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_motion);
    wsland_window *window = data;

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = window->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    pixman_region32_t region;
    pixman_region32_init(&region);
    wlr_scene_node_for_each_buffer(&window->tree->node, collect_region, &region);

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
    window_state_order.windowOffsetX = window->current.x = region.extents.x1;
    window_state_order.windowOffsetY = window->current.y = region.extents.y1;
    pixman_region32_fini(&region);

    struct rdp_update *update = adapter->freerdp->peer->peer->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);
}

static void wsland_window_destroy(struct wl_listener *listener, void *data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_destroy);
    wsland_window *window = data;

    if (!adapter->freerdp->peer || !window->window_id) {
        return;
    }

    if (adapter->freerdp->use_gfxredir) {
        wsland_window_buffer_destroy(adapter, window);
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    window_order_info.windowId = window->window_id;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

    struct rdp_update *update = adapter->freerdp->peer->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowDelete(update->context, &window_order_info);
    update->EndPaint(update->context);

    if (window->surface_id) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
        deleteSurface.surfaceId = (uint16_t)window->surface_id;

        RdpgfxServerContext *gfx_ctx = adapter->freerdp->peer->ctx_server_rdpgfx;
        gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        window->surface_id = 0;
    }
    if (window->texture) {
        wlr_buffer_drop(window->buffer);
        wlr_texture_destroy(window->texture);
        window->texture = NULL;
        window->buffer = NULL;
    }
    if (window->title) {
        free(window->title);
        window->title = NULL;
    }
    window->window_id = 0;
}

static void wsland_cursor_frame(wsland_output *output, wsland_adapter *adapter) {
    struct wlr_output_cursor *temp, *output_cursor = NULL;
    wl_list_for_each(temp, &output->output->cursors, link) {
        if (!temp->enabled || !temp->visible) {
            continue;
        }

        if (output->output->software_cursor_locks > 0) {
            if (output->output->hardware_cursor == temp || !temp->texture) {
                continue;
            }
            output_cursor = temp;
            break;
        } else {
            if (output->output->hardware_cursor == temp) {
                output_cursor = temp;
                break;
            }
        }
    }

    if (output_cursor) {
        struct wlr_texture *cursor_texture = output_cursor->texture;
        if (!output->server->wsland_cursor.swapchain || (output->server->wsland_cursor.swapchain->width != (int)cursor_texture->width || output->server->wsland_cursor.swapchain->height != (int)cursor_texture->height)) {
            if (output->server->wsland_cursor.swapchain) {
                wlr_swapchain_destroy(output->server->wsland_cursor.swapchain);
                output->server->wsland_cursor.swapchain = NULL;
            }

            output->server->wsland_cursor.swapchain = wlr_swapchain_create(
                output->server->allocator, cursor_texture->width, cursor_texture->height,
                &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888}
            );
        }

        struct wlr_buffer *buffer = wlr_swapchain_acquire(output->server->wsland_cursor.swapchain);
        if (buffer) {
            struct wlr_texture *texture = wlr_texture_from_buffer(output->server->renderer, buffer);
            struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(output->server->renderer, buffer, NULL);
            wlr_render_pass_add_texture(render_pass, &(const struct wlr_render_texture_options) {
                .texture = cursor_texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
                .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180,
            });

            if (wlr_render_pass_submit(render_pass)) {
                int cursor_bpp = 4;
                int width = texture->width;
                int height = texture->height;

                int cursor_stride = width * cursor_bpp;
                int cursor_size = cursor_stride * height;
                uint8_t *data = malloc(cursor_size);

                if (wlr_texture_read_pixels(texture, &(const struct wlr_texture_read_pixels_options) {
                    .data = data, .src_box = { 0, 0, width, height }, .stride = cursor_stride,
                    .format = DRM_FORMAT_ARGB8888,
                })) {
                    rdpUpdate *update = adapter->freerdp->peer->peer->context->update;

                    POINTER_LARGE_UPDATE pointerUpdate = {0};
                    pointerUpdate.xorBpp = cursor_bpp * 8;
                    pointerUpdate.cacheIndex = 0;
                    pointerUpdate.hotSpotX = output_cursor->hotspot_x;
                    pointerUpdate.hotSpotY = output_cursor->hotspot_y;
                    pointerUpdate.height = height,
                    pointerUpdate.width = width,
                    pointerUpdate.lengthXorMask = cursor_bpp * width * height;
                    pointerUpdate.xorMaskData = data;
                    pointerUpdate.lengthAndMask = 0;
                    pointerUpdate.andMaskData = NULL;

                    update->BeginPaint(update->context);
                    update->pointer->PointerLarge(update->context, &pointerUpdate);
                    update->EndPaint(update->context);
                }
                free(data);
            }
            wlr_buffer_unlock(buffer);
            wlr_texture_destroy(texture);
        }
    }
}

static void wsland_window_frame(struct wl_listener *listener, void *user_data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.wsland_window_frame);
    wsland_output *output = user_data;

    {
        if (!adapter->freerdp->peer || !(adapter->freerdp->peer->flags & WSLAND_PEER_OUTPUT_ENABLED)) {
            return;
        }
        if (adapter->freerdp->peer->current_frame_id - adapter->freerdp->peer->acknowledged_frame_id > 2) {
            return;
        }
    }

    wsland_cursor_frame(output, adapter);
    wsland_peer *peer = adapter->freerdp->peer;

    int window_size = wl_list_length(&adapter->server->windows);
    if (!window_size) {
        return;
    }
    uint32_t *window_ids = malloc(window_size * sizeof(uint32_t));

    struct wl_array frame_nodes;
    wl_array_init(&frame_nodes);
    {
        uint32_t idx = 0;
        wsland_window *window;
        wl_list_for_each(window, &output->server->windows, server_link) {
            wsland_window_detection(output, adapter, window);

            window_ids[idx++] = window->window_id;
            if (!wlr_box_empty(&window->damage) && (!adapter->freerdp->use_gfxredir || window->window_buffer)) {
                struct frame_node *node = wl_array_add(&frame_nodes, sizeof(*node));
                node->window = window;
            }
        }
    }

    if (output->server->move.mode == WSLAND_CURSOR_MOVE) {
        goto release;
    }

    {
        if (adapter->server->zorder) {
            WINDOW_ORDER_INFO window_order_info = {0};
            MONITORED_DESKTOP_ORDER monitored_desktop_order = {0};

            window_order_info.fieldFlags = WINDOW_ORDER_TYPE_DESKTOP | WINDOW_ORDER_FIELD_DESKTOP_ZORDER | WINDOW_ORDER_FIELD_DESKTOP_ACTIVE_WND;
            monitored_desktop_order.activeWindowId = window_ids[0];
            monitored_desktop_order.numWindowIds = window_size;
            monitored_desktop_order.windowIds = window_ids;

            peer->peer->context->update->window->MonitoredDesktop(
                peer->peer->context, &window_order_info, &monitored_desktop_order
            );
            peer->peer->DrainOutputBuffer(peer->peer);
            adapter->server->zorder = false;
        }

        if (adapter->freerdp->use_gfxredir) {
            GfxRedirServerContext *redir_ctx = peer->ctx_server_gfxredir;

            struct frame_node *node;
            wl_array_for_each(node, &frame_nodes) {
                wsland_window *window = node->window;

                int damage_bpp = 4; /* Bytes Per Pixel. */
                int damage_stride = window->damage.width * damage_bpp;
                int damage_size = damage_stride * window->damage.height;
                int bitmap_size = window->damage.width * window->damage.height;
                int start_offset = window->damage.x *damage_bpp + window->damage.y * damage_stride;
                BYTE *buffer_bits = (BYTE *)window->window_buffer + start_offset;

                GFXREDIR_PRESENT_BUFFER_PDU present_buffer_pdu = {0};
                present_buffer_pdu.timestamp = 0;
                present_buffer_pdu.orientation = 0;
                present_buffer_pdu.windowId = window->window_id;
                present_buffer_pdu.bufferId = window->buffer_id;
                present_buffer_pdu.presentId = ++adapter->freerdp->peer->current_frame_id;
                present_buffer_pdu.targetHeight = window->damage.height;
                present_buffer_pdu.targetWidth = window->damage.width;
                present_buffer_pdu.dirtyRect.left = window->damage.x;
                present_buffer_pdu.dirtyRect.top = window->damage.y;
                present_buffer_pdu.dirtyRect.width = window->damage.width;
                present_buffer_pdu.dirtyRect.height = window->damage.height;
                if (window->buffer_opaque) {
                    RECTANGLE_32 opaque_rect = {
                        .left = window->damage.x, .top = window->damage.y,
                        .width = window->damage.width, .height = window->damage.height,
                    };
                    present_buffer_pdu.numOpaqueRects = 1;
                    present_buffer_pdu.opaqueRects = &opaque_rect;
                } else {
                    present_buffer_pdu.numOpaqueRects = 0;
                    present_buffer_pdu.opaqueRects = NULL;
                }
                if (redir_ctx->PresentBuffer(redir_ctx, &present_buffer_pdu) == 0) {
                    window->update_pending = true;
                }
            }
        } else {
            RdpgfxServerContext *gfx_ctx = peer->ctx_server_rdpgfx;

            RDPGFX_START_FRAME_PDU start_frame = {0};
            start_frame.frameId = ++adapter->freerdp->peer->current_frame_id;
            gfx_ctx->StartFrame(gfx_ctx, &start_frame);

            struct frame_node *node;
            wl_array_for_each(node, &frame_nodes) {
                wsland_window *window = node->window;

                int damage_bpp = 4; /* Bytes Per Pixel. */
                int damage_stride = window->damage.width * damage_bpp;
                int damage_size = damage_stride * window->damage.height;
                int bitmap_size = window->damage.width * window->damage.height;
                uint8_t *data = malloc(damage_size);

                if (wlr_texture_read_pixels(
                    window->texture, &(const struct wlr_texture_read_pixels_options){
                        .data = data, .src_box = window->damage, .stride = damage_stride,
                        .format = DRM_FORMAT_ARGB8888
                    }
                )) {
                    int alpha_size;
                    BYTE *alpha;
                    {
                        int alpha_codec_header_size = 4;
                        if (window->buffer_opaque) { /* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
                            alpha_size = alpha_codec_header_size + 8;
                        } else {
                            alpha_size = alpha_codec_header_size + bitmap_size;
                        }
                        alpha = malloc(alpha_size);

                        /* generate alpha only bitmap */
                        /* set up alpha codec header */
                        alpha[0] = 'L'; /* signature */
                        alpha[1] = 'A'; /* signature */
                        alpha[2] = window->buffer_opaque ? 1 : 0; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
                        alpha[3] = 0; /* compression */

                        if (window->buffer_opaque) {
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
                        else {
                            BYTE *alpha_bits = &data[0];

                            for (int i = 0; i < window->damage.height; i++, alpha_bits += damage_stride) {
                                BYTE *src_alpha_pixel = alpha_bits + 3; /* 3 = xxxA. */
                                BYTE *dst_alpha_pixel = &alpha[alpha_codec_header_size + i * window->damage.width];

                                for (int j = 0; j < window->damage.width; j++, src_alpha_pixel += damage_bpp, dst_alpha_pixel++) {
                                    *dst_alpha_pixel = *src_alpha_pixel;
                                }
                            }
                        }
                    }

                    RDPGFX_SURFACE_COMMAND surface_command = {0};
                    surface_command.surfaceId = window->surface_id;
                    surface_command.format = PIXEL_FORMAT_BGRA32;
                    surface_command.left = window->damage.x;
                    surface_command.top = window->damage.y;
                    surface_command.right = window->damage.x + window->damage.width;
                    surface_command.bottom = window->damage.y + window->damage.height;
                    surface_command.width = window->damage.width;
                    surface_command.height = window->damage.height;
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
                    free(data);
                }
                window->damage = (struct wlr_box){0};
            }

            RDPGFX_END_FRAME_PDU endFrame = { .frameId = start_frame.frameId };
            gfx_ctx->EndFrame(gfx_ctx, &endFrame);
        }
        pixman_region32_clear(&output->pending_commit_damage);
    }

release:
    wl_array_release(&frame_nodes);
    free(window_ids);
}

wsland_adapter_handle wsland_adapter_handle_impl = {
    .wsland_window_frame = wsland_window_frame,

    .wsland_window_motion = wsland_window_motion,
    .wsland_window_destroy = wsland_window_destroy,
};

wsland_adapter_handle *wsland_adapter_handle_init(wsland_adapter *adapter) {
    return &wsland_adapter_handle_impl;
}

struct wl_event_loop *wsland_adapter_fetch_event_loop(wsland_adapter *adapter) {
    return adapter->server->event_loop;
}