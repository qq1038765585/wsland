// ReSharper disable All
#ifndef WSLAND_FREERDP_H
#define WSLAND_FREERDP_H

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/server/disp.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/gfxredir.h>
#include <rdpapplist/rdpapplist_server.h>
#include <wayland-server-protocol.h>

#include "wsland/utils/config.h"

#define RDP_MAX_MONITOR 16
#define MAX_FREERDP_FDS 32
#define MAX_FREERDP_KEYS 256

struct wsland_peer;
struct wsland_adapter;

enum wsland_peer_flags {
    WSLAND_PEER_ACTIVATED = 1 << 0,
    WSLAND_PEER_OUTPUT_ENABLED = 1 << 1,
};

typedef struct wsland_peer_handle {
    BOOL (*xf_peer_adjust_monitor_layout)(freerdp_peer *peer);
    BOOL (*xf_peer_capabilities)(freerdp_peer *peer);
    BOOL (*xf_peer_post_connect)(freerdp_peer *peer);
    BOOL (*xf_peer_activate)(freerdp_peer *peer);

    BOOL (*xf_suppress_output)(rdpContext *context, BYTE allow, const RECTANGLE_16 *area);
    BOOL (*xf_input_synchronize_event)(rdpInput *input, UINT32 flags);
    BOOL (*xf_input_mouse_event)(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
    BOOL (*xf_input_extended_mouse_event)(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y);
    BOOL (*xf_input_keyboard_event)(rdpInput *input, UINT16 flags, UINT16 code);
    BOOL (*xf_input_unicode_keyboard_event)(rdpInput *input, UINT16 flags, UINT16 code);

    void (*rail_client_activate)(struct wsland_peer *peer, UINT32 window_id, BOOL enabled);
} wsland_peer_handle;

typedef struct wsland_freerdp {
    struct wl_event_source *sources[MAX_FREERDP_FDS];
    struct wsland_peer *peer;

    char *key_content;
    char *cert_content;

    char *rail_shell_name;
    void *rail_shell_context;

    bool use_gfxredir;
    bool enable_window_snap_arrange;
    bool enable_window_shadow_remoting;

    freerdp_listener *listener;
    struct wsland_adapter *adapter;
} wsland_freerdp;

typedef struct wsland_peer {
    rdpContext ctx;
    bool pressed[MAX_FREERDP_KEYS];
    struct wl_event_source *sources[MAX_FREERDP_FDS + 1];
    freerdp_peer *peer;

    HANDLE vcm;
    enum wsland_peer_flags flags;
    struct wl_list outputs;

    RailServerContext *ctx_server_rail;
    DrdynvcServerContext *ctx_server_drdynvc;
    DispServerContext *ctx_server_disp;
    RdpgfxServerContext *ctx_server_rdpgfx;
    GfxRedirServerContext *ctx_server_gfxredir;
    RdpAppListServerContext *ctx_server_applist;

    uint32_t current_frame_id;
    uint32_t acknowledged_frame_id;

    bool handshake_completed;
    bool activation_rail_completed;
    bool activation_graphics_completed;
    bool activation_graphics_redirection_completed;
    bool is_acknowledged_suspended;
    bool mouse_button_swap;

    struct wsland_freerdp *freerdp;
    struct wsland_keyboard *keyboard;
    struct wsland_peer_handle *handle;
} wsland_peer;

bool ctx_applist_init(wsland_peer *peer);
bool ctx_gfxredir_init(wsland_peer *peer);
bool ctx_rdpgfx_init(wsland_peer *peer);
bool ctx_disp_init(wsland_peer *peer);
bool ctx_rail_init(wsland_peer *peer);
bool ctx_drdynvc_init(wsland_peer *peer);

void wsland_peer_handle_init(wsland_peer *peer);

BOOL wsland_freerdp_incoming_peer(freerdp_listener *listener, freerdp_peer *peer);
void wsland_freerdp_generate_tls(wsland_freerdp *freerdp);

wsland_freerdp *wsland_freerdp_create(wsland_config *config, struct wsland_adapter *adapter);
void wsland_freerdp_destroy(wsland_freerdp *freerdp);
#endif
