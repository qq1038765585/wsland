#include <fcntl.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <stdatomic.h>
#include <drm/drm_fourcc.h>
#include <linux/input-event-codes.h>

#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/allocator.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/transform.h>

#include "common/buffer.h"
#include "common/config.h"
#include "common/logger.h"
#include "common/time.h"
#include "backend.h"
#include "adapter.h"
#include "server.h"


#define ID_NEW(v, a, b) v ? a : ++b
#define WINDOW_ORDER_ICON_ROWLENGTH( W, BPP ) ((((W) * (BPP) + 31) / 32) * 4)

static atomic_int wsland_window_id = 0, wsland_surface_id = 0, wsland_buffer_id = 0, wsland_pool_id = 0;

static const struct wlr_pointer_impl wsland_pointer_impl = {
    .name = "wsland-pointer",
};

static const struct wlr_keyboard_impl wsland_keyboard_impl = {
    .name = "wsland-keyboard",
};

static struct wl_event_source * server_dispatch_event_source(struct wsland_adapter *adapter, int fd, uint32_t mask, wl_event_loop_fd_func_t func, void *data) {
    return wl_event_loop_add_fd(adapter->server->dispatch, fd, mask, func, data);
}

static void server_remove_event_source(struct wl_event_source *source) {
    if (source) {
        wl_event_source_remove(source);
    }
}

static struct wsland_pointer * backend_pointer_init(struct wsland_adapter *adapter) {
    struct wsland_pointer *pointer = calloc(1, sizeof(*pointer));
    if (!pointer) {
        adapter_log(ERROR, "calloc failed for wsland_pointer");
        return NULL;
    }
    wlr_pointer_init(&pointer->pointer, &wsland_pointer_impl, wsland_pointer_impl.name);

    pointer->pointer.base.data = pointer;
    wl_signal_emit_mutable(&adapter->server->backend->events.new_input, &pointer->pointer.base);
    return pointer;
}

static struct wsland_keyboard * backend_keyboard_init(struct wsland_adapter *adapter) {
    struct wsland_keyboard *keyboard = calloc(1, sizeof(*keyboard));
    if (!keyboard) {
        adapter_log(ERROR, "calloc failed for wsland_keyboard");
        return NULL;
    }
    wlr_keyboard_init(&keyboard->keyboard, &wsland_keyboard_impl, wsland_keyboard_impl.name);

    struct xkb_rule_names rules = {0};
    if (adapter->server->config->keyboard.layout) {
        rules.options = adapter->server->config->keyboard.options;
        rules.variant = adapter->server->config->keyboard.variant;
        rules.layout = adapter->server->config->keyboard.layout;
    } else {
        wsland_backend_fetch_keyboard_layout(adapter->backend, &rules);
    }
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);

    wlr_keyboard_set_keymap(&keyboard->keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(&keyboard->keyboard, 25, 600);
    wl_list_insert(&adapter->server->keyboards, &keyboard->link);

    keyboard->keyboard.base.data = keyboard;
    wl_signal_emit_mutable(&adapter->server->backend->events.new_input, &keyboard->keyboard.base);
    return keyboard;
}

void wsland_client_event_handle(struct backend_client *client) {
    if ((client->flags & BACKEND_CLIENT_ACTIVATED) == 0) { return; }
}

static void backend_source_release(struct wsland_adapter *adapter, struct wsland_pointer *pointer, struct wsland_keyboard *keyboard) {
    {
        struct wsland_output *output, *temp;
        wl_list_for_each_safe(output, temp, &adapter->server->outputs, link) {
            wlr_output_destroy(&output->output);
        }
    }

    if (pointer) {
        wlr_pointer_finish(&pointer->pointer);
        free(pointer);
    }

    if (keyboard) {
        wlr_keyboard_finish(&keyboard->keyboard);
        free(keyboard);
    }
}

static BOOL backend_adjust_monitor_layout(struct wsland_adapter *adapter, int count, rdpMonitor *monitors) {
    {
        struct wsland_output *output, *temp;
        wl_list_for_each_safe(output, temp, &adapter->server->outputs, link) {
            wlr_output_destroy(&output->output);
        }
        wsland_output_reset();
    }

    {
        struct wsland_window *window, *temp;
        wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
            window->metadata.geometry.height = 0;
            window->metadata.geometry.width = 0;
            window->metadata.source.x = 0;
            window->metadata.source.y = 0;
        }
    }

    pixman_region32_t r;
    pixman_region32_init(&r);
    for (int idx = 0; idx < count; idx++) {
        rdpMonitor *monitor = &monitors[idx];
        struct wlr_box output_area = {.x = monitor->x, .y = monitor->y, .width = monitor->width, .height = monitor->height};
        struct wsland_output *wo = wsland_output_create(adapter->server, output_area);

        wo->primary = monitor->is_primary;
        wo->output.phys_width = (int)monitor->attributes.physicalWidth;
        wo->output.phys_height = (int)monitor->attributes.physicalHeight;
        wo->scale.desktop = (float)monitor->attributes.desktopScaleFactor / 100;
        wo->scale.device = (float)monitor->attributes.deviceScaleFactor / 100;

        wo->max_area = wo->full_area;
        pixman_region32_union_rect(&r, &r, wo->full_area.x, wo->full_area.y, wo->full_area.width, wo->full_area.height);
    }

    pixman_box32_t *pb = pixman_region32_extents(&r);
    adapter->area = (struct wlr_box) { .x = pb->x1, .y = pb->y1, .width = pb->x2 - pb->x1, .height = pb->y2 - pb->y1 };
    pixman_region32_fini(&r);
    return TRUE;
}

static void backend_rail_client_activate(bool free_only, void *user_data) {
    struct backend_task_data *data = wl_container_of(user_data, data, task);
    struct wsland_adapter *adapter = data->client->backend->adapter;

    if (!free_only) {
        struct wsland_window *window, *temp;
        wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
            if (window->metadata.ids.window == data->activate.windowId) {
                if (window->role == POPUP) {
                    struct wsland_window *parent;
                    while ((parent = window->parent) != NULL) {
                        if (parent->role == TOPLEVEL) {
                            window = parent;
                            break;
                        }
                    }
                }

                if (data->activate.enabled) {
                    if (!window->scene->node.enabled) {
                        wlr_scene_node_set_enabled(&window->scene->node, true);
                        window->server->adapter->handle->backend_window_minimize(window, true);
                    }
                    window->server->service->dispatch_window_focus(window);
                } else {
                    window->handle->window_activate(window, data->activate.enabled);
                }

                break;
            }
        }
    }
    free(data);
}

