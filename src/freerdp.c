// ReSharper disable All
#include <wlr/util/log.h>
#include <wlr/render/pixman.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/backend/headless.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_output.h>
#include <freerdp/locale/keyboard.h>
#include <freerdp/server/cliprdr.h>
#include <freerdp/server/drdynvc.h>
#include <freerdp/server/rdpgfx.h>
#include <freerdp/server/rail.h>
#include <freerdp/server/disp.h>
#include <freerdp/codec/nsc.h>
#include <freerdp/freerdp.h>
#include <linux/input.h>

#include "wsland.h"


struct rdp_to_xkb_keyboard_layout {
    UINT32 rdp_layout_code;
    const char *xkb_layout;
    const char *xkb_variant;
};

/* table reversed from
	https://github.com/awakecoding/FreeRDP/blob/master/libfreerdp/locale/xkb_layout_ids.c#L811 */
static struct rdp_to_xkb_keyboard_layout rdp_keyboards[] = {
    {KBD_ARABIC_101, "ara", 0},
    {KBD_BULGARIAN, 0, 0},
    {KBD_CHINESE_TRADITIONAL_US, 0, 0},
    {KBD_CZECH, "cz", 0},
    {KBD_CZECH_PROGRAMMERS, "cz", "bksl"},
    {KBD_CZECH_QWERTY, "cz", "qwerty"},
    {KBD_DANISH, "dk", 0},
    {KBD_GERMAN, "de", 0},
    {KBD_GERMAN_NEO, "de", "neo"},
    {KBD_GERMAN_IBM, "de", "qwerty"},
    {KBD_GREEK, "gr", 0},
    {KBD_GREEK_220, "gr", "simple"},
    {KBD_GREEK_319, "gr", "extended"},
    {KBD_GREEK_POLYTONIC, "gr", "polytonic"},
    {KBD_US, "us", 0},
    {KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, "ara", "buckwalter"},
    {KBD_SPANISH, "es", 0},
    {KBD_SPANISH_VARIATION, "es", "nodeadkeys"},
    {KBD_FINNISH, "fi", 0},
    {KBD_FRENCH, "fr", 0},
    {KBD_HEBREW, "il", 0},
    {KBD_HUNGARIAN, "hu", 0},
    {KBD_HUNGARIAN_101_KEY, "hu", "standard"},
    {KBD_ICELANDIC, "is", 0},
    {KBD_ITALIAN, "it", 0},
    {KBD_ITALIAN_142, "it", "nodeadkeys"},
    {KBD_JAPANESE, "jp", 0},
    {KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, "jp", "kana"},
    {KBD_KOREAN, "kr", 0},
    {KBD_KOREAN_INPUT_SYSTEM_IME_2000, "kr", "kr104"},
    {KBD_DUTCH, "nl", 0},
    {KBD_NORWEGIAN, "no", 0},
    {KBD_POLISH_PROGRAMMERS, "pl", 0},
    {KBD_POLISH_214, "pl", "qwertz"},
    {KBD_ROMANIAN, "ro", 0},
    {KBD_RUSSIAN, "ru", 0},
    {KBD_RUSSIAN_TYPEWRITER, "ru", "typewriter"},
    {KBD_CROATIAN, "hr", 0},
    {KBD_SLOVAK, "sk", 0},
    {KBD_SLOVAK_QWERTY, "sk", "qwerty"},
    {KBD_ALBANIAN, 0, 0},
    {KBD_SWEDISH, "se", 0},
    {KBD_THAI_KEDMANEE, "th", 0},
    {KBD_THAI_KEDMANEE_NON_SHIFTLOCK, "th", "tis"},
    {KBD_TURKISH_Q, "tr", 0},
    {KBD_TURKISH_F, "tr", "f"},
    {KBD_URDU, "in", "urd-phonetic3"},
    {KBD_UKRAINIAN, "ua", 0},
    {KBD_BELARUSIAN, "by", 0},
    {KBD_SLOVENIAN, "si", 0},
    {KBD_ESTONIAN, "ee", 0},
    {KBD_LATVIAN, "lv", 0},
    {KBD_LITHUANIAN_IBM, "lt", "ibm"},
    {KBD_FARSI, "af", 0},
    {KBD_VIETNAMESE, "vn", 0},
    {KBD_ARMENIAN_EASTERN, "am", 0},
    {KBD_AZERI_LATIN, 0, 0},
    {KBD_FYRO_MACEDONIAN, "mk", 0},
    {KBD_GEORGIAN, "ge", 0},
    {KBD_FAEROESE, 0, 0},
    {KBD_DEVANAGARI_INSCRIPT, 0, 0},
    {KBD_MALTESE_47_KEY, 0, 0},
    {KBD_NORWEGIAN_WITH_SAMI, "no", "smi"},
    {KBD_KAZAKH, "kz", 0},
    {KBD_KYRGYZ_CYRILLIC, "kg", "phonetic"},
    {KBD_TATAR, "ru", "tt"},
    {KBD_BENGALI, "bd", 0},
    {KBD_BENGALI_INSCRIPT, "bd", "probhat"},
    {KBD_PUNJABI, 0, 0},
    {KBD_GUJARATI, "in", "guj"},
    {KBD_TAMIL, "in", "tam"},
    {KBD_TELUGU, "in", "tel"},
    {KBD_KANNADA, "in", "kan"},
    {KBD_MALAYALAM, "in", "mal"},
    {KBD_HINDI_TRADITIONAL, "in", 0},
    {KBD_MARATHI, 0, 0},
    {KBD_MONGOLIAN_CYRILLIC, "mn", 0},
    {KBD_UNITED_KINGDOM_EXTENDED, "gb", "intl"},
    {KBD_SYRIAC, "syc", 0},
    {KBD_SYRIAC_PHONETIC, "syc", "syc_phonetic"},
    {KBD_NEPALI, "np", 0},
    {KBD_PASHTO, "af", "ps"},
    {KBD_DIVEHI_PHONETIC, 0, 0},
    {KBD_LUXEMBOURGISH, 0, 0},
    {KBD_MAORI, "mao", 0},
    {KBD_CHINESE_SIMPLIFIED_US, 0, 0},
    {KBD_SWISS_GERMAN, "ch", "de_nodeadkeys"},
    {KBD_UNITED_KINGDOM, "gb", 0},
    {KBD_LATIN_AMERICAN, "latam", 0},
    {KBD_BELGIAN_FRENCH, "be", 0},
    {KBD_BELGIAN_PERIOD, "be", "oss_sundeadkeys"},
    {KBD_PORTUGUESE, "pt", 0},
    {KBD_SERBIAN_LATIN, "rs", 0},
    {KBD_AZERI_CYRILLIC, "az", "cyrillic"},
    {KBD_SWEDISH_WITH_SAMI, "se", "smi"},
    {KBD_UZBEK_CYRILLIC, "af", "uz"},
    {KBD_INUKTITUT_LATIN, "ca", "ike"},
    {KBD_CANADIAN_FRENCH_LEGACY, "ca", "fr-legacy"},
    {KBD_SERBIAN_CYRILLIC, "rs", 0},
    {KBD_CANADIAN_FRENCH, "ca", "fr-legacy"},
    {KBD_SWISS_FRENCH, "ch", "fr"},
    {KBD_BOSNIAN, "ba", 0},
    {KBD_IRISH, 0, 0},
    {KBD_BOSNIAN_CYRILLIC, "ba", "us"},
    {KBD_UNITED_STATES_DVORAK, "us", "dvorak"},
    {KBD_PORTUGUESE_BRAZILIAN_ABNT2, "br", "nativo"},
    {KBD_CANADIAN_MULTILINGUAL_STANDARD, "ca", "multix"},
    {KBD_GAELIC, "ie", "CloGaelach"},

