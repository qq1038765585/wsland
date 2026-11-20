#include "common/logger.h"


enum wlr_log_importance to_wlr(const enum wsland_log_level level) {
    switch (level) {
        case DEBUG:
            return WLR_DEBUG;
        case INFO:
            return WLR_INFO;
        case ERROR:
            return WLR_ERROR;
    }
    return WLR_SILENT;
}