static void backend_rail_client_sysparam(bool free_only, void *user_data) {
    struct backend_task_data *data = wl_container_of(user_data, data, task);
    struct wsland_adapter *adapter = data->client->backend->adapter;

    if (!free_only) {
        uint32_t p = data->sysparam.params;
        bool is_work_area = p & SPI_MASK_SET_WORK_AREA;
        bool is_taskbar_pos = p & SPI_MASK_TASKBAR_POS;

        if (is_work_area || is_taskbar_pos) {
            RECTANGLE_16 rect = is_work_area ? data->sysparam.workArea : data->sysparam.taskbarPos;
            struct wlr_box area = { .x = rect.left, .y = rect.top, .width = rect.right - rect.left, .height = rect.bottom - rect.top };
            
            struct wlr_output *wlo;
            struct wsland_output *wso;
            if ((wlo = wlr_output_layout_output_at(adapter->server->output_layout, area.x, area.y)) != NULL && (wso = wlo->data) != NULL) {
                if (is_work_area) {
                    wso->max_area = area;
                } else {
                    wso->max_area = wso->full_area;
                    wso->max_area.height -= area.height;
                }
            }
        }
    }
    free(data);
}

static void backend_rail_client_syscommand(bool free_only, void *user_data) {
    struct backend_task_data *data = wl_container_of(user_data, data, task);
    struct wsland_adapter *adapter = data->client->backend->adapter;

    if (!free_only) {
        struct wsland_window *window, *temp;
        wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
            if (window->metadata.ids.window == data->syscommand.windowId) {

                switch (data->syscommand.command) {
                case SC_MINIMIZE:
                    window->handle->window_minimize(window);
                    break;
                case SC_MAXIMIZE:
                    window->handle->window_maximize(window);
                    break;
                case SC_CLOSE:
                    // window->handle->window_shutdown(window);
                    break;
                case SC_MOVE:
                    adapter_log(DEBUG, "rail client syscommand SC_MOVE");
                    break;
                case SC_SIZE:
                    adapter_log(DEBUG, "rail client syscommand SC_SIZE");
                    break;
                default:
                    adapter_log(DEBUG, "rail client syscommand %d", data->syscommand.command);
                    break;
                }

                break;
            }
        }
    }

    free(data);
}

static void backend_rail_client_appid_response(bool free_only, void *user_data) {
    struct backend_task_data *data = wl_container_of(user_data, data, task);
    struct wsland_adapter *adapter = data->client->backend->adapter;

    if (!free_only) {
        struct wsland_window *window, *temp;
        wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
            if (window->metadata.ids.window == data->appid.windowId) {
                if (window->metadata.appid) {
                    adapter->handle->backend_window_appid(window);
                }
                break;
            }
        }
    }

    free(data);
}

static void backend_rdpgfx_frame_acknowledge(struct backend_client *client, const RDPGFX_FRAME_ACKNOWLEDGE_PDU *frameAcknowledge) {
    client->acknowledged_frame_id = frameAcknowledge->frameId;
}

static void backend_gfxredir_frame_acknowledge(struct backend_client *client, const GFXREDIR_PRESENT_BUFFER_ACK_PDU *present_buffer_ack) {
    struct wsland_server *server = client->backend->adapter->server;

    struct wsland_window *window, *temp;
    wl_list_for_each_safe(window, temp, &server->windows, link) {
        if (window->metadata.ids.window == present_buffer_ack->windowId) {
            window->render.pending = false;
            break;
        }
    }

    client->acknowledged_frame_id = present_buffer_ack->presentId;
}

static void backend_keyboard_key(struct wsland_keyboard *keyboard, struct wlr_keyboard_key_event *event) {
    wlr_keyboard_notify_key(&keyboard->keyboard, event);
}

static void backend_mouse_frame(struct wsland_adapter *adapter) {
    wlr_seat_pointer_notify_frame(adapter->server->seat);
}

static void backend_mouse_button(struct wlr_pointer_button_event *event) {
    wlr_pointer_notify_button(event->pointer, event);
}

static void backend_mouse_move(struct wsland_adapter *adapter, struct wlr_pointer_motion_absolute_event *event) {
    struct wsland_server *server = adapter->server;

    if (event->time_msec) {
        double dx = event->x - server->cursor->x;
        double dy = event->y - server->cursor->y;
        wlr_relative_pointer_manager_v1_send_relative_motion(
            server->relative_pointer_manager, server->seat, event->time_msec * 1000,
            dx, dy, dx, dy
        );
    }

    wlr_scene_node_set_position(&server->scene_grab->node, (int)round(server->cursor->x), (int)round(server->cursor->y));
    wlr_cursor_warp_closest(server->cursor, NULL, event->x, event->y);
    server->service->process_cursor_motion(server, event);
}

static void backend_mouse_axis(struct wsland_adapter *adapter, struct wlr_pointer_axis_event *event) {
    adapter->server->service->process_cursor_axis(adapter->server, event);
}

static struct wsland_window * find_bottom_window(struct wsland_window *window, struct wl_list *children) {
    struct wlr_scene_node *node,  *temp;
    wl_list_for_each_safe(node, temp, children, link) {
        if (node->type == WLR_SCENE_NODE_TREE && node->data) {
            struct wsland_window *child = node->data;

            if (child != window) {
                return find_bottom_window(child, &child->scene->children);
            }
        }
    }

    return window;
}

static struct wsland_output * server_output_from_point(struct wsland_adapter *adapter, uint16_t x, uint16_t y) {
    struct wsland_output *output;
    wl_list_for_each(output, &adapter->server->outputs, link) {
        if (wlr_box_contains_point(&output->full_area, x, y)) {
            return output;
        }
    }
    return NULL;
}

static void server_seat_source_config(struct wsland_server *server, struct wlr_data_source *source) {
    int serial = (int)wl_display_next_serial(server->display);
    wlr_seat_set_selection(server->seat, source, serial);
}

static struct wlr_seat * server_seat_fetch(struct wsland_server *server) {
    return server->seat;
}

static void backend_selection_source(struct wsland_adapter *adapter, struct wlr_seat *seat) {
    linux_provide_format(adapter, seat);
}

