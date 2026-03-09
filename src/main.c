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

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_DEBUG, NULL);

    wsland_config *config = wsland_config_create(argc, argv);
    if (!config) {
        return EXIT_FAILURE;
    }

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
