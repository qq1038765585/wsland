#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include <wlr/util/log.h>

#include "wsland/server.h"
#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/config.h"

typedef struct wsland_signal_system {
    wsland_config *config;
    wsland_server *server;
    wsland_adapter *adapter;
    wsland_freerdp *freerdp;
} wsland_signal_system;

static void restore_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);

    struct sigaction sa_dfl = { .sa_handler = SIG_DFL };
    sigaction(SIGCHLD, &sa_dfl, NULL);
    sigaction(SIGPIPE, &sa_dfl, NULL);
}

static int terminate_signal(int signal, void *data) {
    wsland_signal_system *system = data;

    wsland_freerdp_destroy(system->freerdp);
    wsland_adapter_destroy(system->adapter);
    wsland_server_destroy(system->server);
    wsland_config_destroy(system->config);
    return 0;
}

static void wsland_signal_system_init(wsland_signal_system *signal) {
    wl_event_loop_add_signal(signal->server->event_loop, SIGTERM, terminate_signal, signal);
    wl_event_loop_add_signal(signal->server->event_loop, SIGINT, terminate_signal, signal);

    struct sigaction sa_ign = { .sa_handler = SIG_IGN };
    // sigaction(SIGCHLD, &sa_ign, NULL);
    sigaction(SIGPIPE, &sa_ign, NULL);

    pthread_atfork(NULL, NULL, restore_signals);
}

static wsland_config *config;
static void log_to_file(enum wlr_log_importance importance, const char *fmt, va_list args) {
    static FILE *log_file = NULL;
    if (!log_file) {
        log_file = fopen(config->log ? config->log : "/tmp/wsland.log", "a");
        if (!log_file) {
            return;
        }
    }

    const enum wlr_log_importance current = wlr_log_get_verbosity();
    if (importance > current) {
        return;
    }

    const char *prefix = "";
    switch (importance) {
        case WLR_ERROR:   prefix = "[ERROR] "; break;
        case WLR_INFO:    prefix = "[INFO ]  "; break;
        case WLR_DEBUG:   prefix = "[DEBUG] "; break;
        default: break;
    }

    fprintf(log_file, "%s", prefix);
    vfprintf(log_file, fmt, args);
    fputc('\n', log_file);
    fflush(log_file);
}

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_INFO, NULL);
    config = wsland_config_create(argc, argv);
    if (!config) {
        return EXIT_FAILURE;
    }
    wlr_log_init(WLR_INFO, NULL);

    wsland_server *server = wsland_server_create(config);
    if (!server) {
        return EXIT_FAILURE;
    }

    wsland_adapter *adapter = wsland_adapter_create(server);
    if (!adapter) {
        return EXIT_FAILURE;
    }

    wsland_freerdp *freerdp = wsland_freerdp_create(config, adapter);
    if (!freerdp) {
        return EXIT_FAILURE;
    }

    wsland_signal_system signal = { config, server, adapter, freerdp };
    wsland_signal_system_init(&signal);

    wsland_server_running(server);
    return EXIT_SUCCESS;
}
