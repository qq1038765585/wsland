// ReSharper disable All
#include <wlr/types/wlr_scene.h>

#include "wsland.h"

static uint32_t next_window_id = 1;
static uint32_t next_surface_id = 1;

int wsland_freerdp_instance_new_surface(void) {
    return next_surface_id++;
}

int wsland_freerdp_instance_new_window(void) {
    while (true) {
        bool exists = false;
        struct wlr_scene_node *node;
        wl_list_for_each(node, &server.scene->tree.children, link) {
            struct wsland_toplevel *toplevel = node->data;

            if (toplevel->state.window_id == next_window_id) {
                next_window_id++;
                exists = true;
                break;
            }
        }

        if (!exists) {
            return next_window_id;
        }
    }
}

struct wsland_toplevel * wsland_freerdp_instance_loop_wsland_surface(struct wsland_peer_context *peer_ctx, uint32_t window_id) {
    struct wlr_scene_node *node;
    wl_list_for_each(node, &server.scene->tree.children, link) {
        struct wsland_toplevel *toplevel = node->data;

        if (toplevel->state.window_id == window_id) {
            return toplevel;
        }
    }
    return NULL;
}