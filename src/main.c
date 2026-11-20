#include <stdlib.h>
#include <signal.h>
#include <pthread.h>

#include "common/config.h"
#include "backend.h"
#include "adapter.h"
#include "server.h"


// ReSharper disable once CppUseInternalLinkage
struct wsland_signal_system {
    struct wsland_config *config;
    struct wsland_server *server;
    struct wsland_adapter *adapter;
    struct wsland_backend *backend;
};

static void restore_signals(void) {
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, NULL);

    const struct sigaction sa_dfl = { .sa_handler = SIG_DFL };
    sigaction(SIGCHLD, &sa_dfl, NULL);
    sigaction(SIGPIPE, &sa_dfl, NULL);
}

static int terminate_signal(int _, void *data) {
    const struct wsland_signal_system *system = (struct wsland_signal_system*)data;

    wsland_backend_destroy(system->backend);
    wsland_adapter_destroy(system->adapter);
    wsland_server_destroy(system->server);
    wsland_config_destroy(system->config);
    return 0;
}

static void wsland_signal_system_init(struct wsland_signal_system *signal) {
    wl_event_loop_add_signal(signal->server->dispatch, SIGTERM, terminate_signal, signal);
    wl_event_loop_add_signal(signal->server->dispatch, SIGINT, terminate_signal, signal);

    struct sigaction sa_ign = { .sa_handler = SIG_IGN };
    sigaction(SIGPIPE, &sa_ign, NULL);
    // sigaction(SIGCHLD, &sa_ign, NULL);

    pthread_atfork(NULL, NULL, restore_signals);
}

int main(const int argc, char *argv[]) {
    struct wsland_config *config = wsland_config_create(argc, argv);
    if (!config) {
        return EXIT_FAILURE;
    }
    struct wsland_adapter *adapter = wsland_adapter_create();
    if (!adapter) {
        return EXIT_FAILURE;
    }
    struct wsland_server *server = wsland_server_create(config, adapter);
    if (!server) {
        return EXIT_FAILURE;
    }
    struct wsland_backend *backend = wsland_backend_create(config, adapter);
    if (!backend) {
        return EXIT_FAILURE;
    }

    wsland_config_custom(config, server);
    struct wsland_signal_system signal = { config, server, adapter, backend };
    wsland_signal_system_init(&signal);

    wsland_server_running(server);
    return EXIT_SUCCESS;
}
