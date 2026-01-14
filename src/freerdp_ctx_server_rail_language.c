// ReSharper disable All
#include <assert.h>
#include <wlr/util/log.h>
#include <freerdp/locale/keyboard.h>
#include <winpr/rpc.h>

#include "wsland.h"


/* GUID_CHTIME_BOPOMOFO is not defined in FreeRDP */
#ifndef GUID_CHTIME_BOPOMOFO
#define GUID_CHTIME_BOPOMOFO { \
    0xB115690A, 0xEA02, 0x48D5, 0xA2, 0x31, 0xE3, 0x57, 0x8D, 0x2F, 0xDF, 0x80 \
}
#endif

/* Define GUID for Google IME */
#define GUID_GOOGLEIME_JPN { \
    0xd5a86fd5, 0x5308, 0x47ea, 0xad, 0x16, 0x9c, 0x4e, 0xb1, 0x60, 0xec, 0x3c \
}

#define GUID_PROFILE_GOOGLEIME_JPN { \
    0x773eb24e, 0xca1d, 0x4b1b, 0xb4, 0x20, 0xfa, 0x98, 0x5b, 0xb0, 0xb8, 0x0d \
}

struct lang_guid {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    BYTE Data4_0;
    BYTE Data4_1;
    BYTE Data4_2;
    BYTE Data4_3;
    BYTE Data4_4;
    BYTE Data4_5;
    BYTE Data4_6;
    BYTE Data4_7;
};

static void wsland_debug_raw_guid_string(const GUID *guid) {
    wlr_log(
        WLR_DEBUG, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid->Data1, guid->Data2, guid->Data3,
        guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
        guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]
    );
}

static char* language_guid_to_string(const GUID *guid) {
    assert(sizeof(struct lang_guid) == sizeof(GUID));
    static const struct lang_guid c_GUID_NULL = GUID_NULL;
    static const struct lang_guid c_GUID_MS_JPNIME = GUID_MSIME_JPN;
    static const struct lang_guid c_GUID_GOOGLE_JPNIME = GUID_GOOGLEIME_JPN;
    static const struct lang_guid c_GUID_KORIME = GUID_MSIME_KOR;
    static const struct lang_guid c_GUID_CHSIME = GUID_CHSIME;
    static const struct lang_guid c_GUID_CHTIME = GUID_CHTIME;
    static const struct lang_guid c_GUID_CHTIME_BOPOMOFO = GUID_CHTIME_BOPOMOFO;
    static const struct lang_guid c_GUID_PROFILE_NEWPHONETIC = GUID_PROFILE_NEWPHONETIC;
    static const struct lang_guid c_GUID_PROFILE_CHANGJIE = GUID_PROFILE_CHANGJIE;
    static const struct lang_guid c_GUID_PROFILE_QUICK = GUID_PROFILE_QUICK;
    static const struct lang_guid c_GUID_PROFILE_CANTONESE = GUID_PROFILE_CANTONESE;
    static const struct lang_guid c_GUID_PROFILE_PINYIN = GUID_PROFILE_PINYIN;
    static const struct lang_guid c_GUID_PROFILE_SIMPLEFAST = GUID_PROFILE_SIMPLEFAST;
    static const struct lang_guid c_GUID_PROFILE_MSIME_JPN = GUID_GUID_PROFILE_MSIME_JPN;
    static const struct lang_guid c_GUID_PROFILE_GOOGLEIME_JPN = GUID_PROFILE_GOOGLEIME_JPN;
    static const struct lang_guid c_GUID_PROFILE_MSIME_KOR = GUID_PROFILE_MSIME_KOR;

    RPC_STATUS rpc_status;
    if (UuidEqual(guid, (GUID*)&c_GUID_NULL, &rpc_status))
        return "GUID_NULL";
    else if (UuidEqual(guid, (GUID*)&c_GUID_MS_JPNIME, &rpc_status))
        return "GUID_MS_JPNIME";
    else if (UuidEqual(guid, (GUID*)&c_GUID_GOOGLE_JPNIME, &rpc_status))
        return "GUID_GOOGLE_JPNIME";
    else if (UuidEqual(guid, (GUID*)&c_GUID_KORIME, &rpc_status))
        return "GUID_KORIME";
    else if (UuidEqual(guid, (GUID*)&c_GUID_CHSIME, &rpc_status))
        return "GUID_CHSIME";
    else if (UuidEqual(guid, (GUID*)&c_GUID_CHTIME, &rpc_status))
        return "GUID_CHTIME";
    else if (UuidEqual(guid, (GUID*)&c_GUID_CHTIME_BOPOMOFO, &rpc_status))
        return "GUID_CHTIME_BOPOMOFO";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_NEWPHONETIC, &rpc_status))
        return "GUID_PROFILE_NEWPHONETIC";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_CHANGJIE, &rpc_status))
        return "GUID_PROFILE_CHANGJIE";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_QUICK, &rpc_status))
        return "GUID_PROFILE_QUICK";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_CANTONESE, &rpc_status))
        return "GUID_PROFILE_CANTONESE";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_PINYIN, &rpc_status))
        return "GUID_PROFILE_PINYIN";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_SIMPLEFAST, &rpc_status))
        return "GUID_PROFILE_SIMPLEFAST";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_MSIME_JPN, &rpc_status))
        return "GUID_PROFILE_MSIME_JPN";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_GOOGLEIME_JPN, &rpc_status))
        return "GUID_PROFILE_GOOGLEIME_JPN";
    else if (UuidEqual(guid, (GUID*)&c_GUID_PROFILE_MSIME_KOR, &rpc_status))
        return "GUID_PROFILE_MSIME_KOR";
    else
        return "Unknown GUID";
}

