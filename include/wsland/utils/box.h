#ifndef WSLAND_UTILS_BOX_H
#define WSLAND_UTILS_BOX_H

#include <pixman.h>
#include <wlr/util/box.h>

struct wlr_box region_to_box(pixman_region32_t *region);

#endif
