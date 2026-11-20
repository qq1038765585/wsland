#ifndef WSLAND_BACKEND_H
#define WSLAND_BACKEND_H

#include <stdatomic.h>
#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/audin.h>
#include <freerdp/server/rdpsnd.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/gfxredir.h>
#include <wlr/types/wlr_data_device.h>


#define BACKEND_MAX_FDS 32
#define BACKEND_MAX_KEYS 256
#define BACKEND_MAX_MONITOR 16
#define WS_CLIPBOARD_MAX_FORMATS 5


#define DISPATCH(C, T, D, CB) {                                 \
    struct backend_task_data *data = malloc(sizeof(*data));     \
    data->client = C; data->T = *(D);                           \
    backend_task_dispatch(C, CB, &data->task);                  \
}

struct wsland_config;
struct wsland_adapter;
struct client_audio_tx;
struct client_audio_rx;
struct audio_cmd_header;

enum backend_client_flags {
    BACKEND_CLIENT_ACTIVATED = 1 << 0,
    BACKEND_OUTPUT_ENABLED = 1 << 1,
};

struct wsland_backend {
    struct wl_event_source *sources[BACKEND_MAX_FDS];
    struct backend_client *client;

    char *key_content;
    char *cert_content;

    char *rail_shell_name;
    void *rail_shell_context;

    bool use_gfxredir;
    char *shared_memory_mount_point;
    size_t shared_memory_mount_point_size;

    bool enable_window_snap_arrange;
    bool enable_window_shadow_remoting;

    freerdp_listener *listener;
    struct wsland_adapter *adapter;
};

struct backend_client {
    rdpContext ctx;
    bool pressed[BACKEND_MAX_KEYS];
    struct wl_event_source *sources[BACKEND_MAX_FDS + 1];
    freerdp_peer *peer;

    HANDLE vcm;
    enum backend_client_flags flags;

    RailServerContext *rail_ctx;
    DispServerContext *disp_ctx;
    RdpgfxServerContext *rdpgfx_ctx;
    DrdynvcServerContext *drdynvc_ctx;
    GfxRedirServerContext *gfxredir_ctx;
    CliprdrServerContext *clipboard_ctx;

    struct clipboard_data_source *clipboard_source;
    struct clipboard_data_source *clipboard_inflight_source;

    atomic_int current_frame_id;
    atomic_int acknowledged_frame_id;

    bool handshake_completed;
    bool activation_rail_completed;
    bool activation_graphics_completed;
    bool activation_graphics_redirection_completed;
    bool is_acknowledged_suspended;
    bool mouse_button_swap;

    struct wl_list dispatch_tasks;
    struct wl_event_source *dispatch_event_source;
    pthread_mutex_t dispatch_mutex;
    int dispatch_fd;

    struct client_audio_tx *audio_tx;
    struct client_audio_rx *audio_rx;

    struct {
        uint32_t op_id;
        uint32_t format_ids[WS_CLIPBOARD_MAX_FORMATS];
        size_t count;

        struct clipboard_request_source *request_flight;
        struct clipboard_response_source *response_flight;
    } clipboard;

    struct {
        struct wl_list output;

        struct wl_list motion;
        struct wl_list button;
        struct wl_list axis;

        struct wl_list input;

        struct wl_list clipboard_request;
        struct wl_list clipboard_response;
    } event;

    struct wsland_pointer *pointer;
    struct wsland_keyboard *keyboard;
    struct wsland_backend *backend;
};

struct audio_block_info {
    UINT64 submissionTime;
    UINT64 ackReceivedTime;
    UINT64 ackPlayedTime;
};

struct client_audio_tx {
    rdpsnd_server_context *tx_ctx;

    BOOL audioExitSignal;
    int pulseAudioSinkListenerFd;
    int pulseAudioSinkFd;
    pthread_t pulseAudioSinkThread;
    int bytesPerFrame;
    UINT audioBufferSize;
    BYTE* audioBuffer;
    BYTE lastBlockSent;
    UINT64 lastNetworkLatency;
    UINT64 accumulatedNetworkLatency;
    UINT accumulatedNetworkLatencyCount;
    UINT64 lastRenderedLatency;
    UINT64 accumulatedRenderedLatency;
    UINT accumulatedRenderedLatencyCount;
    struct audio_block_info blockInfo[256];
    int nextValidBlock;
    UINT PAVersion;
    int audioSem;
};

struct client_audio_rx {
    audin_server_context *rx_ctx;

    BOOL audioInExitSignal;
    int pulseAudioSourceListenerFd;
    int pulseAudioSourceFd;
    int closeAudioSourceFd;
    pthread_t pulseAudioSourceThread;
    BOOL isAudioInStreamOpened;
};

struct wsc_monitor_change_data {
    rdpMonitor *monitors;
    int count;
};

struct wsc_output_change_event {
    struct wl_list link;
};

struct wsc_keyboard_input_event {
    struct wlr_keyboard_key_event source;

    struct wl_list link;
};

struct wsc_mouse_motion_event {
    struct wlr_pointer_motion_absolute_event source;

    struct wl_list link;
};

struct wsc_mouse_button_event {
    struct wlr_pointer_button_event source;

    struct wl_list link;
};

struct wsc_mouse_axis_event {
    struct wlr_pointer_axis_event source;

    struct wl_list link;
};

typedef void (*backend_task_func_t)(bool free_only, void *data);

struct backend_task {
    backend_task_func_t func;
    struct wl_list link;

    struct backend_client *client;
};

struct backend_task_data {
    struct backend_task task;

    union {
        RAIL_ACTIVATE_ORDER activate;
        RAIL_SYSPARAM_ORDER sysparam;
        RAIL_SYSCOMMAND_ORDER syscommand;
        RAIL_GET_APPID_REQ_ORDER appid;

        struct wsc_monitor_change_data change_data;
    };

    struct backend_client *client;
};

void backend_task_dispatch(struct backend_client *client, backend_task_func_t func, struct backend_task *task);
void linux_provide_format(struct wsland_adapter *adapter, struct wlr_seat *seat);
void backend_client_clipboard_source_destroy(struct backend_client *client);
void backend_client_audio_tx_destroy(struct backend_client *client);
void backend_client_audio_rx_destroy(struct backend_client *client);

bool backend_client_drdynvc_init(struct backend_client *client);
bool backend_client_gfxredir_init(struct backend_client *client);
bool backend_client_clipboard_init(struct backend_client *client);
bool backend_client_audiotx_init(struct backend_client *client);
bool backend_client_audiorx_init(struct backend_client *client);
bool backend_client_rdpgfx_init(struct backend_client *client);
bool backend_client_rail_init(struct backend_client *client);
bool backend_client_disp_init(struct backend_client *client);

void wsland_backend_fetch_keyboard_layout(struct wsland_backend *backend, struct xkb_rule_names *rules);
BOOL wsland_backend_incoming_client(freerdp_listener *listener, freerdp_peer *peer);
void wsland_backend_generate_crypto(struct wsland_backend *backend);

struct wsland_backend *wsland_backend_create(struct wsland_config *config, struct wsland_adapter *adapter);
void wsland_backend_destroy(struct wsland_backend *backend);
#endif //WSLAND_BACKEND_H