static void backend_input_synchronize(struct wsland_adapter *adapter, uint32_t flags) {
    struct backend_client *client = adapter->backend->client;

    if (client) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        for (int idx = 0; idx < BACKEND_MAX_KEYS; idx++) {
            if (client->pressed[idx]) {
                client->pressed[idx] = false;

                struct wlr_keyboard_key_event event = {0};
                event.time_msec = timespec_to_msec(&now);
                event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
                event.keycode = idx - 8;
                event.update_state = true;
                adapter->handle->backend_keyboard_key(client->keyboard, &event);
            }
        }

        struct xkb_state *xs;
        if (flags && (xs = client->keyboard->keyboard.xkb_state) != NULL) {
            bool caps_lock = flags & KBD_SYNC_CAPS_LOCK;
            bool xkb_active = xkb_state_mod_name_is_active(xs, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE);

            if (caps_lock != xkb_active) {
                struct wlr_keyboard_key_event event = {0};

                {
                    event.time_msec = timespec_to_msec(&now);
                    event.state = WL_KEYBOARD_KEY_STATE_PRESSED;
                    event.keycode = KEY_CAPSLOCK;
                    event.update_state = true;
                    adapter->handle->backend_keyboard_key(client->keyboard, &event);
                }

                {
                    event.time_msec = timespec_to_msec(&now);
                    event.state = WL_KEYBOARD_KEY_STATE_RELEASED;
                    event.keycode = KEY_CAPSLOCK;
                    event.update_state = true;
                    adapter->handle->backend_keyboard_key(client->keyboard, &event);
                }
            }
        }
    }
}

static void backend_cursor_update(struct wsland_output *output, struct wlr_buffer *buffer, int hotspot_x, int hotspot_y) {
    if (buffer) {
        struct wlr_texture *texture = wlr_texture_from_buffer(output->output.renderer, buffer);
        if (!texture) {
            return;
        }

        struct wlr_buffer *cursor_buffer = wlr_allocator_create_buffer(
            output->server->allocator, buffer->width, buffer->height,
            &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888}
        );

        struct wlr_render_pass *render_pass = wlr_renderer_begin_buffer_pass(output->server->renderer, cursor_buffer, NULL);
        wlr_render_pass_add_texture(render_pass, &(const struct wlr_render_texture_options) {
            .texture = texture, .blend_mode = WLR_RENDER_BLEND_MODE_NONE,
            .transform = WL_OUTPUT_TRANSFORM_FLIPPED_180,
        });

        if (wlr_render_pass_submit(render_pass)) {
            int cursor_bpp = 4;
            int width = cursor_buffer->width;
            int height = cursor_buffer->height;

            int cursor_stride = width * cursor_bpp;
            int cursor_size = cursor_stride * height;
            uint8_t *data = malloc(cursor_size);

            struct wlr_texture *cursor_texture = wlr_texture_from_buffer(output->output.renderer, cursor_buffer);
            if (cursor_texture && wlr_texture_read_pixels(cursor_texture, &(const struct wlr_texture_read_pixels_options) {
                .data = data, .src_box = { .x = 0, .y = 0, .width = width, .height = height }, .stride = cursor_stride,
                .format = DRM_FORMAT_ARGB8888,
            })) {
                rdpUpdate *update = output->server->adapter->backend->client->peer->context->update;

                POINTER_LARGE_UPDATE pointerUpdate = {0};
                pointerUpdate.xorBpp = cursor_bpp * 8;
                pointerUpdate.cacheIndex = 0;
                pointerUpdate.hotSpotX = hotspot_x;
                pointerUpdate.hotSpotY = hotspot_y;
                pointerUpdate.height = height,
                pointerUpdate.width = width,
                pointerUpdate.lengthXorMask = cursor_size;
                pointerUpdate.xorMaskData = data;
                pointerUpdate.lengthAndMask = 0;
                pointerUpdate.andMaskData = NULL;

                update->BeginPaint(update->context);
                update->pointer->PointerLarge(update->context, &pointerUpdate);
                update->EndPaint(update->context);
            }
            free(data);
            wlr_texture_destroy(cursor_texture);
            wlr_buffer_drop(cursor_buffer);
        }
        wlr_texture_destroy(texture);
    }
}

