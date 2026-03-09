#ifndef WSLAND_UTILS_BOX_H
#define WSLAND_UTILS_BOX_H

#include <pixman.h>
#include <wlr/util/box.h>

void region_to_box(pixman_region32_t *region, struct wlr_box *box);

#endif