    {0x00000000, 0, 0},
};

/* taken from 2.2.7.1.6 Input Capability Set (TS_INPUT_CAPABILITYSET) */
static char *rdp_keyboard_types[] = {
    "", /* 0: unused */
    "", /* 1: IBM PC/XT or compatible (83-key) keyboard */
    "", /* 2: Olivetti "ICO" (102-key) keyboard */
    "", /* 3: IBM PC/AT (84-key) or similar keyboard */
    "pc102", /* 4: IBM enhanced (101- or 102-key) keyboard */
    "", /* 5: Nokia 1050 and similar keyboards */
    "", /* 6: Nokia 9140 and similar keyboards */
    "" /* 7: Japanese keyboard */
};

struct xkb_rule_names fetch_xkb_rule_names(struct rdp_settings *settings) {
    // We need to set up an XKB context and jump through some hoops to convert
    // RDP input events into scancodes
    struct xkb_rule_names xkb_rule_names = {0};
    if (settings->KeyboardType <= 7) {
        xkb_rule_names.model = rdp_keyboard_types[settings->KeyboardType];
    }
    for (int i = 0; rdp_keyboards[i].rdp_layout_code; ++i) {
        if (rdp_keyboards[i].rdp_layout_code == settings->KeyboardLayout) {
            xkb_rule_names.layout = rdp_keyboards[i].xkb_layout;
            xkb_rule_names.variant = rdp_keyboards[i].xkb_variant;
            wlr_log(
                WLR_DEBUG, "Mapped RDP keyboard to xkb layout %s variant "
                "%s", xkb_rule_names.layout, xkb_rule_names.variant
            );
            break;
        }
    }
    return xkb_rule_names;
}