static void backend_output_frame(struct wsland_adapter *adapter) {
    struct backend_client *client = adapter->backend->client;
    struct wsland_server *server = adapter->server;

    if (client->flags & BACKEND_CLIENT_ACTIVATED && client->current_frame_id - client->acknowledged_frame_id < 2) {
        struct render_node { struct wsland_window *window; };
        struct wl_array render_nodes;
        wl_array_init(&render_nodes);

        struct render_node *node;
        struct wsland_window *ww, *tmp;
        wl_list_for_each_safe(ww, tmp, &server->windows, link) {
            if (ww->render.resizing) {
                goto release;
            }

            if (!ww->metadata.mapped || ww->render.pending) {
                continue;
            }

            struct wsland_output *wo;
            if ((wo = ww->handle->fetch_output(ww)) != NULL && wo != ww->render.wo) {
                ww->render.wo = wo;
                ww->metadata.source.y = 0;
                ww->metadata.source.x = 0;
                ww->metadata.geometry.width = 0;
                ww->metadata.geometry.height = 0;
                ww->server->service->window_dest_scale(ww);
            }

            adapter->handle->backend_window_metadata_detect(ww);
            if (ww->render.dirty || pixman_region32_not_empty(&ww->render.damage)) {
                server->service->window_render_detect(ww);

                if (ww->render.buffer) {
                    struct render_node *rn = wl_array_add(&render_nodes, sizeof(*rn));
                    rn->window = ww;
                }
            }
        }

        if (render_nodes.size > 0) {
            RDPGFX_START_FRAME_PDU start_frame = {0};
            wl_array_for_each(node, &render_nodes) {
                struct wsland_window *window = node->window;
                bool buffer_opaque = wlr_buffer_is_opaque(window->render.buffer);
                struct wlr_texture *texture = wlr_texture_from_buffer(server->renderer, window->render.buffer);
                if (!texture) { goto render_release; }

                int tw = window->metadata.target.width;
                int th = window->metadata.target.height;
                pixman_box32_t *pb = pixman_region32_extents(&window->render.damage);
                struct wlr_box damage = { .x = pb->x1, .y = pb->y1, .width = pb->x2 - pb->x1, .height = pb->y2 - pb->y1, };
                server->service->scale_box(server, &damage, window->render.wo->scale.desktop);

                wlr_box_intersection(&damage, &damage, &(struct wlr_box){ .width = tw, .height = th });
                if (wlr_box_empty(&damage)) { goto render_release; }

                int damage_bpp = 4;
                if (adapter->backend->use_gfxredir) {
                    assert(window->render.gfx_buffer);
                    int damage_stride = tw * damage_bpp;

                    int start_offset = damage.x * damage_bpp + damage.y * damage_stride;
                    BYTE *data = (BYTE *)window->render.gfx_buffer + start_offset;

                    if (wlr_texture_read_pixels(
                            texture, &(const struct wlr_texture_read_pixels_options){
                                .data = data, .src_box = damage, .stride = damage_stride,
                                .format = DRM_FORMAT_ARGB8888,
                            }
                    )) {
                        GFXREDIR_PRESENT_BUFFER_PDU present_buffer_pdu = {0};
                        present_buffer_pdu.timestamp = 0;
                        present_buffer_pdu.orientation = 0;
                        present_buffer_pdu.windowId = window->metadata.ids.window;
                        present_buffer_pdu.bufferId = window->metadata.ids.buffer;
                        present_buffer_pdu.presentId = ++client->current_frame_id;
                        present_buffer_pdu.targetHeight = th;
                        present_buffer_pdu.targetWidth = tw;
                        present_buffer_pdu.dirtyRect.left = damage.x;
                        present_buffer_pdu.dirtyRect.top = damage.y;
                        present_buffer_pdu.dirtyRect.width = damage.width;
                        present_buffer_pdu.dirtyRect.height = damage.height;
                        if (buffer_opaque) {
                            RECTANGLE_32 opaque_rect = {
                                .left = damage.x, .top = damage.y,
                                .width = damage.width, .height = damage.height,
                            };
                            present_buffer_pdu.numOpaqueRects = 1;
                            present_buffer_pdu.opaqueRects = &opaque_rect;
                        } else {
                            present_buffer_pdu.numOpaqueRects = 0;
                            present_buffer_pdu.opaqueRects = NULL;
                        }
                        if (client->gfxredir_ctx->PresentBuffer(client->gfxredir_ctx, &present_buffer_pdu) == 0) {
                            pixman_region32_clear(&window->render.damage);
                            window->render.pending = true;
                            window->render.dirty = false;
                        }
                    }

                    goto render_release;
                }

                if (!start_frame.frameId) {
                    start_frame.frameId = ++client->current_frame_id;
                    client->rdpgfx_ctx->StartFrame(client->rdpgfx_ctx, &start_frame);
                }

                int damage_stride = damage.width * damage_bpp;
                int damage_size = damage.height * damage_stride;
                int bitmap_size = damage.height * damage.width;
                uint8_t *data = malloc(damage_size);

                if (wlr_texture_read_pixels(
                    texture, &(const struct wlr_texture_read_pixels_options){
                        .data = data, .src_box = damage, .stride = damage_stride,
                        .format = DRM_FORMAT_ARGB8888,
                    }
                )) {
                    int alpha_size;
                    uint8_t *alpha;

                    {
                        int alpha_codec_header_size = 4;
                        if (buffer_opaque) { /* 8 = max of ALPHA_RLE_SEGMENT for single alpha value. */
                            alpha_size = alpha_codec_header_size + 8;
                        } else {
                            alpha_size = alpha_codec_header_size + bitmap_size;
                        }
                        alpha = malloc(alpha_size);

                        /* generate alpha only bitmap */
                        /* set up alpha codec header */
                        alpha[0] = 'L'; /* signature */
                        alpha[1] = 'A'; /* signature */
                        alpha[2] = buffer_opaque ? 1 : 0; /* compression: RDP spec indicate this is non-zero value for compressed, but it must be 1.*/
                        alpha[3] = 0; /* compression */

                        if (buffer_opaque) {
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
                                *(short*)&alpha[alpha_codec_header_size + 2] = (short)0xFFFF;
                                *(int*)&alpha[alpha_codec_header_size + 4] = bitmap_size;
                                alpha_size = alpha_codec_header_size + 8; /* alpha value + 1 + 2 + size in int. */
                            }
                        }
                        else {
                            BYTE *alpha_bits = &data[0];

                            for (int i = 0; i < damage.height; i++, alpha_bits += damage_stride) {
                                BYTE *src_alpha_pixel = alpha_bits + 3; /* 3 = xxxA. */
                                BYTE *dst_alpha_pixel = &alpha[alpha_codec_header_size + i * damage.width];

                                for (int j = 0; j < damage.width; j++, src_alpha_pixel += damage_bpp, dst_alpha_pixel++) {
                                    *dst_alpha_pixel = *src_alpha_pixel;
                                }
                            }
                        }
                    }

                    RDPGFX_SURFACE_COMMAND surface_command = {0};
                    surface_command.surfaceId = window->metadata.ids.surface;
                    surface_command.format = PIXEL_FORMAT_BGRA32;
                    surface_command.left = damage.x;
                    surface_command.top = damage.y;
                    surface_command.right = damage.x + damage.width;
                    surface_command.bottom = damage.y + damage.height;
                    surface_command.height = damage.height;
                    surface_command.width = damage.width;
                    surface_command.contextId = 0;
                    surface_command.extra = NULL;

                    surface_command.codecId = RDPGFX_CODECID_ALPHA;
                    surface_command.length = alpha_size;
                    surface_command.data = &alpha[0];
                    client->rdpgfx_ctx->SurfaceCommand(client->rdpgfx_ctx, &surface_command);

                    surface_command.codecId = RDPGFX_CODECID_UNCOMPRESSED;
                    surface_command.length = damage_size;
                    surface_command.data = &data[0];
                    client->rdpgfx_ctx->SurfaceCommand(client->rdpgfx_ctx, &surface_command);
                    pixman_region32_clear(&window->render.damage);
                    window->render.dirty = false;
                    free(alpha);
                    free(data);
                }

            render_release:
                if (texture) { wlr_texture_destroy(texture); }
            }

            if (start_frame.frameId) {
                RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;
                RDPGFX_END_FRAME_PDU endFrame = { .frameId = start_frame.frameId };
                gfx_ctx->EndFrame(gfx_ctx, &endFrame);
            }
        }

    release:
        wl_array_for_each(node, &render_nodes) {
            wlr_buffer_unlock(node->window->render.buffer);
            node->window->render.buffer = NULL;
        }
        wl_array_release(&render_nodes);
    }
}

