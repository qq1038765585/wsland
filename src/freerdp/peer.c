// ReSharper disable CppDeprecatedEntity
#include <unistd.h>
#include <pthread.h>
#include <freerdp/channels/wtsvc.h>
#include <freerdp/channels/channels.h>
#include <sys/eventfd.h>

#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"

static int rdp_peer_context_new(freerdp_peer *rdp_peer, wsland_peer *peer) {
    peer->peer = rdp_peer;

    peer->dispatch_fd = -1;
    wl_list_init(&peer->dispatch_tasks);
    wl_list_init(&peer->outputs);
    return true;
}

static void rdp_peer_context_free(freerdp_peer *rdp_peer, wsland_peer *peer) {
    if (!peer) {
        return;
    }

    wsland_output *output, *o_temp;
    wl_list_for_each_safe(output, o_temp, &peer->outputs, peer_link) {
        wlr_output_destroy(&output->output);
    }
    wlr_keyboard_finish(&peer->keyboard->keyboard);
    free(peer->keyboard);


    for (int i = 0; i < MAX_FREERDP_FDS + 1; ++i) {
        if (peer->sources[i]) {
            wl_event_source_remove(peer->sources[i]);
        }
    }

    if (peer->dispatch_event_source) {
        wl_event_source_remove(peer->dispatch_event_source);
        peer->dispatch_event_source = NULL;
    }

    dispatch_task *task, *t_temp;
    wl_list_for_each_reverse_safe(task, t_temp, &peer->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        task->func(true, task);
    }

    if (peer->dispatch_fd != -1) {
        close(peer->dispatch_fd);
        peer->dispatch_fd = -1;
    }

    pthread_mutex_destroy(&peer->dispatch_mutex);
    rail_clipboard_destroy(peer);
    peer->freerdp->peer = NULL;
}

static int wsland_peer_activity(int fd, uint32_t mask, void *data) {
    wsland_peer *peer = data;

    if (!peer->peer->CheckFileDescriptor(peer->peer)) {
        wsland_log(FREERDP, ERROR, "Unable to check client file descriptor for %p", (void *) data);
        freerdp_peer_context_free(peer->peer);
        freerdp_peer_free(peer->peer);
    }

    if (peer->vcm) {
        if (!WTSVirtualChannelManagerCheckFileDescriptor(peer->vcm)) {
            wsland_log(FREERDP, ERROR, "failed to check freerdp wts vc file descriptor for %p", data);
            freerdp_peer_context_free(peer->peer);
            freerdp_peer_free(peer->peer);
            exit(0);
        }
    }

    return 0;
}

void dispatch_to_display(wsland_peer *peer, dispatch_task_func_t func, dispatch_task *task) {
    task->peer = peer;
    task->func = func;

    pthread_mutex_lock(&peer->dispatch_mutex);
    wl_list_insert(&peer->dispatch_tasks, &task->link);
    pthread_mutex_unlock(&peer->dispatch_mutex);

    eventfd_write(peer->dispatch_fd, 1);
}

static int dispatch_task_exec(int fd, uint32_t mask, void *user_data) {
    wsland_peer *peer = user_data;

    eventfd_t dummy;
    eventfd_read(peer->dispatch_fd, &dummy);

    dispatch_task *task, *temp;
    pthread_mutex_lock(&peer->dispatch_mutex);
    wl_list_for_each_reverse_safe(task, temp, &peer->dispatch_tasks, link) {
        wl_list_remove(&task->link);
        break;
    }
    pthread_mutex_unlock(&peer->dispatch_mutex);

    task->func(false, task);
    return 0;
}

static bool dispatch_task_init(wsland_peer* peer) {
    if (pthread_mutex_init(&peer->dispatch_mutex, NULL) == -1) {
        goto mutex_failed;
    }

    peer->dispatch_fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (peer->dispatch_fd == -1) {
        goto dispatch_fd_error;
    }

    struct wl_event_loop* loop = wsland_adapter_fetch_event_loop(peer->freerdp->adapter);
    peer->dispatch_event_source = wl_event_loop_add_fd(
        loop, peer->dispatch_fd, WL_EVENT_READABLE, dispatch_task_exec, peer
    );
    if (!peer->dispatch_event_source) {
        goto dispatch_event_source_error;
    }
    return true;
    dispatch_event_source_error:
        close(peer->dispatch_fd);
    peer->dispatch_fd = -1;
    dispatch_fd_error:
        pthread_mutex_destroy(&peer->dispatch_mutex);
    mutex_failed:
        return false;
}

