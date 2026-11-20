#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <getopt.h>
#include <string.h>
#include <ini.h>

#include <wlr/types/wlr_keyboard.h>

#include "common/config.h"
#include "common/logger.h"
#include "server.h"


static FILE *log_file;
static enum wlr_log_importance g_level = WLR_INFO;
static void wsland_redirect_logger(enum wlr_log_importance importance, const char *fmt, va_list args) {
    if (!log_file || importance > g_level) { return; }

    const char *level = "UKN";
    switch (importance) {
        case WLR_ERROR:
            level = "ERR";
            break;
        case WLR_INFO:
            level = "INF";
            break;
        case WLR_DEBUG:
            level = "DBG";
            break;
        default:
            break;
    }

    struct tm tm;
    time_t t = time(NULL);
    localtime_r(&t, &tm);

    fprintf(
        log_file, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, level
    );
    vfprintf(log_file, fmt, args);
    fputc('\n', log_file);
}

static bool parse_rgba(const char *str_hex, struct wlr_render_color *color) {
    if (!str_hex) { return false; }
    if (str_hex[0] != '#') { return false; }

    char *end;
    uint32_t value;
    size_t len = strlen(str_hex);

    if (len == 7) {
        value = strtoul(str_hex + 1, &end, 16);
        if (*end != '\0') { return false; }

        color->r = (float)((value >> 16) & 0xff) / 255.0f;
        color->g = (float)((value >> 8) & 0xff) / 255.0f;
        color->b = (float)(value & 0xff) / 255.0f;
        color->a = 1.0f;
        return true;
    }

    if (len == 9) {
        value = strtoul(str_hex + 1, &end, 16);
        if (*end != '\0') { return false; }

        color->r = (float)((value >> 24) & 0xff) / 255.0f;
        color->g = (float)((value >> 16) & 0xff) / 255.0f;
        color->b = (float)((value >> 8) & 0xff) / 255.0f;
        color->a = (float)(value & 0xff) / 255.0f;
        return true;
    }

    return false;
}

static int parse_config_handler(void* user, const char* section, const char* name, const char* value) {
    struct wsland_server *server = user;

    if (strcmp("output", section) == 0) {
        if (strcmp("scale", name) == 0) {
            server->config->output.scale = strcmp("true", value) == 0;
        }
    }
    else if (strcmp("keyboard", section) == 0) {
        if (strcmp("layout", name) == 0) {
            server->config->keyboard.layout = strdup(value);
        }
        else if (strcmp("variant", name) == 0) {
            server->config->keyboard.variant = strdup(value);
        }
        else if (strcmp("options", name) == 0) {
            server->config->keyboard.options = strdup(value);
        }
    }
    else if (strcmp("border", section) == 0) {
        if (strcmp("width", name) == 0) {
            server->config->border.width = atoi(value); // NOLINT(*-err34-c)
        }
        else if (strcmp("radius", name) == 0) {
            server->config->border.radius = atoi(value); // NOLINT(*-err34-c)
        }
        else if (strcmp("column-gaps", name) == 0) {
            server->config->border.column_gaps = atoi(value); // NOLINT(*-err34-c)
        }
        else if (strcmp("active-color", name) == 0) {
            parse_rgba(value, &server->config->border.active_color);
        }
        else if (strcmp("inactive-color", name) == 0) {
            parse_rgba(value, &server->config->border.inactive_color);
        }
    }
    else if (strcmp("binding", section) == 0) {
        char *temp;
        char *token = strtok_r((char*)name, "+", &temp);

        struct keybinding *binding = &server->config->bindings[server->config->binding_count];
        while (token) {
            if (strcmp("Alt", token) == 0) {
                binding->modifiers |= WLR_MODIFIER_ALT;
            }
            else if (strcmp("Shift", token) == 0) {
                binding->modifiers |= WLR_MODIFIER_SHIFT;
            }
            else if (strcmp("Ctrl", token) == 0) {
                binding->modifiers |= WLR_MODIFIER_CTRL;
            }
            else if (strcmp("Super", token) == 0) {
                binding->modifiers |= WLR_MODIFIER_LOGO;
            }
            else {
                binding->keysym = xkb_keysym_from_name(token, XKB_KEYSYM_CASE_INSENSITIVE);
            }

            token = strtok_r(NULL, "+", &temp);
        }

        if (strcmp("kill", value) == 0) {
            binding->action = server->service->window_kill_action;
        }
        else if (strcmp("center", value) == 0) {
            binding->action = server->service->window_center_action;
        }
        else if (strcmp("maximize", value) == 0) {
            binding->action = server->service->window_maximize_action;
        }
        else if (strcmp("fullscreen", value) == 0) {
            binding->action = server->service->window_fullscreen_action;
        }
        else if (strcmp("column-maximize", value) == 0) {
            binding->action = server->service->window_column_maximize_action;
        }
        server->config->binding_count++;
    }

    char *layout;
    if ((layout = server->config->keyboard.layout) != NULL) {
        char *variant = server->config->keyboard.variant;
        char *options = server->config->keyboard.options;

        struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        struct xkb_rule_names rules = { .layout = layout, .variant = variant, .options = options, };
        struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
        if (!keymap) {
            xkb_context_unref(context);
            wlr_log(WLR_ERROR, "keyboard config invalid, please check: layout = %s, variant = %s, options = %s", layout, variant, options);
            exit(EXIT_FAILURE);
            return false;
        }
    }

    return true;
}

