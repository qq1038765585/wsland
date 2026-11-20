#ifndef WSLAND_COMMON_CONFIG_H
#define WSLAND_COMMON_CONFIG_H

#include <stdbool.h>
#include <wlr/render/pass.h>
#include <xkbcommon/xkbcommon.h>


struct wsland_server;

typedef void(*key_action)(struct wsland_server *server);

struct keybinding {
    uint32_t modifiers;
    xkb_keysym_t keysym;

    key_action action;
};

struct wsland_config {
    char *custom;
    char *socket;
    char *logger;

    int port;
    char *address;

    struct {
        bool scale;
    } output;

    struct {
        char *layout;
        char *variant;
        char *options;
    } keyboard;

    struct {
        int width;
        int radius;
        int column_gaps;
        struct wlr_render_color active_color;
        struct wlr_render_color inactive_color;
    } border;

    struct keybinding bindings[128];
    int binding_count;
};

void wsland_config_custom(struct wsland_config *config, struct wsland_server *server);

struct wsland_config *wsland_config_create(int argc, char *argv[]);
void wsland_config_destroy(struct wsland_config *config);
#endif // WSLAND_COMMON_CONFIG_H
