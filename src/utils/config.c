#include <stdlib.h>
#include <getopt.h>
#include <string.h>

#include "wsland/utils/log.h"
#include "wsland/utils/config.h"

wsland_config* wsland_config_create(int argc, char *argv[]) {
    wsland_config *config = calloc(1, sizeof(*config));
    if (!config) {
        wsland_log(CONFIG, ERROR, "calloc failed for wsland_config");
        goto create_failed;
    }

    { // parse args
        int c;
        while ((c = getopt(argc, argv, "c:s:l:h")) != -1) {
            switch (c) {
            case 'c':
                config->command = optarg;
                break;
            case 's':
                config->socket = optarg;
                break;
            case 'l':
                config->log = optarg;
                break;
            default:
                wsland_log(CONFIG, INFO, "usage: %s [-s command]", argv[0]);
                goto create_failed;
            }
        }

        if (optind < argc) {
            wsland_log(CONFIG, ERROR, "usage: %s [-s command]", argv[0]);
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
                wsland_log(CONFIG, ERROR, "expected env [ WSLAND_PORT ] to be a positive integer less or equal to 65535");
                goto create_failed;
            }
            config->port = port;
        }
    }

    return config;
create_failed:
    wsland_config_destroy(config);
    return NULL;
}

void wsland_config_destroy(wsland_config *config) {
    if (config) {
        free(config);
    }
}