void wsland_config_custom(struct wsland_config *config, struct wsland_server *server) {
    server->config = config;

    config->border.width = 2;
    config->border.radius = 12;
    config->border.column_gaps = 8;
    config->border.active_color = (struct wlr_render_color) { .r = 0, .g = 0, .b = 1, .a = 1 };
    config->border.inactive_color = (struct wlr_render_color) { .r = 0, .g = 0, .b = 0, .a = 0 };
    config->output.scale = false;

    if (config->custom) {
        ini_parse(config->custom, parse_config_handler, server);
    }

    if (config->binding_count <= 0) {
        struct keybinding *binding = &server->config->bindings[0];
        binding->modifiers = WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT;
        binding->action = server->service->window_kill_action;
        binding->keysym = XKB_KEY_q;

        binding = &server->config->bindings[1];
        binding->modifiers = WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT;
        binding->action = server->service->window_center_action;
        binding->keysym = XKB_KEY_c;

        binding = &server->config->bindings[2];
        binding->modifiers = WLR_MODIFIER_ALT | WLR_MODIFIER_SHIFT;
        binding->action = server->service->window_column_maximize_action;
        binding->keysym = XKB_KEY_f;

        config->binding_count = 3;
    }
}

static void wsland_config_logger(struct wsland_config *config) {
    log_file = config->logger ? fopen(config->logger, "a") : NULL;
    wlr_log_init(g_level, config->logger ? wsland_redirect_logger : NULL);

    if (log_file) {
        setvbuf(log_file, NULL, _IOLBF, 0);
        fprintf(log_file, "#####   wsland reboot   #####\n\n");
    }
}

struct wsland_config* wsland_config_create(const int argc, char *argv[]) {
    struct wsland_config *config = calloc(1, sizeof(*config));
    if (!config) {
        fprintf(stderr, "calloc failed for wsland_config\n");
        goto create_failed;
    }

    { // parse args
        int c;
        while ((c = getopt(argc, argv, "c:s:l:h")) != -1) {
            switch (c) {
                case 'c':
                    config->custom = optarg;
                    break;
                case 's':
                    config->socket = optarg;
                    break;
                case 'l':
                    config->logger = optarg;
                    break;
                default:
                    printf("usage: %s [-c custom | -s socket | -l logger]\n", argv[0]);
                    goto create_failed;
            }
        }

        if (optind < argc) {
            fprintf(stderr, "usage: %s [-c custom | -s socket | -l logger]\n", argv[0]);
            goto create_failed;
        }
    }

    { // parse envs
        config->address = "0.0.0.0";
        config->port = 3389;

        const char *address = getenv("WSLAND_ADDR");
        if (address) {
            config->address = strdup(address);
        }

        const char *temp_port = getenv("WSLAND_PORT");
        if (temp_port) {
            char *end_ptr;
            int port = (int)strtol(temp_port, &end_ptr, 10);
            if (*end_ptr || port <= 0 || port > 65535) {
                fprintf(stderr, "expected env [ WSLAND_PORT ] to be a positive integer less or equal to 65535\n");
                goto create_failed;
            }
            config->port = port;
        }
    }

    wsland_config_logger(config);
    return config;
create_failed:
    wsland_config_destroy(config);
    return NULL;
}

void wsland_config_destroy(struct wsland_config *config) {
    if (config) {
        if (log_file) {
            fclose(log_file);
            log_file = NULL;
        }
        if (config->keyboard.layout) {
            free(config->keyboard.layout);
            config->keyboard.layout = NULL;
        }
        if (config->keyboard.variant) {
            free(config->keyboard.variant);
            config->keyboard.variant = NULL;
        }
        if (config->keyboard.options) {
            free(config->keyboard.options);
            config->keyboard.options = NULL;
        }
        free(config);
    }
}