static void backend_window_buffer_resize(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;

    if (!window->metadata.ids.window || wlr_box_empty(&window->metadata.target)) {
        return;
    }

    if (adapter->backend->use_gfxredir) {
        GfxRedirServerContext *redir_ctx = adapter->backend->client->gfxredir_ctx;

        if (window->render.gfx_buffer) {
            adapter->handle->backend_window_buffer_destroy(window);
            window->render.gfx_memory.name[0] = '\0';
        }

        int buffer_bpp = 4;
        int width = window->metadata.target.width;
        int height = window->metadata.target.height;
        long page_size = sysconf(_SC_PAGESIZE);
        window->render.gfx_memory.size = width * height * buffer_bpp + (page_size - 1) & ~(page_size - 1);
        if (adapter->handle->backend_shared_memory_new(adapter, &window->render.gfx_memory)) {
            uint16_t section_name[BACKEND_SHARED_MEMORY_NAME_SIZE + 1];

            for (uint32_t idx = 0; idx < BACKEND_SHARED_MEMORY_NAME_SIZE; idx++) {
                section_name[idx] = (uint16_t)window->render.gfx_memory.name[idx];
            }
            section_name[BACKEND_SHARED_MEMORY_NAME_SIZE] = 0;

            GFXREDIR_OPEN_POOL_PDU open_pool_pdu = {0};
            open_pool_pdu.poolId = ++wsland_pool_id;
            open_pool_pdu.poolSize = window->render.gfx_memory.size;
            open_pool_pdu.sectionNameLength = BACKEND_SHARED_MEMORY_NAME_SIZE + 1;
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
                    window->render.gfx_buffer = window->render.gfx_memory.addr;
                    window->metadata.ids.buffer = create_buffer_pdu.bufferId;
                    window->metadata.ids.pool = create_buffer_pdu.poolId;
                } else {
                    adapter_log(ERROR, "redir ctx create buffer failed");
                }
            } else {
                adapter_log(ERROR, "redir ctx open pool failed");
            }

            if (!window->render.gfx_buffer) {
                adapter->handle->backend_window_buffer_destroy(window);
            }
        } else {
            adapter_log(ERROR, "backend shared memory create failed");
        }
    } else {
        uint32_t prev_surface_id = window->metadata.ids.surface;
        RdpgfxServerContext *gfx_ctx = adapter->backend->client->rdpgfx_ctx;

        RDPGFX_CREATE_SURFACE_PDU create_surface = {0};
        create_surface.surfaceId = ++wsland_surface_id;
        create_surface.height = window->metadata.target.height;
        create_surface.width = window->metadata.target.width;
        create_surface.pixelFormat = GFX_PIXEL_FORMAT_ARGB_8888;
        if (gfx_ctx->CreateSurface(gfx_ctx, &create_surface) == 0) {
            window->metadata.ids.surface = create_surface.surfaceId;
        }

        RDPGFX_MAP_SURFACE_TO_SCALED_WINDOW_PDU map_surface_to_window = {0};
        map_surface_to_window.windowId = window->metadata.ids.window;
        map_surface_to_window.surfaceId = window->metadata.ids.surface;
        map_surface_to_window.mappedWidth = window->metadata.target.width;
        map_surface_to_window.mappedHeight = window->metadata.target.height;
        map_surface_to_window.targetWidth = window->metadata.target.width;
        map_surface_to_window.targetHeight = window->metadata.target.height;
        gfx_ctx->MapSurfaceToScaledWindow(gfx_ctx, &map_surface_to_window);

        if (prev_surface_id) {
            RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
            deleteSurface.surfaceId = (uint16_t)prev_surface_id;
            gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        }
    }

    if (window->render.swapchain) {
        wlr_swapchain_destroy(window->render.swapchain);
        window->render.swapchain = NULL;
    }

    window->render.swapchain = wlr_swapchain_create(
        window->server->allocator, window->metadata.target.width, window->metadata.target.height,
        &(const struct wlr_drm_format) { .format = DRM_FORMAT_ARGB8888 }
    );
    pixman_region32_init_rect(
        &window->render.damage, 0, 0,
        window->metadata.target.width,
        window->metadata.target.height
    );
}

static void backend_window_title(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;

    if (!adapter->backend->client || !window->metadata.ids.window) {
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = window->metadata.ids.window;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    char *title = window->handle->fetch_title(window);
    RAIL_UNICODE_STRING backend_window_title = {.length = 0, .string = NULL};
    if (title) {
        if (utf8_string_to_rail_string(title, &backend_window_title)) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
            window_state_order.titleInfo = backend_window_title;
        }
    }

    rdpUpdate *update = adapter->backend->client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);
    free(backend_window_title.string);
}

static void backend_window_appid(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;

    if (!adapter->backend->client || !window->metadata.ids.window) {
        return;
    }
    RAIL_GET_APPID_RESP_EX resp = {0};
    resp.windowID = window->metadata.ids.window;

    size_t wchar_len = 0;
    WCHAR *wchar_data = ConvertUtf8NToWCharAlloc(window->metadata.appid, -1, &wchar_len);
    memcpy(resp.applicationID, wchar_data, wchar_len);

    RailServerContext *rail_ctx = adapter->backend->client->rail_ctx;
    rail_ctx->ServerGetAppidRespEx(rail_ctx, &resp);
}

static void backend_window_create(struct wsland_window *window) {
    struct wsland_server *server = window->server;
    struct wsland_adapter *adapter = server->adapter;

    struct backend_client *client;
    if ((client = adapter->backend->client) == NULL) {
        return;
    }

    window->server->service->window_title_reset(window);
    window->server->service->window_appid_reset(window);
    window->metadata.ids.window = ID_NEW(adapter->reconnect, window->metadata.ids.window, wsland_window_id);

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = window->metadata.ids.window;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_NEW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_STYLE;
    window_state_order.style = BACKEND_WINDOW_NORMAL_STYLE;
    window_state_order.extendedStyle = WS_EX_LAYERED;

    if (window->metadata.above) {
        window_state_order.extendedStyle = WS_EX_PALETTEWINDOW;
        window_state_order.style = WS_POPUPWINDOW;
    }

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_OWNER;
    window_state_order.ownerWindowId = window->parent ? window->parent->metadata.ids.window : BACKEND_WINDOW_DESKTOP_ID;

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
    window_state_order.windowOffsetX = window->metadata.target.x;
    window_state_order.windowOffsetY = window->metadata.target.y;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
    window_state_order.TaskbarButton = window->parent ? true : false;
    window_state_order.showState = WINDOW_HIDE;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
    window_state_order.clientAreaWidth = window->metadata.target.width;
    window_state_order.clientAreaHeight = window->metadata.target.height;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
    window_state_order.windowWidth = window->metadata.target.width;
    window_state_order.windowHeight = window->metadata.target.height;

    RECTANGLE_16 window_rect = {
        .left = 0,
        .top = 0,
        .right = window->metadata.target.width,
        .bottom = window->metadata.target.height
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
    window_state_order.numWindowRects = 1;
    window_state_order.windowRects = &window_rect;

    RECTANGLE_16 window_vis = {
        .left = 0,
         .top = 0,
         .right = window->metadata.target.width,
         .bottom = window->metadata.target.height,
    };

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
    window_state_order.numVisibilityRects = 1;
    window_state_order.visibilityRects = &window_vis;

    RAIL_UNICODE_STRING backend_window_title = {.length = 0, .string = NULL};
    if (window->metadata.title) {
        if (utf8_string_to_rail_string(window->metadata.title, &backend_window_title)) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_TITLE;
            window_state_order.titleInfo = backend_window_title;
        }
    }

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowCreate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);

    if (backend_window_title.string) {
        free(backend_window_title.string);
    }

    if (window->metadata.icon) {
        adapter->handle->backend_window_icon_sync(window);
    }

    struct wsland_output *output = window->handle->fetch_output(window);
    if (output) {
        RAIL_MINMAXINFO_ORDER minmax_order = {0};
        minmax_order.windowId = window->metadata.ids.window;
        minmax_order.maxPosX = 0;
        minmax_order.maxPosY = 0;
        minmax_order.maxWidth = (int16_t)output->full_area.width;
        minmax_order.maxHeight = (int16_t)output->full_area.height;
        minmax_order.maxTrackWidth = (int16_t)output->full_area.width;
        minmax_order.maxTrackHeight = (int16_t)output->full_area.height;
        minmax_order.minTrackHeight = 0;
        minmax_order.minTrackWidth = 0;
        adapter->backend->client->rail_ctx->ServerMinMaxInfo(adapter->backend->client->rail_ctx, &minmax_order);
    }
}

