// ReSharper disable All

#include "wsland.h"

static struct wl_client *request_launch_shell_process(void *shell_context, char *exec_name) {
    struct wl_client *test = malloc(sizeof test); // todo fix
    return test;
}

static const struct wsland_freerdp_shell_api freerdp_shell_api = {
    .request_launch_shell_process = request_launch_shell_process
};

const struct wsland_freerdp_shell_api * wsland_freerdp_shell_api_init(void) {
    return &freerdp_shell_api;
}