static bool rdp_peer_init(wsland_freerdp *freerdp, freerdp_peer *rdp_peer) {
    bool start = false;

    if (freerdp->peer) {
        wsland_log(FREERDP, ERROR, "already a peer connection, system current only support single peer client!")
        return false;
    }

    rdp_peer->ContextSize = sizeof(wsland_peer);
    rdp_peer->ContextNew = (psPeerContextNew)rdp_peer_context_new;
    rdp_peer->ContextFree = (psPeerContextFree)rdp_peer_context_free;
    freerdp_peer_context_new(rdp_peer);

    wsland_peer *peer = (wsland_peer*)rdp_peer->context;
    peer->freerdp = freerdp;

    wsland_peer_handle_init(peer);
    if (!peer->handle) {
        wsland_log(FREERDP, ERROR, "failed to invoke wsland_peer_event_init");
        goto init_failed;
    }

    if (freerdp->key_content && freerdp->cert_content) {
        rdp_peer->settings->CertificateContent = strdup(freerdp->cert_content);
        rdp_peer->settings->PrivateKeyContent = strdup(freerdp->key_content);
        rdp_peer->settings->RdpKeyContent = strdup(freerdp->key_content);
    }
    else {
        rdp_peer->context->settings->TlsSecurity = FALSE;
    }
    rdp_peer->context->settings->NlaSecurity = FALSE;

    if (!rdp_peer->Initialize(rdp_peer)) {
        wsland_log(FREERDP, ERROR, "failed to invoke freerdp peer Initialize");
        goto init_failed;
    }
    start = true;

    rdp_peer->context->settings->ColorDepth = 32;
    rdp_peer->context->settings->NSCodec = FALSE;
    rdp_peer->context->settings->RemoteFxCodec = FALSE;
    rdp_peer->context->settings->OsMajorType = OSMAJORTYPE_UNIX;
    rdp_peer->context->settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
    rdp_peer->context->settings->FrameMarkerCommandEnabled = TRUE;
    rdp_peer->context->settings->SurfaceFrameMarkerEnabled = TRUE;
    rdp_peer->context->settings->RefreshRect = TRUE;

    UINT32 remote_application_level = RAIL_LEVEL_SUPPORTED |
        RAIL_LEVEL_SHELL_INTEGRATION_SUPPORTED |
        RAIL_LEVEL_LANGUAGE_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_SERVER_TO_CLIENT_IME_SYNC_SUPPORTED |
        RAIL_LEVEL_HANDSHAKE_EX_SUPPORTED;
    rdp_peer->context->settings->RemoteApplicationSupportLevel = remote_application_level;
    rdp_peer->context->settings->SupportGraphicsPipeline = TRUE;
    rdp_peer->context->settings->SupportMonitorLayoutPdu = TRUE;
    rdp_peer->context->settings->RedirectClipboard = TRUE;

    rdp_peer->AdjustMonitorsLayout = peer->handle->xf_peer_adjust_monitor_layout;
    rdp_peer->Capabilities = peer->handle->xf_peer_capabilities;
    rdp_peer->PostConnect = peer->handle->xf_peer_post_connect;
    rdp_peer->Activate = peer->handle->xf_peer_activate;

    rdp_peer->context->update->SuppressOutput = peer->handle->xf_suppress_output;

    rdp_peer->context->input->SynchronizeEvent = peer->handle->xf_input_synchronize_event;
    rdp_peer->context->input->MouseEvent = peer->handle->xf_input_mouse_event;
    rdp_peer->context->input->ExtendedMouseEvent = peer->handle->xf_input_extended_mouse_event;
    rdp_peer->context->input->KeyboardEvent = peer->handle->xf_input_keyboard_event;
    rdp_peer->context->input->UnicodeKeyboardEvent = peer->handle->xf_input_unicode_keyboard_event;

    HANDLE handles[MAX_FREERDP_FDS + 1];
    int handle_count = rdp_peer->GetEventHandles(rdp_peer, handles, MAX_FREERDP_FDS);
    if (!handle_count) {
        wsland_log(FREERDP, ERROR, "failed to invoke freerdp peer GetEventHandles");
        goto init_failed;
    }

    PWtsApiFunctionTable func_table = FreeRDP_InitWtsApi();
    WTSRegisterWtsApiFunctionTable(func_table);
    peer->vcm = WTSOpenServerA((LPSTR)peer);
    if (peer->vcm) {
        handles[handle_count++] = WTSVirtualChannelManagerGetEventHandle(peer->vcm);
    }
    else {
        wsland_log(FREERDP, ERROR, "failed to invoke freerdp peer WTSOpenServerA");
    }

    int i;
    for (i = 0; i < handle_count; ++i) {
        int fd = GetEventFileDescriptor(handles[i]);

        peer->sources[i] = wl_event_loop_add_fd(
            wsland_adapter_fetch_event_loop(freerdp->adapter), fd, WL_EVENT_READABLE, wsland_peer_activity, peer
        );
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        peer->sources[i] = 0;
    }

    if (!dispatch_task_init(peer)) {
        wsland_log(FREERDP, ERROR, "failed to invoke rdp_dispatch_task_init");
        goto init_failed;
    }

    if (!freerdp->peer) {
        freerdp->peer = peer;
    }

    return true;

init_failed:
    if (start) {
        rdp_peer->Close(rdp_peer);
    }
    return false;
}

BOOL wsland_freerdp_incoming_peer(freerdp_listener *listener, freerdp_peer *peer) {
    wsland_freerdp *freerdp = listener->param4;

    if (!rdp_peer_init(freerdp, peer)) {
        wsland_log(FREERDP, ERROR, "Error initializing incoming peer");
        return FALSE;
    }
    return TRUE;
}