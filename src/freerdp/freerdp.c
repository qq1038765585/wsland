// ReSharper disable All
#define _GNU_SOURCE

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <linux/vm_sockets.h>

#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"
#include "wsland/utils/config.h"

static int create_vsock_fd(int port) {
    struct sockaddr_vm socket_address;

    int socket_fd = socket(AF_VSOCK, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (socket_fd < 0) {
        wsland_log(FREERDP, INFO, "fail to create vsocket");
        return -1;
    }

    const int bufferSize = 65536;

    if (setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wsland_log(FREERDP, INFO, "fail to setsockopt SO_SNDBUF");
        return -1;
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &bufferSize, sizeof(bufferSize)) < 0) {
        wsland_log(FREERDP, INFO, "fail to setsockopt SO_RCVBUF");
        return -1;
    }

    memset(&socket_address, 0, sizeof(socket_address));

    socket_address.svm_family = AF_VSOCK;
    socket_address.svm_cid = VMADDR_CID_ANY;
    socket_address.svm_port = port;

    socklen_t socket_addr_size = sizeof(socket_address);

    if (bind(socket_fd, (const struct sockaddr*)&socket_address, socket_addr_size) < 0) {
        wsland_log(FREERDP, INFO, "fail to bind socket to address socket");
        close(socket_fd);
        return -2;
    }

    int status = listen(socket_fd, 1);

    if (status != 0) {
        wsland_log(FREERDP, INFO, "fail to listen on socket");
        close(socket_fd);
        return -4;
    }
    return socket_fd;
}

static int use_vsock_fd(int port) {
    char *fd_str = getenv("USE_VSOCK");
    if (!fd_str) {
        return -1;
    }

    int fd;
    if (strlen(fd_str) != 0) {
        fd = atoi(fd_str);
        wsland_log(FREERDP, INFO, "using external fd for incoming connections: %d", fd);
        if (fd == 0) {
            fd = -1;
        }
    }
    else {
        fd = create_vsock_fd(port);
        wsland_log(FREERDP, INFO, "created vsock for external connections: %d", fd);
    }
    return fd;
}

static int rdp_listener_activity(int fd, uint32_t mask, void *data) {
    freerdp_listener *listener = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    if (!listener->CheckFileDescriptor(listener)) {
        wsland_log(FREERDP, ERROR, "failed to check freerdp file descriptor");
        return -1;
    }
    return 0;
}

bool wsland_allocate_shared_memory(wsland_freerdp *freerdp, wsland_shared_memory *shared_memory) {
    int fd = -1;
    void *addr = NULL;
    char path[freerdp->shared_memory_mount_point_size + 1 + WSLAND_SHARED_MEMORY_NAME_SIZE + 1];

    if (shared_memory->size <= 0) {
        goto release;
    }

    if (shared_memory->name[0] == '\0') {
        int fd_uuid = open("/proc/sys/kernal/random/uuid", O_RDONLY);
        if (fd_uuid < 0) {
            goto release;
        }
        if (read(fd_uuid, &shared_memory->name[1], 32 + 4) < 0) {
            goto release;
        }
        close(fd_uuid);
        shared_memory->name[0] = '{';
        shared_memory->name[WSLAND_SHARED_MEMORY_NAME_SIZE - 1] = '}';
        shared_memory->name[WSLAND_SHARED_MEMORY_NAME_SIZE] = '\0';
    } else if (strlen(shared_memory->name) != WSLAND_SHARED_MEMORY_NAME_SIZE ||
        shared_memory->name[0] != '{' ||
        shared_memory->name[WSLAND_SHARED_MEMORY_NAME_SIZE - 1] != '}' ||
        shared_memory->name[WSLAND_SHARED_MEMORY_NAME_SIZE] != '\0') {
        goto release;
    }

    strcpy(path, freerdp->shared_memory_mount_point);
    strcpy(path, "/");
    strcpy(path, shared_memory->name);

    fd = open(path, O_CREAT | O_RDWR | O_EXCL, S_IWUSR | S_IRUSR);
    if (fd < 0) {
        goto release;
    }

    if (fallocate(fd, 0, 0, shared_memory->size) < 0) {
        goto release;
    }

    addr = mmap(NULL, shared_memory->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        goto release;
    }

    shared_memory->fd = fd;
    shared_memory->addr = addr;
    return true;

release:
    if (fd > 0) {
        close(fd);
    }
    if (addr) {
        munmap(addr, shared_memory->size);
    }

    shared_memory->fd = -1;
    shared_memory->addr = NULL;
    return false;
}