void wsland_freerdp_rail_client_language_ime_info_callback(bool free_only, void *arg) {
    struct wsland_rail_dispatch_data *data = wl_container_of(arg, data, task_base);
    const RAIL_LANGUAGEIME_INFO_ORDER *languageImeInfo = &data->language_ime_info;
    struct wsland_peer_context *peer_ctx = data->peer_ctx;
    struct rdp_settings *settings = peer_ctx->peer->context->settings;
    uint32_t new_keyboard_layout = 0;
    struct xkb_keymap *keymap = NULL;
    char *s;

    switch (languageImeInfo->ProfileType) {
    case TF_PROFILETYPE_INPUTPROCESSOR:
        s = "TF_PROFILETYPE_INPUTPROCESSOR";
        break;
    case TF_PROFILETYPE_KEYBOARDLAYOUT:
        s = "TF_PROFILETYPE_KEYBOARDLAYOUT";
        break;
    default:
        s = "Unknown profile type";
        break;
    }
    wlr_log(WLR_DEBUG, "Client: LanguageImeInfo: ProfileType: %d (%s)", languageImeInfo->ProfileType, s);
    wlr_log(WLR_DEBUG, "Client: LanguageImeInfo: LanguageID: 0x%x", languageImeInfo->LanguageID);

    wlr_log(WLR_DEBUG, "Client: LanguageImeInfo: LanguageProfileCLSID: %s : ", language_guid_to_string(&languageImeInfo->LanguageProfileCLSID));
    wsland_debug_raw_guid_string(&languageImeInfo->LanguageProfileCLSID);
    wlr_log(WLR_DEBUG, "");

    wlr_log(WLR_DEBUG, "Client: LanguageImeInfo: ProfileGUID: %s : ", language_guid_to_string(&languageImeInfo->ProfileGUID));
    wsland_debug_raw_guid_string(&languageImeInfo->ProfileGUID);
    wlr_log(WLR_DEBUG, "");

    wlr_log(WLR_DEBUG, "Client: LanguageImeInfo: KeyboardLayout: 0x%x", languageImeInfo->KeyboardLayout);

    if (!free_only) {
        if (languageImeInfo->ProfileType == TF_PROFILETYPE_KEYBOARDLAYOUT) {
            new_keyboard_layout = languageImeInfo->KeyboardLayout;
        }
        else if (languageImeInfo->ProfileType == TF_PROFILETYPE_INPUTPROCESSOR) {
            assert(sizeof(struct lang_guid) == sizeof(GUID));

            static const struct lang_guid c_GUID_MS_JPNIME = GUID_MSIME_JPN;
            static const struct lang_guid c_GUID_GOOGLE_JPNIME = GUID_GOOGLEIME_JPN;
            static const struct lang_guid c_GUID_KORIME = GUID_MSIME_KOR;
            static const struct lang_guid c_GUID_CHSIME = GUID_CHSIME;
            static const struct lang_guid c_GUID_CHTIME = GUID_CHTIME;
            static const struct lang_guid c_GUID_CHTIME_BOPOMOFO = GUID_CHTIME_BOPOMOFO;

            RPC_STATUS rpc_status;
            if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_MS_JPNIME, &rpc_status)) {
                new_keyboard_layout = KBD_JAPANESE;
            }
            else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_GOOGLE_JPNIME, &rpc_status)) {
                new_keyboard_layout = KBD_JAPANESE;
            }
            else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_KORIME, &rpc_status)) {
                new_keyboard_layout = KBD_KOREAN;
            }
            else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_CHSIME, &rpc_status)) {
                new_keyboard_layout = KBD_CHINESE_SIMPLIFIED_US;
            }
            else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_CHTIME, &rpc_status)) {
                new_keyboard_layout = KBD_CHINESE_TRADITIONAL_US;
            }
            else if (UuidEqual(&languageImeInfo->LanguageProfileCLSID, (GUID*)&c_GUID_CHTIME_BOPOMOFO, &rpc_status)) {
                new_keyboard_layout = KBD_CHINESE_TRADITIONAL_US;
            }
            else {
                new_keyboard_layout = KBD_US;
            }
        }

        if (new_keyboard_layout && (new_keyboard_layout != settings->KeyboardLayout)) {
            struct xkb_rule_names rule_names = fetch_xkb_rule_names(settings);
            if (rule_names.layout) {
                struct xkb_context *xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                keymap = xkb_keymap_new_from_names(xkb_context, &rule_names, 0);
                if (keymap) {
                    wlr_keyboard_set_keymap(&peer_ctx->keyboard->keyboard, keymap);
                    xkb_keymap_unref(keymap);
                    xkb_context_unref(xkb_context);
                    settings->KeyboardLayout = new_keyboard_layout;
                    wlr_log(WLR_DEBUG, "%s: new keyboard layout: 0x%x", __func__, new_keyboard_layout);
                }
            }
            if (!keymap) {
                wlr_log(
                    WLR_ERROR, "%s: Failed to switch to kbd_layout:0x%x kbd_type:0x%x kbd_subType:0x%x",
                    __func__, new_keyboard_layout,
                    settings->KeyboardType,
                    settings->KeyboardSubType
                );

                wlr_log(WLR_DEBUG, "%s: Resetting default keymap", __func__);

                struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                keymap = xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
                wlr_keyboard_set_keymap(&peer_ctx->keyboard->keyboard, keymap);
                xkb_keymap_unref(keymap);
                xkb_context_unref(context);
                settings->KeyboardLayout = new_keyboard_layout;
            }
        }
    }

    free(data);
}