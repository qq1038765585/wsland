#include <unistd.h>
#include <linux/vm_sockets.h>

#include "common/logger.h"
#include "common/config.h"
#include "backend.h"
#include "adapter.h"



static int create_vsock_fd(const int port) {
    struct sockaddr_vm socket_address;

    const int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0) {
        backend_log(INFO, "fail to create vsocket");
        return -1;
    }

    const int bufferSize = 65536;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        backend_log(INFO, "fail to setsockopt SO_SNDBUF");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        backend_log(INFO, "fail to setsockopt SO_RCVBUF");
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));

    socket_address.svm_family = AF_VSOCK;
    socket_address.svm_cid = VMADDR_CID_ANY;
    socket_address.svm_port = port;

    const socklen_t socket_addr_size = sizeof(socket_address);

    if (bind(socket_fd, (const struct sockaddr *) &socket_address, socket_addr_size) < 0) {
        backend_log(INFO, "fail to bind socket to address socket");
        close(socket_fd);
        return -2;
    }

    const int status = listen(socket_fd, 1);

    if (status != 0) {
        backend_log(INFO, "fail to listen on socket");
        close(socket_fd);
        return -4;
    }
    return socket_fd;
}

static int use_vsock_fd(const int port) {
    char *fd_str = getenv("USE_VSOCK");
    if (!fd_str) {
        return -1;
    }

    int fd;
    if (strlen(fd_str) != 0) {
        fd = atoi(fd_str); // NOLINT(*-err34-c)
        backend_log(INFO, "using external fd for incoming connections: %d", fd);
        if (fd == 0) {
            fd = -1;
        }
    } else {
        fd = create_vsock_fd(port);
        backend_log(INFO, "created vsock for external connections: %d", fd);
    }
    return fd;
}

static int freerdp_listener_activity(int _, uint32_t mask, void *data) {
    freerdp_listener *listener = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    if (!listener->CheckFileDescriptor(listener)) {
        backend_log(ERROR, "failed to check freerdp file descriptor");
        return -1;
    }
    return 0;
}

struct wsland_backend *wsland_backend_create(struct wsland_config *config, struct wsland_adapter *adapter) {
    struct wsland_backend *backend = calloc(1, sizeof(*backend));
    if (!backend) {
        backend_log(ERROR, "calloc failed for wsland_backend");
        return NULL;
    }

    backend->listener = freerdp_listener_new();
    if (!backend->listener) {
        backend_log(ERROR, "failed to invoke freerdp_listener_new");
        goto create_failed;
    }

    backend->listener->param4 = backend;
    backend->listener->PeerAccepted = wsland_backend_incoming_client;

    int vsock_fd = use_vsock_fd(config->port);
    wsland_backend_generate_crypto(backend);

    if (vsock_fd > 0) {
        if (!backend->listener->OpenFromSocket(backend->listener, vsock_fd)) {
            backend_log(ERROR, "failed to invoke freerdp OpenFromSocket [ fd: %d ]", vsock_fd);
            goto create_failed;
        }
    } else {
        if (!backend->listener->Open(backend->listener, config->address, config->port)) {
            backend_log(ERROR, "failed to invoke freerdp Open");
            goto create_failed;
        }
    }

    HANDLE handles[BACKEND_MAX_FDS] = { 0 };
    const int count = (int) backend->listener->GetEventHandles(backend->listener, handles, BACKEND_MAX_FDS);
    if (!count) {
        backend_log(ERROR, "failed to invoke freerdp GetFileDescriptor");
        goto create_failed;
    }

    int i;
    for (i = 0; i < count; ++i) {
        const int fd = GetEventFileDescriptor(handles[i]);
        backend->sources[i] = adapter->handle->server_dispatch_event_source(
            adapter, fd, WL_EVENT_READABLE, freerdp_listener_activity, backend->listener
        );
    }

    for (; i < BACKEND_MAX_FDS; ++i) {
        backend->sources[i] = 0;
    }

    backend->adapter = adapter;
    adapter->backend = backend;
    adapter->handle->backend_shared_memory_config(adapter);
    return backend;

create_failed:
    wsland_backend_destroy(backend);
    return NULL;
}

void wsland_backend_destroy(struct wsland_backend *backend) {
    if (backend) {
        free(backend);
    }
}
