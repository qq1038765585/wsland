// ReSharper disable All
#define _GNU_SOURCE 1

#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <wlr/util/log.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/interfaces/wlr_output.h>

#include "wsland.h"

void wsland_freerdp_dispatch_to_display_loop(
    struct wsland_peer_context *peer_ctx, wsland_loop_task_func_t func, struct wsland_loop_task *task
) {
    task->client_context = peer_ctx;
    task->func = func;

    pthread_mutex_lock(&peer_ctx->loop_task_list_mutex);
    wl_list_insert(&peer_ctx->loop_task_list, &task->link);
    pthread_mutex_unlock(&peer_ctx->loop_task_list_mutex);

    eventfd_write(peer_ctx->loop_task_event_source_fd, 1);
}

static bool wsland_freedrdp_event_loop_add_fd(
    struct wl_event_loop *loop, int fd, uint32_t mask, wl_event_loop_fd_func_t func,
    void *data, struct wl_event_source **event_source
) {
    *event_source = wl_event_loop_add_fd(loop, fd, 0, func, data);
    if (!*event_source) {
        wlr_log(WLR_INFO, "%s: wl_event_loop_add_fd failed.", __func__);
        return false;
    }

    wl_event_source_fd_update(*event_source, mask);
    return true;
}

static int wsland_freerdp_dispatch_task(int fd, uint32_t mask, void *arg) {
    struct wsland_peer_context *client_ctx = (struct wsland_peer_context*)arg;
    struct wsland_loop_task *task, *tmp;
    eventfd_t dummy;

    eventfd_read(client_ctx->loop_task_event_source_fd, &dummy);

    pthread_mutex_lock(&client_ctx->loop_task_list_mutex);
    /* dequeue the first task which is at last, so use reverse. */
    assert(!wl_list_empty(&client_ctx->loop_task_list));
    wl_list_for_each_reverse_safe(task, tmp, &client_ctx->loop_task_list, link) {
        wl_list_remove(&task->link);
        break;
    }
    pthread_mutex_unlock(&client_ctx->loop_task_list_mutex);

    /* Dispatch and task will be freed by caller. */
    task->func(false, task);
    return 0;
}

bool wsland_freerdp_dispatch_task_init(struct wsland_peer_context *peer_ctx) {
    if (pthread_mutex_init(&peer_ctx->loop_task_list_mutex, NULL) == -1) {
        wlr_log(WLR_ERROR, "%s: pthread_mutex_init failed. %s", __func__, strerror(errno));
        goto error_mutex;
    }

    assert(peer_ctx->loop_task_event_source_fd == -1);
    peer_ctx->loop_task_event_source_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (peer_ctx->loop_task_event_source_fd == -1) {
        wlr_log(WLR_ERROR, "%s: eventfd(EFD_SEMAPHORE) failed. %s", __func__, strerror(errno));
        goto error_event_source_fd;
    }

    assert(wl_list_empty(&peer_ctx->loop_task_list));

    assert(peer_ctx->loop_task_event_source == NULL);
    if (!wsland_freedrdp_event_loop_add_fd(
        server.event_loop, peer_ctx->loop_task_event_source_fd, WL_EVENT_READABLE,
        wsland_freerdp_dispatch_task, peer_ctx, &peer_ctx->loop_task_event_source
    )) {
        goto error_event_loop_add_fd;
    }

    return true;

    error_event_loop_add_fd:
        close(peer_ctx->loop_task_event_source_fd);
    peer_ctx->loop_task_event_source_fd = -1;

    error_event_source_fd:
        pthread_mutex_destroy(&peer_ctx->loop_task_list_mutex);

    error_mutex:
        return false;
}