static void backend_window_minimize(struct wsland_window *window, bool minimize) {
    struct wsland_adapter *adapter = window->server->adapter;

    if (!adapter->backend->client || !window->metadata.ids.window) {
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = window->metadata.ids.window;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
    window_state_order.showState = minimize ? WINDOW_SHOW : WINDOW_SHOW_MINIMIZED;

    rdpUpdate *update = adapter->backend->client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);
}

static void backend_window_maximize(struct wsland_window *window, bool maximize) {
    struct wsland_adapter *adapter = window->server->adapter;

    if (!adapter->backend->client || !window->metadata.ids.window) {
        return;
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    WINDOW_STATE_ORDER window_state_order = {0};

    window_order_info.windowId = window->metadata.ids.window;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

    window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW;
    window_state_order.showState = maximize ? WINDOW_SHOW_MAXIMIZED : WINDOW_SHOW;

    rdpUpdate *update = adapter->backend->client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
    update->EndPaint(update->context);
}

static void backend_window_reconnect(struct wsland_adapter *adapter) {
    struct wsland_window *window, *temp;
    wl_list_for_each_safe(window, temp, &adapter->server->windows, link) {
        window->render.center = false;
        window->render.pending = false;
        window->render.resizing = false;
        window->metadata.geometry = (struct wlr_box) {0};
        window->metadata.source = (struct wlr_box) {0};
        window->metadata.target = (struct wlr_box) {0};
        window->metadata.ids.surface = 0;
        window->metadata.ids.buffer = 0;
        window->metadata.ids.pool = 0;

        if (window->metadata.mapped && !window->scene->node.enabled) {
            wlr_scene_node_set_enabled(&window->scene->node, true);
        }
        adapter->handle->backend_window_create(window);
    }
}

static cairo_status_t fetch_icon_data_func(void *closure, unsigned char *data, unsigned int length) {
    FILE *fp = closure;

    size_t n = fread(data, 1, length, fp);
    return n != length ? CAIRO_STATUS_READ_ERROR : CAIRO_STATUS_SUCCESS;
}

static void backend_window_icon_sync(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;

    struct backend_client *client = adapter->backend->client;
    if (!client || !window->metadata.ids.window) {
        return;
    }

    char *appid = window->handle->fetch_appid(window);
    if (!appid) { return; }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "/usr/local/bin/query_icon %s", appid);

    FILE *fetch_icon_path = popen(cmd, "r");
    if (!fetch_icon_path) { return; }

    char command[4096];
    if (fgets(command, sizeof(command), fetch_icon_path)) {
        command[strcspn(command, "\n")] = 0;

        FILE *fetch_icon_data = popen(command, "r");
        if (!fetch_icon_data) { return; }

        cairo_surface_t *source = cairo_image_surface_create_from_png_stream(fetch_icon_data_func, fetch_icon_data);
        if (cairo_surface_status(source) == CAIRO_STATUS_SUCCESS) {
            int width = cairo_image_surface_get_width(source);
            int height = cairo_image_surface_get_height(source);

            if (width > 0 && height > 0) {
                bool bits_color_allocated = false;
                void *bits_color = NULL, *bits_mask = NULL;
                cairo_surface_t *target = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 96);
                cairo_t *cr = cairo_create(target);

                cairo_scale(cr, 96.0 / width, 96.0 / height);
                cairo_set_source_surface(cr, source, 0, 0);
                cairo_paint(cr);
                cairo_destroy(cr);

                int w = cairo_image_surface_get_width(target);
                int h = cairo_image_surface_get_height(target);
                int s = cairo_image_surface_get_stride(target);

                int stride_color = WINDOW_ORDER_ICON_ROWLENGTH(w, 32);
                int size_color = stride_color * h;

                if (stride_color != s) {
                    size_color = stride_color * h;
                    bits_color = malloc(size_color);
                    bits_color_allocated = true;
                } else {
                    bits_color = cairo_image_surface_get_data(target);
                }

                int stride_mask = WINDOW_ORDER_ICON_ROWLENGTH(w, 1);
                int size_mask = stride_mask * h;

                bits_mask = malloc(size_mask);
                char *src_color = (char *)cairo_image_surface_get_data(target);
                char *dst_color = bits_color;
                char *dst_mask = bits_mask;

                for (int i = 0; i < h; i++) {
                    uint32_t *src = (uint32_t *)src_color;
                    uint32_t *dst = (uint32_t *)dst_color;
                    char *mask = dst_mask;

                    for (int j = 0; j < w; j++) {
                        if (dst != src) {
                            *dst = *src;
                        }
                        if (*dst & 0xFF000000) {
                            mask[j / 8] |= 0x80 >> (j % 8); // NOLINT(*-narrowing-conversions)
                        }
                        dst++;
                        src++;
                    }
                    dst_color += stride_color;
                    dst_mask += stride_mask;
                    src_color += s;
                }


                WINDOW_ORDER_INFO order_info = {0};
                WINDOW_ICON_ORDER icon_order = {0};
                ICON_INFO icon_info = {0};

                order_info.windowId = window->metadata.ids.window;
                order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_ICON;
                icon_info.cacheEntry = 0xFFFF;
                icon_info.cacheId = 0xFF;
                icon_info.bpp = 32;
                icon_info.width = w;
                icon_info.height = h;
                icon_info.cbColorTable = 0;
                icon_info.cbBitsMask = size_mask;
                icon_info.cbBitsColor = size_color;
                icon_info.bitsMask = bits_mask;
                icon_info.colorTable = NULL;
                icon_info.bitsColor = bits_color;
                icon_order.iconInfo = &icon_info;

                rdpUpdate *update = client->peer->context->update;
                update->BeginPaint(update->context);
                update->window->WindowIcon(update->context, &order_info, &icon_order);
                update->EndPaint(update->context);

                free(bits_mask);
                if (bits_color_allocated) { free(bits_color); }
                cairo_surface_destroy(target);
            }
        }
        cairo_surface_destroy(source);
        pclose(fetch_icon_data);
    }
    pclose(fetch_icon_path);
}

