#ifndef WSLAND_UTILS_CONFIG_H
#define WSLAND_UTILS_CONFIG_H

typedef struct wsland_config {
    char *command;
    char *socket;
    char *log;

    char *address;
    int port;
} wsland_config;

wsland_config *wsland_config_create(int argc, char *argv[]);
void wsland_config_destroy(wsland_config *config);
#endif
