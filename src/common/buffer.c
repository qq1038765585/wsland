#include <stdlib.h>
#include <drm/drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/pass.h>

#include "common/buffer.h"


static void cairo_buffer_destroy(struct wlr_buffer* wb) {
    struct wsland_buffer* buffer = wl_container_of(wb, buffer, buffer);

    wlr_buffer_finish(wb);
    cairo_surface_destroy(buffer->surface);
    free(buffer);
}

static bool cairo_buffer_begin_data_ptr_access(struct wlr_buffer* wb, uint32_t flags, void** d, uint32_t* f, size_t* s) {
    struct wsland_buffer* buffer = wl_container_of(wb, buffer, buffer);

    if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
        return false;
    }

    *f = DRM_FORMAT_ARGB8888;
    *d = cairo_image_surface_get_data(buffer->surface);
    *s = cairo_image_surface_get_stride(buffer->surface);
    return true;
}

static void cairo_buffer_end_data_ptr_access(struct wlr_buffer* wlr_buffer) {

}

static const struct wlr_buffer_impl wsland_buffer_impl = {
    .begin_data_ptr_access = cairo_buffer_begin_data_ptr_access,
    .end_data_ptr_access = cairo_buffer_end_data_ptr_access,
    .destroy = cairo_buffer_destroy,
};

struct wsland_buffer* wsland_buffer_create(int width, int height) {
    struct wsland_buffer* buffer = calloc(1, sizeof(*buffer));
    if (!buffer) { return NULL; }

    wlr_buffer_init(&buffer->buffer, &wsland_buffer_impl, width, height);
    buffer->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

    if (cairo_surface_status(buffer->surface) != CAIRO_STATUS_SUCCESS) {
        free(buffer);
        return NULL;
    }

    return buffer;
}

static void cairo_arc_border(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_path(cr);
    cairo_move_to(cr, x + r, y);
    cairo_line_to(cr, x + w - r, y);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_line_to(cr, x + w, y + h - r);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_line_to(cr, x + r, y + h);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_line_to(cr, x, y + r);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

void wsland_mask_border(struct wlr_buffer *b, double width, double height, double r, double bw) {
    double x = bw / 4, y = bw / 4, w = width - bw / 2, h = height - bw / 2;

    void *data;
    size_t stride;
    uint32_t format;
    if (wlr_buffer_begin_data_ptr_access(b, WLR_BUFFER_DATA_PTR_ACCESS_READ|WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &format, &stride)) {
        cairo_surface_t *surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, b->width, b->height, (int)stride);

        cairo_t *cr = cairo_create(surface);
        cairo_set_operator(cr, CAIRO_OPERATOR_DEST_IN);
        cairo_set_source_rgba(cr, 1,1,1, 1);
        cairo_arc_border(cr, x, y, w, h, r);
        cairo_fill(cr);
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        wlr_buffer_end_data_ptr_access(b);
    }
}

void wsland_draw_border(cairo_surface_t *s, double width, double height, double r, double bw, struct wlr_render_color *c) {
    double x = bw / 2, y = bw / 2, w = width - bw, h = height - bw;

    cairo_t *cr = cairo_create(s);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_line_width(cr, bw);
    cairo_set_source_rgba(cr, c->r, c->g, c->b, c->a);
    cairo_arc_border(cr, x, y, w, h, r);
    cairo_stroke(cr);
    cairo_destroy(cr);
}