void wsland_free_shared_memory(wsland_freerdp *freerdp, wsland_shared_memory *shared_memory) {
    if (shared_memory->addr) {
        munmap(shared_memory->addr, shared_memory->size);
        shared_memory->addr = NULL;
    }
    if (shared_memory->fd > 0) {
        close(shared_memory->fd);
        shared_memory->fd = -1;
    }
}

static void config_gfxredir(wsland_freerdp *freerdp) {
    bool use_gfxredir = true;

    if (use_gfxredir) {
        use_gfxredir = false;

        char *mount_point = getenv("WSL2_SHARED_MEMORY_MOUNT_POINT");
        if (mount_point) {
            freerdp->shared_memory_mount_point = mount_point;
            freerdp->shared_memory_mount_point_size = strlen(freerdp->shared_memory_mount_point);
            use_gfxredir = true;
        }
    }
    if (use_gfxredir) {
        use_gfxredir = false;

        struct wsland_shared_memory shared_memory = {0};
        shared_memory.size = sysconf(_SC_PAGESIZE);
        if (wsland_allocate_shared_memory(freerdp, &shared_memory)) {
            *(uint32_t *)shared_memory.addr = 0x12344321;
            wsland_free_shared_memory(freerdp, &shared_memory);
            use_gfxredir = true;
        }
    }

    freerdp->use_gfxredir = use_gfxredir;
}

wsland_freerdp *wsland_freerdp_create(wsland_config *config, wsland_adapter *adapter) {
    wsland_freerdp *freerdp = calloc(1, sizeof(*freerdp));
    if (!freerdp) {
        wsland_log(FREERDP, ERROR, "calloc failed for wsland_freerdp");
        goto create_failed;
    }

    freerdp->listener = freerdp_listener_new();
    if (!freerdp->listener) {
        wsland_log(FREERDP, ERROR, "failed to invoke freerdp_listener_new");
        goto create_failed;
    }

    config_gfxredir(freerdp);
    freerdp->listener->param4 = freerdp;
    freerdp->listener->PeerAccepted = wsland_freerdp_incoming_peer;
    freerdp->adapter = adapter;
    adapter->freerdp = freerdp;

    int vosck_fd = use_vsock_fd(config->port);
    if (vosck_fd < 0) {
        wsland_freerdp_generate_tls(freerdp);
    }

    if (vosck_fd > 0) {
        if (!freerdp->listener->OpenFromSocket(freerdp->listener, vosck_fd)) {
            wsland_log(FREERDP, ERROR, "failed to invoke freerdp OpenFromSocket [ fd: %d ]", vosck_fd);
            goto create_failed;
        }
    }
    else {
        if (!freerdp->listener->Open(freerdp->listener, config->address, config->port)) {
            wsland_log(FREERDP, ERROR, "failed to invoke freerdp Open");
            goto create_failed;
        }
    }

    HANDLE handles[MAX_FREERDP_FDS] = {0};
    int handle_count = freerdp->listener->GetEventHandles(freerdp->listener, handles, MAX_FREERDP_FDS);
    if (!handle_count) {
        wsland_log(FREERDP, ERROR, "failed to invoke freerdp GetFileDescriptor");
        goto create_failed;
    }

    int i;
    for (i = 0; i < handle_count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);

        freerdp->sources[i] = wl_event_loop_add_fd(
            wsland_adapter_fetch_event_loop(adapter), fd, WL_EVENT_READABLE, rdp_listener_activity, freerdp->listener
        );
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        freerdp->sources[i] = 0;
    }

    return freerdp;

create_failed:
    wsland_freerdp_destroy(freerdp);
    return NULL;
}

void wsland_freerdp_destroy(wsland_freerdp *freerdp) {
    if (freerdp) {
        for (int idx = 0; idx < MAX_FREERDP_FDS; idx++) {
            struct wl_event_source *source = freerdp->sources[idx];
            if (source) {
                wl_event_source_remove(source);
                freerdp->sources[idx] = 0;
            }
        }
        if (freerdp->peer) {
            freerdp_peer *peer = freerdp->peer->peer;
            freerdp_peer_context_free(peer);
            freerdp_peer_free(peer);
        }
        if (freerdp->listener) {
            freerdp_listener_free(freerdp->listener);
        }
        free(freerdp);
    }
}