static void wlr_add_rdp_output(void *data) {
    struct wsland_client_context *ctx = data;

    ctx->server->freerdp->data = (void*)ctx;
    struct wlr_output *wlr_output = wlr_headless_add_output(
        ctx->server->backend, ctx->client->settings->DesktopWidth, ctx->client->settings->DesktopHeight
    );

    if (!wlr_output) {
        wlr_log(WLR_ERROR, "failed to invoke headless add output");
    }
}

static void wlr_rdp_add_keyboard(void *data) {
    struct wsland_client_context *context = data;

    struct wlr_input_device device = {
        .type = WLR_INPUT_DEVICE_KEYBOARD,
        .name = "wsland-keyboard",
        .data = context
    };
    wl_signal_emit_mutable(&context->server->backend->events.new_input, &device);
}

static void wlr_rdp_add_pointer(void *data) {
    struct wsland_client_context *context = data;

    struct wlr_input_device device = {
        .type = WLR_INPUT_DEVICE_POINTER,
        .name = "wsland-pointer",
        .data = context
    };
    wl_signal_emit_mutable(&context->server->backend->events.new_input, &device);
}

static BOOL xf_peer_capabilities(struct rdp_freerdp_peer *client) {
    return TRUE;
}

static BOOL xf_peer_post_connect(struct rdp_freerdp_peer *client) {
    struct wsland_client_context *ctx = (struct wsland_client_context*)client->context;
    return TRUE;
}

static void xf_peer_disconnect(struct rdp_freerdp_peer *client) {
    struct wsland_client_context *ctx = (struct wsland_client_context*)client->context;
}