static void backend_window_destroy(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;

    struct backend_client *client = adapter->backend->client;
    if (!client || !window->metadata.ids.window) {
        return;
    }

    if (window->metadata.ids.surface || window->metadata.ids.buffer) {
        int waitRetry = 0;
        while (window->render.pending || client->current_frame_id != client->acknowledged_frame_id) {
            if (++waitRetry > 1000) {
                break;
            }
            usleep(10000);
            client->peer->CheckFileDescriptor(client->peer);
            WTSVirtualChannelManagerCheckFileDescriptor(client->vcm);
        }
    }

    if (adapter->backend->use_gfxredir) {
        adapter->handle->backend_window_buffer_destroy(window);
    }

    WINDOW_ORDER_INFO window_order_info = {0};
    window_order_info.windowId = window->metadata.ids.window;
    window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW | WINDOW_ORDER_STATE_DELETED;

    rdpUpdate *update = client->peer->context->update;
    update->BeginPaint(update->context);
    update->window->WindowDelete(update->context, &window_order_info);
    update->EndPaint(update->context);

    if (window->metadata.ids.surface) {
        RDPGFX_DELETE_SURFACE_PDU deleteSurface = {0};
        deleteSurface.surfaceId = (uint16_t)window->metadata.ids.surface;

        RdpgfxServerContext *gfx_ctx = client->rdpgfx_ctx;
        gfx_ctx->DeleteSurface(gfx_ctx, &deleteSurface);
        window->metadata.ids.surface = 0;
    }

    if (window->render.swapchain) {
        wlr_swapchain_destroy(window->render.swapchain);
        window->render.swapchain = NULL;
    }

    if (window->buffer_border) {
        wlr_scene_buffer_set_buffer(window->scene_border, NULL);
        wlr_buffer_drop(&window->buffer_border->buffer);
        window->buffer_border = NULL;
    }

    if (window->server->grab.window == window) {
        window->server->grab.window = NULL;
    }

    pixman_region32_fini(&window->render.damage);
    window->metadata.geometry = (struct wlr_box) {0};
    window->metadata.source = (struct wlr_box) {0};
    window->metadata.target = (struct wlr_box) {0};
    window->metadata.ids.window = 0;
}

static void backend_window_metadata_detect(struct wsland_window *window) {
    struct wsland_server *server = window->server;
    struct wsland_adapter *adapter = server->adapter;

    struct backend_client *client = adapter->backend->client;
    if (!client || !window->metadata.ids.window) {
        return;
    }

    bool move = false, resize = false;
    if (server->service->window_geometry_resize(window)) {
        struct wlr_box box = { .width = window->metadata.source.width, .height = window->metadata.source.height };
        window->server->service->scale_box(window->server, &box, window->render.wo->scale.desktop);
        window->metadata.target.height = box.height;
        window->metadata.target.width = box.width;
        resize = true;
    }

    if (window->render.center) {
        window->handle->window_center(window);
        window->render.center = false;
    }

    if (server->service->window_position_move(window)) {
        move = true;
    }

    if (move || resize) {
        WINDOW_ORDER_INFO window_order_info = {0};
        WINDOW_STATE_ORDER window_state_order = {0};

        window_order_info.windowId = window->metadata.ids.window;
        window_order_info.fieldFlags = WINDOW_ORDER_TYPE_WINDOW;

        if (move) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_OFFSET;
            window_state_order.windowOffsetX = window->metadata.target.x;
            window_state_order.windowOffsetY = window->metadata.target.y;
        }

        if (resize) {
            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_SHOW | WINDOW_ORDER_FIELD_TASKBAR_BUTTON;
            window_state_order.showState = wlr_box_empty(&window->metadata.target) ? WINDOW_HIDE : WINDOW_SHOW;
            window_state_order.TaskbarButton = window->parent ? 1 : 0;

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_CLIENT_AREA_SIZE;
            window_state_order.clientAreaHeight = window->metadata.target.height;
            window_state_order.clientAreaWidth = window->metadata.target.width;

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_SIZE;
            window_state_order.windowHeight = window->metadata.target.height;
            window_state_order.windowWidth = window->metadata.target.width;

            RECTANGLE_16 window_rect = {
                .left = 0,
                .top = 0,
                .right = window->metadata.target.width,
                .bottom = window->metadata.target.height,
            };

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_WND_RECTS;
            window_state_order.numWindowRects = 1;
            window_state_order.windowRects = &window_rect;

            RECTANGLE_16 window_vis = {
                .left = 0,
                 .top = 0,
                 .right = window->metadata.target.width,
                 .bottom = window->metadata.target.height,
            };

            window_order_info.fieldFlags |= WINDOW_ORDER_FIELD_VISIBILITY;
            window_state_order.numVisibilityRects = 1;
            window_state_order.visibilityRects = &window_vis;
        }

        rdpUpdate *update = client->peer->context->update;
        update->BeginPaint(update->context);
        update->window->WindowUpdate(update->context, &window_order_info, &window_state_order);
        update->EndPaint(update->context);

        if (resize) {
            adapter->handle->backend_window_buffer_resize(window);
        }
    }
}

static void backend_window_buffer_destroy(struct wsland_window *window) {
    struct wsland_adapter *adapter = window->server->adapter;
    GfxRedirServerContext *redir_ctx = adapter->backend->client->gfxredir_ctx;

    if (!adapter->reconnect) {
        if (window->metadata.ids.buffer) {
            GFXREDIR_DESTROY_BUFFER_PDU destroy_buffer_pdu = {0};
            destroy_buffer_pdu.bufferId = window->metadata.ids.buffer;
            redir_ctx->DestroyBuffer(redir_ctx, &destroy_buffer_pdu);
            window->metadata.ids.buffer = 0;
        }

        if (window->metadata.ids.pool) {
            GFXREDIR_CLOSE_POOL_PDU close_pool_pdu = {0};
            close_pool_pdu.poolId = window->metadata.ids.pool;
            redir_ctx->ClosePool(redir_ctx, &close_pool_pdu);
            window->metadata.ids.pool = 0;
        }
    }

    adapter->handle->backend_shared_memory_free(adapter, &window->render.gfx_memory);
    window->render.gfx_buffer = NULL;
}

