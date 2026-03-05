#include <assert.h>

#include "wsland/utils/box.h"

struct wlr_box region_to_box(pixman_region32_t *region) {
    assert(region);

    return (struct wlr_box) {
        region->extents.x1, region->extents.y1,
        region->extents.x2 - region->extents.x1,
        region->extents.y2 - region->extents.y1,
    };
}