static BOOL xf_peer_activate(struct rdp_freerdp_peer *client) {
    struct wsland_client_context *ctx = (struct wsland_client_context*)client->context;
    struct rdp_settings *settings = client->settings;

    if (!settings->SurfaceCommandsEnabled) {
        wlr_log(WLR_ERROR, "RDP peer does not support SurfaceCommands");
        return FALSE;
    }

    rfx_context_reset(
        ctx->rfx_context, settings->DesktopWidth, settings->DesktopHeight
    );
    nsc_context_reset(
        ctx->nsc_context, settings->DesktopWidth, settings->DesktopHeight
    );

    if (ctx->flags & WSLAND_RDP_PEER_ACTIVATED) {
        return TRUE;
    }

    if (!wl_event_loop_add_idle(ctx->server->event_loop, wlr_add_rdp_output, ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add output");
        return FALSE;
    }

    if (!wl_event_loop_add_idle(ctx->server->event_loop, wlr_rdp_add_pointer, ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add pointer");
        return FALSE;
    }

    if (!wl_event_loop_add_idle(ctx->server->event_loop, wlr_rdp_add_keyboard, ctx)) {
        wlr_log(WLR_ERROR, "failed to add event for add keyboard");
        return FALSE;
    }

    {
        // Use wlroots' software cursors instead of remote cursors
        POINTER_SYSTEM_UPDATE pointer_system;
        rdpPointerUpdate *pointer = client->update->pointer;
        pointer_system.type = SYSPTR_NULL;
        pointer->PointerSystem(client->context, &pointer_system);
    }

    ctx->flags |= WSLAND_RDP_PEER_ACTIVATED;
    return TRUE;
}

static int xf_suppress_output(struct rdp_context *context, BYTE allow, const RECTANGLE_16 *area) {
    struct wsland_client_context *peer_context = (struct wsland_client_context*)context;

    if (allow) {
        peer_context->flags |= WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    else {
        peer_context->flags &= ~WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    return true;
}

static int xf_input_synchronize_event(struct rdp_input *input, UINT32 flags) {
    struct wsland_client_context *context = (struct wsland_client_context*)input->context;

    // wlr_output_update_needs_frame(context->output->base);
    // wlr_output_damage_whole(&context->output->base);
    return true;
}

static int64_t timespec_to_msec(const struct timespec *a) {
    return (int64_t)a->tv_sec * 1000 + a->tv_nsec / 1000000;
}

static int xf_input_mouse_event(struct rdp_input *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wsland_client_context *context = (struct wsland_client_context*)input->context;
    struct wlr_pointer *pointer = &context->pointer->pointer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(context->flags & WSLAND_RDP_PEER_POINTER_ENABLE)) {
        return true;
    }

    bool frame = false;
    if (flags & PTR_FLAGS_MOVE) {
        struct wlr_pointer_motion_absolute_event event = {0};
        event.pointer = pointer;
        event.time_msec = timespec_to_msec(&now);
        event.x = x / (double)context->output->base->width;
        event.y = y / (double)context->output->base->height;
        wl_signal_emit_mutable(&pointer->events.motion_absolute, &event);
        frame = true;
    }

    uint32_t button = 0;
    if (flags & PTR_FLAGS_BUTTON1) {
        button = BTN_LEFT;
    }
    else if (flags & PTR_FLAGS_BUTTON2) {
        button = BTN_RIGHT;
    }
    else if (flags & PTR_FLAGS_BUTTON3) {
        button = BTN_MIDDLE;
    }

    if (button) {
        struct wlr_pointer_button_event event = {0};
        event.pointer = pointer;
        event.time_msec = timespec_to_msec(&now);
        event.button = button;
        event.state = (flags & PTR_FLAGS_DOWN) ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;
        wl_signal_emit_mutable(&pointer->events.button, &event);
        frame = true;
    }

    if (flags & PTR_FLAGS_WHEEL) {
        double value = -(flags & 0xFF) / 120.0;
        if (flags & PTR_FLAGS_WHEEL_NEGATIVE) {
            value = -value;
        }

        struct wlr_pointer_axis_event event = {0};
        event.pointer = pointer;
        event.time_msec = timespec_to_msec(&now);
        event.source = WL_POINTER_AXIS_SOURCE_WHEEL;
        event.orientation = WL_POINTER_AXIS_VERTICAL_SCROLL;
        event.delta = value;
        event.delta_discrete = (int32_t)value;
        wl_signal_emit_mutable(&pointer->events.axis, &event);
        frame = true;
    }

    if (frame) {
        wl_signal_emit_mutable(&pointer->events.frame, pointer);
    }

    return true;
}

static int xf_input_extended_mouse_event(struct rdp_input *input, UINT16 flags, UINT16 x, UINT16 y) {
    struct wsland_client_context *context = (struct wsland_client_context*)input->context;
    struct wlr_pointer *pointer = &context->pointer->pointer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(context->flags & WSLAND_RDP_PEER_POINTER_ENABLE)) {
        return true;
    }

    struct wlr_pointer_motion_absolute_event event = {0};
    event.pointer = pointer;
    event.time_msec = timespec_to_msec(&now);
    event.x = x / (double)context->output->base->width;
    event.y = y / (double)context->output->base->height;
    wl_signal_emit_mutable(&pointer->events.motion_absolute, &event);
    wl_signal_emit_mutable(&pointer->events.frame, pointer);
    return true;
}

static int xf_input_keyboard_event(struct rdp_input *input, UINT16 flags, UINT16 code) {
    struct wsland_client_context *context = (struct wsland_client_context*)input->context;
    struct wlr_keyboard *keyboard = &context->keyboard->keyboard;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    if (!(context->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE)) {
        return true;
    }

    bool notify = false;
    enum wl_keyboard_key_state state = {0};
    if ((flags & KBD_FLAGS_DOWN)) {
        state = WL_KEYBOARD_KEY_STATE_PRESSED;
        notify = true;
    }
    else if ((flags & KBD_FLAGS_RELEASE)) {
        state = WL_KEYBOARD_KEY_STATE_RELEASED;
        notify = true;
    }

    if (notify) {
        uint32_t full_code = code;
        if (flags & KBD_FLAGS_EXTENDED) {
            full_code |= KBD_FLAGS_EXTENDED;
        }

        uint32_t vk_code = GetVirtualKeyCodeFromVirtualScanCode(full_code, 4);
        if (flags & KBD_FLAGS_EXTENDED) {
            vk_code |= KBDEXT;
        }

        uint32_t scan_code = GetKeycodeFromVirtualKeyCode(vk_code, KEYCODE_TYPE_EVDEV);
        struct wlr_keyboard_key_event event = {0};
        event.time_msec = timespec_to_msec(&now);
        event.keycode = scan_code - 8;
        event.state = state;
        event.update_state = true;
        wlr_keyboard_notify_key(keyboard, &event);
    }

    return true;
}

static int xf_input_unicode_keyboard_event(struct rdp_input *input, UINT16 flags, UINT16 code) {
    struct wsland_client_context *context = (struct wsland_client_context*)input->context;
    wlr_log(WLR_DEBUG, "Unhandled RDP unicode keyboard event " "(flags:0x%X code:0x%X)\n", flags, code);

    if (!(context->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE)) {
        return true;
    }

    return true;
}

static int rdp_client_activity(int fd, uint32_t mask, void *data) {
    struct rdp_freerdp_peer *client = (struct rdp_freerdp_peer*)data;

    if (!client->CheckFileDescriptor(client)) {
        wlr_log(WLR_ERROR, "Unable to check client file descriptor for %p", (void *) client);
        freerdp_peer_context_free(client);
        freerdp_peer_free(client);
    }
    return 0;
}

static int rdp_peer_context_new(struct rdp_freerdp_peer *client, struct wsland_client_context *context) {
    context->client = client;
    context->rfx_context = rfx_context_new(TRUE);
    if (!context->rfx_context) {
        return false;
    }

    context->rfx_context->mode = RLGR3;
    context->rfx_context->width = client->settings->DesktopWidth;
    context->rfx_context->height = client->settings->DesktopHeight;
    rfx_context_set_pixel_format(context->rfx_context, PIXEL_FORMAT_BGRA32);

    context->nsc_context = nsc_context_new();
    if (!context->nsc_context) {
        rfx_context_free(context->rfx_context);
        return false;
    }

    nsc_context_set_parameters(context->nsc_context, NSC_COLOR_FORMAT, PIXEL_FORMAT_BGRA32);
    context->encode_stream = Stream_New(NULL, 65536);
    if (!context->encode_stream) {
        nsc_context_free(context->nsc_context);
        rfx_context_free(context->rfx_context);
        return false;
    }

    return true;
}

static void rdp_peer_context_free(struct rdp_freerdp_peer *client, struct wsland_client_context *context) {
    if (!context) {
        return;
    }

    for (int i = 0; i < MAX_FREERDP_FDS; ++i) {
        if (context->events[i]) {
            wl_event_source_remove(context->events[i]);
        }
    }
    if (context->flags & WSLAND_RDP_PEER_OUTPUT_ENABLED) {
        wl_list_remove(&context->server->primary_output->frame.link);
        context->flags &= WSLAND_RDP_PEER_OUTPUT_ENABLED;
    }
    if (context->flags & WSLAND_RDP_PEER_POINTER_ENABLE) {
        wlr_pointer_finish(&context->pointer->pointer);
    }
    if (context->flags & WSLAND_RDP_PEER_KEYBOARD_ENABLE) {
        wlr_keyboard_finish(&context->keyboard->keyboard);
    }

    wl_list_remove(&context->link);

    Stream_Free(context->encode_stream, TRUE);
    nsc_context_free(context->nsc_context);
    rfx_context_free(context->rfx_context);
    free(context->rfx_rects);
}

static int rdp_peer_init(struct rdp_freerdp_peer *client, struct wsland_server *server) {
    client->ContextSize = sizeof(struct wsland_client_context);
    client->ContextNew = (psPeerContextNew)rdp_peer_context_new;
    client->ContextFree = (psPeerContextFree)rdp_peer_context_free;
    freerdp_peer_context_new(client);

    struct wsland_client_context *peer_context = (struct wsland_client_context*)client->context;
    peer_context->server = server;

    client->settings->CertificateFile = strdup(server->freerdp->tls_cert_path);
    client->settings->PrivateKeyFile = strdup(server->freerdp->tls_key_path);
    client->settings->NlaSecurity = FALSE;

    if (!client->Initialize(client)) {
        wlr_log(WLR_ERROR, "Failed to initialize FreeRDP peer");
        goto err_init;
    }

    client->settings->ColorDepth = 32;
    client->settings->RefreshRect = TRUE;
    client->settings->RemoteFxCodec = FALSE;
    client->settings->OsMajorType = OSMAJORTYPE_UNIX;
    client->settings->OsMinorType = OSMINORTYPE_PSEUDO_XSERVER;
    client->settings->FrameMarkerCommandEnabled = TRUE;
    client->settings->SurfaceFrameMarkerEnabled = TRUE;
    client->settings->NSCodec = TRUE;

    client->Capabilities = xf_peer_capabilities;
    client->PostConnect = xf_peer_post_connect;
    client->Disconnect = xf_peer_disconnect;
    client->Activate = xf_peer_activate;

    client->update->SuppressOutput = (pSuppressOutput)xf_suppress_output;

    client->input->SynchronizeEvent = xf_input_synchronize_event;
    client->input->MouseEvent = xf_input_mouse_event;
    client->input->ExtendedMouseEvent = xf_input_extended_mouse_event;
    client->input->KeyboardEvent = xf_input_keyboard_event;
    client->input->UnicodeKeyboardEvent = xf_input_unicode_keyboard_event;

    int rcount = 0;
    void *rfds[MAX_FREERDP_FDS];
    if (!client->GetFileDescriptor(client, rfds, &rcount)) {
        wlr_log(WLR_ERROR, "Unable to retrieve client file descriptors");
        goto err_init;
    }

    int i;
    for (i = 0; i < rcount; ++i) {
        int fd = (int)(long)(rfds[i]);
        peer_context->events[i] = wl_event_loop_add_fd(
            server->event_loop, fd, WL_EVENT_READABLE, rdp_client_activity, client
        );
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        peer_context->events[i] = NULL;
    }

    wl_list_insert(&server->freerdp->clients, &peer_context->link);
    return 0;

err_init:
    client->Close(client);
    return -1;
}

static int rdp_incoming_peer(struct rdp_freerdp_listener *listener, struct rdp_freerdp_peer *client) {
    struct wsland_server *backend = (struct wsland_server*)listener->param4;
    if (rdp_peer_init(client, backend) < 0) {
        wlr_log(WLR_ERROR, "Error initializing incoming peer");
        return false;
    }
    return true;
}

static int rdp_listener_activity(int fd, uint32_t mask, void *data) {
    freerdp_listener *listener = data;
    if (!(mask & WL_EVENT_READABLE)) {
        return 0;
    }

    if (!listener->CheckFileDescriptor(listener)) {
        wlr_log(WLR_ERROR, "Failed to check FreeRDP file descriptor");
        return -1;
    }
    return 0;
}

static bool rfx_swap_buffers(struct wsland_output *output) {
    pixman_region32_t *damage = &output->shadow_region;
    pixman_image_t *image = output->shadow_surface;

    if (!pixman_region32_not_empty(&output->shadow_region)) {
        return true;
    }

    struct wsland_client_context *ctx = output->context;
    struct rdp_freerdp_peer *peer = ctx->client;
    struct rdp_update *update = peer->update;

    Stream_Clear(ctx->encode_stream);
    Stream_SetPosition(ctx->encode_stream, 0);
    int width = damage->extents.x2 - damage->extents.x1;
    int height = damage->extents.y2 - damage->extents.y1;

    SURFACE_BITS_COMMAND cmd;
    cmd.skipCompression = TRUE;
    cmd.destLeft = damage->extents.x1;
    cmd.destTop = damage->extents.y1;
    cmd.destRight = damage->extents.x2;
    cmd.destBottom = damage->extents.y2;
    cmd.bmp.bpp = pixman_image_get_depth(image);
    cmd.bmp.codecID = peer->settings->RemoteFxCodecId;
    cmd.cmdType = CMDTYPE_STREAM_SURFACE_BITS;
    cmd.bmp.width = width;
    cmd.bmp.height = height;

    int stride = pixman_image_get_stride(image);
    uint32_t *data = pixman_image_get_data(image);

    uint32_t *ptr = data + damage->extents.x1 + damage->extents.y1 * (stride / sizeof(uint32_t));

    int nrects;
    RFX_RECT *rfx_rect;
    pixman_box32_t *rects = pixman_region32_rectangles(damage, &nrects);
    rfx_rect = realloc(ctx->rfx_rects, nrects * sizeof(*rfx_rect));
    if (rfx_rect == NULL) {
        wlr_log(WLR_ERROR, "RDP swap buffers failed: could not realloc rects");
        return false;
    }
    ctx->rfx_rects = rfx_rect;

    for (int i = 0; i < nrects; ++i) {
        pixman_box32_t *region = &rects[i];
        rfx_rect = &ctx->rfx_rects[i];
        rfx_rect->x = region->x1 - damage->extents.x1;
        rfx_rect->y = region->y1 - damage->extents.y1;
        rfx_rect->width = region->x2 - region->x1;
        rfx_rect->height = region->y2 - region->y1;
    }

    rfx_compose_message(
        ctx->rfx_context, ctx->encode_stream, ctx->rfx_rects, nrects, (BYTE*)ptr, width, height, stride * 4
    );

    cmd.bmp.bitmapDataLength = Stream_GetPosition(ctx->encode_stream);
    cmd.bmp.bitmapData = Stream_Buffer(ctx->encode_stream);

    update->SurfaceBits(update->context, &cmd);
    return true;
}

static bool nsc_swap_buffers(struct wsland_output *output) {
    pixman_region32_t *damage = &output->shadow_region;
    pixman_image_t *image = output->shadow_surface;

    if (!pixman_region32_not_empty(&output->shadow_region)) {
        return true;
    }

    struct wsland_client_context *ctx = output->context;
    struct rdp_freerdp_peer *peer = ctx->client;
    struct rdp_update *update = peer->update;

    Stream_Clear(ctx->encode_stream);
    Stream_SetPosition(ctx->encode_stream, 0);
    int width = damage->extents.x2 - damage->extents.x1;
    int height = damage->extents.y2 - damage->extents.y1;

    SURFACE_BITS_COMMAND cmd;
    cmd.skipCompression = TRUE;
    cmd.destTop = damage->extents.y1;
    cmd.destLeft = damage->extents.x1;
    cmd.destRight = damage->extents.x2;
    cmd.destBottom = damage->extents.y2;
    cmd.bmp.bpp = 32;
    cmd.bmp.codecID = peer->context->settings->NSCodecId;
    cmd.cmdType = CMDTYPE_SET_SURFACE_BITS;
    cmd.bmp.height = height;
    cmd.bmp.width = width;

    int stride = pixman_image_get_stride(image);
    uint32_t *data = pixman_image_get_data(image);

    uint32_t *ptr = data + damage->extents.x1 + damage->extents.y1 * (stride / sizeof(uint32_t));

    nsc_compose_message(
        ctx->nsc_context, ctx->encode_stream, (BYTE*)ptr, width, height, stride
    );

    cmd.bmp.bitmapDataLength = Stream_GetPosition(ctx->encode_stream);
    cmd.bmp.bitmapData = Stream_Buffer(ctx->encode_stream);

    return update->SurfaceBits(update->context, &cmd);
}

void commit_buffer(struct wsland_output *output) {
    // Send along to clients
    struct rdp_settings *settings = output->context->client->settings;
    if (settings->RemoteFxCodec) {
        rfx_swap_buffers(output);
    }
    else if (settings->NSCodec) {
        nsc_swap_buffers(output);
    }
    else {
        // This would perform like ass so why bother
        wlr_log(WLR_ERROR, "Raw updates are not supported; use rfx or nsc");
    }
}

void freerdp_init(struct wsland_server *server) {
    server->freerdp->listener = freerdp_listener_new();
    if (!server->freerdp->listener->listener) {
        wlr_log(WLR_ERROR, "Failed to allocate FreeRDP listener");
        return;
    }

    wl_list_init(&server->freerdp->clients);
    server->freerdp->listener->PeerAccepted = rdp_incoming_peer;
    server->freerdp->listener->param4 = server;

    if (!server->freerdp->listener->Open(server->freerdp->listener, server->freerdp->address, server->freerdp->port)) {
        wlr_log(WLR_ERROR, "Failed to bind to RDP socket");
        return;
    }

    int rcount = 0;
    void *rfds[MAX_FREERDP_FDS];
    if (!server->freerdp->listener->GetFileDescriptor(server->freerdp->listener, rfds, &rcount)) {
        wlr_log(WLR_ERROR, "Failed to get FreeRDP file descriptors");
        return;
    }

    int i;
    for (i = 0; i < rcount; ++i) {
        int fd = (int)(long)(rfds[i]);
        server->freerdp->listener_events[i] = wl_event_loop_add_fd(server->event_loop, fd, WL_EVENT_READABLE, rdp_listener_activity, server->freerdp->listener);
    }

    for (; i < MAX_FREERDP_FDS; ++i) {
        server->freerdp->listener_events[i] = NULL;
    }
}
