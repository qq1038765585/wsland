#include <stdlib.h>

#include "common/logger.h"
#include "adapter.h"


struct wsland_adapter* wsland_adapter_create(void) {
    struct wsland_adapter *adapter = calloc(1, sizeof(*adapter));
    if (!adapter) {
        adapter_log(ERROR, "calloc failed for wsland_adapter");
        return NULL;
    }

    wsland_adapter_handle_init(adapter);
    if (!adapter->handle) {
        adapter_log(ERROR, "failed to invoke wsland_adapter_handle_init");
        goto create_failed;
    }
    return adapter;

create_failed:
    wsland_adapter_destroy(adapter);
    return NULL;
}

void wsland_adapter_destroy(struct wsland_adapter *adapter) {
    if (adapter) {
        free(adapter);
    }
}