#include <assert.h>

#include "wsland/utils/box.h"

void region_to_box(pixman_region32_t *region, struct wlr_box *box) {
    assert(region);
    assert(box);

    box->x = region->extents.x1;
    box->y = region->extents.y1;
    box->width = region->extents.x2 - region->extents.x1;
    box->height = region->extents.y2 - region->extents.y1;
}