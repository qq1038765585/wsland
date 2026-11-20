#ifndef WSLAND_COMMON_BUFFER_H
#define WSLAND_COMMON_BUFFER_H

#include <wlr/types/wlr_buffer.h>
#include <cairo.h>

struct wsland_buffer {
    struct wlr_buffer buffer;
    cairo_surface_t* surface;
};

void wsland_mask_border(struct wlr_buffer *b, double width, double height, double r, double bw);
void wsland_draw_border(cairo_surface_t *s, double width, double height, double r, double bw, struct wlr_render_color *c);
struct wsland_buffer* wsland_buffer_create(int width, int height);
#endif // WSLAND_COMMON_BUFFER_H
