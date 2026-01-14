#include <wlr/util/log.h>

#include "wsland.h"

int main(const int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    return wsland_server_init(argc, argv) ? 0 : -1;
}