static bool backend_shared_memory_new(struct wsland_adapter *adapter, struct wsland_shared_memory *shared_memory) {
    int fd = -1;
    void *addr = NULL;
    char path[adapter->backend->shared_memory_mount_point_size + 1 + BACKEND_SHARED_MEMORY_NAME_SIZE + 1];

    if (shared_memory->size <= 0) {
        goto release;
    }

    if (shared_memory->name[0] == '\0') {
        int fd_uuid = open("/proc/sys/kernel/random/uuid", O_RDONLY);
        if (fd_uuid < 0) {
            goto release;
        }
        if (read(fd_uuid, &shared_memory->name[1], 32 + 4) < 0) {
            goto release;
        }
        close(fd_uuid);
        shared_memory->name[0] = '{';
        shared_memory->name[BACKEND_SHARED_MEMORY_NAME_SIZE - 1] = '}';
        shared_memory->name[BACKEND_SHARED_MEMORY_NAME_SIZE] = '\0';
    } else if (strlen(shared_memory->name) != BACKEND_SHARED_MEMORY_NAME_SIZE ||
        shared_memory->name[0] != '{' ||
        shared_memory->name[BACKEND_SHARED_MEMORY_NAME_SIZE - 1] != '}' ||
        shared_memory->name[BACKEND_SHARED_MEMORY_NAME_SIZE] != '\0') {
        goto release;
        }

    strcpy(path, adapter->backend->shared_memory_mount_point);
    strcat(path, "/");
    strcat(path, shared_memory->name);

    fd = open(path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
    if (fd < 0) {
        goto release;
    }

    if (fallocate(fd, 0, 0, (long)shared_memory->size) < 0) {
        goto release;
    }

    addr = mmap(NULL, shared_memory->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        goto release;
    }

    shared_memory->fd = fd;
    shared_memory->addr = addr;
    return true;

release:
    if (fd > 0) {
        close(fd);
    }
    if (addr) {
        munmap(addr, shared_memory->size);
    }

    shared_memory->fd = -1;
    shared_memory->addr = NULL;
    return false;
}

static void backend_shared_memory_free(struct wsland_adapter *adapter, struct wsland_shared_memory *shared_memory) {
    if (!shared_memory) { return; }

    if (shared_memory->addr) {
        munmap(shared_memory->addr, shared_memory->size);
        shared_memory->addr = NULL;
    }
    if (shared_memory->fd > 0) {
        close(shared_memory->fd);
        shared_memory->fd = -1;
    }
}

static void backend_shared_memory_config(struct wsland_adapter *adapter) {
    bool use_gfxredir = false;
    char *mount_point = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
    if (mount_point) {
        adapter->backend->shared_memory_mount_point = mount_point;
        adapter->backend->shared_memory_mount_point_size = strlen(adapter->backend->shared_memory_mount_point);
        use_gfxredir = true;
    }

    if (use_gfxredir) {
        use_gfxredir = false;

        for (int idx = 1; idx <=3; ++idx) {
            struct wsland_shared_memory shared_memory = {0};
            shared_memory.size = sysconf(_SC_PAGESIZE);
            if (adapter->handle->backend_shared_memory_new(adapter, &shared_memory)) {
                *(uint32_t *)shared_memory.addr = 0x12344321;
                adapter->handle->backend_shared_memory_free(adapter, &shared_memory);
                use_gfxredir = true;
                break;
            }

            backend_log(ERROR, "use gfxredir, but shared memory failed, try: %d", idx);
            sleep(3);
        }

        if (!use_gfxredir) {
            backend_log(ERROR, "use gfxredir, but shared memory failed, try exit");
            exit(0);
        }
    }

    adapter->backend->use_gfxredir = use_gfxredir;
}

static const struct wsland_adapter_handle adapter_handle = {
    .server_dispatch_event_source = server_dispatch_event_source,
    .server_remove_event_source = server_remove_event_source,
    .server_seat_source_config = server_seat_source_config,
    .server_seat_fetch = server_seat_fetch,
    .server_output_from_point = server_output_from_point,
    .find_bottom_window = find_bottom_window,

    .backend_pointer_init = backend_pointer_init,
    .backend_keyboard_init = backend_keyboard_init,
    .backend_source_release = backend_source_release,
    .backend_selection_source = backend_selection_source,
    .backend_input_synchronize = backend_input_synchronize,

    .backend_rail_client_activate = backend_rail_client_activate,
    .backend_rail_client_sysparam = backend_rail_client_sysparam,
    .backend_rail_client_syscommand = backend_rail_client_syscommand,
    .backend_rail_client_appid_response = backend_rail_client_appid_response,
    .backend_adjust_monitor_layout = backend_adjust_monitor_layout,
    .backend_rdpgfx_frame_acknowledge = backend_rdpgfx_frame_acknowledge,
    .backend_gfxredir_frame_acknowledge = backend_gfxredir_frame_acknowledge,

    .backend_mouse_move = backend_mouse_move,
    .backend_mouse_axis = backend_mouse_axis,
    .backend_mouse_frame = backend_mouse_frame,
    .backend_mouse_button = backend_mouse_button,
    .backend_keyboard_key = backend_keyboard_key,
    .backend_cursor_update = backend_cursor_update,

    .backend_output_frame = backend_output_frame,
    .backend_window_title = backend_window_title,
    .backend_window_appid = backend_window_appid,
    .backend_window_create = backend_window_create,
    .backend_window_destroy = backend_window_destroy,
    .backend_window_minimize = backend_window_minimize,
    .backend_window_maximize = backend_window_maximize,
    .backend_window_reconnect = backend_window_reconnect,
    .backend_window_icon_sync = backend_window_icon_sync,
    .backend_window_buffer_resize = backend_window_buffer_resize,
    .backend_window_buffer_destroy = backend_window_buffer_destroy,
    .backend_window_metadata_detect = backend_window_metadata_detect,
    .backend_shared_memory_new = backend_shared_memory_new,
    .backend_shared_memory_free = backend_shared_memory_free,
    .backend_shared_memory_config = backend_shared_memory_config,
};

void wsland_adapter_handle_init(struct wsland_adapter *adapter) {
    adapter->handle = &adapter_handle;
}