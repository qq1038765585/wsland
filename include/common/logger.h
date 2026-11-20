#ifndef WSLAND_COMMON_LOG_H
#define WSLAND_COMMON_LOG_H

#include <wlr/util/log.h>

enum wsland_log_type {
    ADAPTER, BACKEND, SERVER
};

enum wsland_log_level {
    DEBUG, INFO, ERROR
};

enum wlr_log_importance to_wlr(enum wsland_log_level level);

#define server_log(level, fmt, ...) wlr_log(to_wlr(level), "[wsland.server] " fmt, ##__VA_ARGS__);
#define adapter_log(level, fmt, ...) wlr_log(to_wlr(level), "[wsland.adapter] " fmt, ##__VA_ARGS__);
#define backend_log(level, fmt, ...) wlr_log(to_wlr(level), "[wsland.backend] " fmt, ##__VA_ARGS__);
#endif // WSLAND_COMMON_LOG_H
