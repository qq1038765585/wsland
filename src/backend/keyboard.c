#include <freerdp/locale/keyboard.h>

#include "backend.h"
#include "adapter.h"


#ifndef KBD_HEBREW_STANDARD
#define KBD_HEBREW_STANDARD 0x2040d
#endif
#ifndef KBD_PERSIAN
#define KBD_PERSIAN 0x50429
#endif
#ifndef KBD_FRENCH_STANDARD_BEPO
#define KBD_FRENCH_STANDARD_BEPO 0x2040c
#endif
#ifndef KBD_FRENCH_STANDARD_AZERTY
#define KBD_FRENCH_STANDARD_AZERTY 0x1040c
#endif

// ReSharper disable once CppUseInternalLinkage
struct backend_keyboard_layout {
	UINT32 rdp_layout_code;
	const char *xkb_layout;
	const char *xkb_variant;
};

static const struct backend_keyboard_layout backend_keyboards[] = {
	{.rdp_layout_code = KBD_ARABIC_101, .xkb_layout = "ara", .xkb_variant = 0},
	{.rdp_layout_code = KBD_BULGARIAN, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_CHINESE_TRADITIONAL_US, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_CZECH, .xkb_layout = "cz", .xkb_variant = 0},
	{.rdp_layout_code = KBD_CZECH_PROGRAMMERS, .xkb_layout = "cz", .xkb_variant = "bksl"},
	{.rdp_layout_code = KBD_CZECH_QWERTY, .xkb_layout = "cz", .xkb_variant = "qwerty"},
	{.rdp_layout_code = KBD_DANISH, .xkb_layout = "dk", .xkb_variant = 0},
	{.rdp_layout_code = KBD_GERMAN, .xkb_layout = "de", .xkb_variant = 0},
	{.rdp_layout_code = KBD_GERMAN_NEO, .xkb_layout = "de", .xkb_variant = "neo"},
	{.rdp_layout_code = KBD_GERMAN_IBM, .xkb_layout = "de", .xkb_variant = "qwerty"},
	{.rdp_layout_code = KBD_GREEK, .xkb_layout = "gr", .xkb_variant = 0},
	{.rdp_layout_code = KBD_GREEK_220, .xkb_layout = "gr", .xkb_variant = "simple"},
	{.rdp_layout_code = KBD_GREEK_319, .xkb_layout = "gr", .xkb_variant = "extended"},
	{.rdp_layout_code = KBD_GREEK_POLYTONIC, .xkb_layout = "gr", .xkb_variant = "polytonic"},
	{.rdp_layout_code = KBD_US, .xkb_layout = "us", .xkb_variant = 0},
	{.rdp_layout_code = KBD_UNITED_STATES_INTERNATIONAL, .xkb_layout = "us", .xkb_variant = "intl"},
	{.rdp_layout_code = KBD_US_ENGLISH_TABLE_FOR_IBM_ARABIC_238_L, .xkb_layout = "ara", .xkb_variant = "buckwalter"},
	{.rdp_layout_code = KBD_SPANISH, .xkb_layout = "es", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SPANISH_VARIATION, .xkb_layout = "es", .xkb_variant = "nodeadkeys"},
	{.rdp_layout_code = KBD_FINNISH, .xkb_layout = "fi", .xkb_variant = 0},
	{.rdp_layout_code = KBD_FRENCH, .xkb_layout = "fr", .xkb_variant = 0},
	{.rdp_layout_code = KBD_FRENCH_STANDARD_BEPO, .xkb_layout = "fr", .xkb_variant = "bepo"},
	{.rdp_layout_code = KBD_FRENCH_STANDARD_AZERTY, .xkb_layout = "fr", .xkb_variant = "afnor"},
	{.rdp_layout_code = KBD_HEBREW, .xkb_layout = "il", .xkb_variant = 0},
	{.rdp_layout_code = KBD_HEBREW_STANDARD, .xkb_layout = "il", .xkb_variant = "basic"},
	{.rdp_layout_code = KBD_HUNGARIAN, .xkb_layout = "hu", .xkb_variant = 0},
	{.rdp_layout_code = KBD_HUNGARIAN_101_KEY, .xkb_layout = "hu", .xkb_variant = "standard"},
	{.rdp_layout_code = KBD_ICELANDIC, .xkb_layout = "is", .xkb_variant = 0},
	{.rdp_layout_code = KBD_ITALIAN, .xkb_layout = "it", .xkb_variant = 0},
	{.rdp_layout_code = KBD_ITALIAN_142, .xkb_layout = "it", .xkb_variant = "nodeadkeys"},
	{.rdp_layout_code = KBD_JAPANESE, .xkb_layout = "jp", .xkb_variant = 0},
	{.rdp_layout_code = KBD_JAPANESE_INPUT_SYSTEM_MS_IME2002, .xkb_layout = "jp", .xkb_variant = 0},
	{.rdp_layout_code = KBD_KOREAN, .xkb_layout = "kr", .xkb_variant = 0},
	{.rdp_layout_code = KBD_KOREAN_INPUT_SYSTEM_IME_2000, .xkb_layout = "kr", .xkb_variant = "kr104"},
	{.rdp_layout_code = KBD_DUTCH, .xkb_layout = "nl", .xkb_variant = 0},
	{.rdp_layout_code = KBD_NORWEGIAN, .xkb_layout = "no", .xkb_variant = 0},
	{.rdp_layout_code = KBD_POLISH_PROGRAMMERS, .xkb_layout = "pl", .xkb_variant = 0},
	{.rdp_layout_code = KBD_POLISH_214, .xkb_layout = "pl", .xkb_variant = "qwertz"},
	{.rdp_layout_code = KBD_ROMANIAN, .xkb_layout = "ro", .xkb_variant = 0},
	{.rdp_layout_code = KBD_RUSSIAN, .xkb_layout = "ru", .xkb_variant = 0},
	{.rdp_layout_code = KBD_RUSSIAN_TYPEWRITER, .xkb_layout = "ru", .xkb_variant = "typewriter"},
	{.rdp_layout_code = KBD_CROATIAN, .xkb_layout = "hr", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SLOVAK, .xkb_layout = "sk", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SLOVAK_QWERTY, .xkb_layout = "sk", .xkb_variant = "qwerty"},
	{.rdp_layout_code = KBD_ALBANIAN, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_SWEDISH, .xkb_layout = "se", .xkb_variant = 0},
	{.rdp_layout_code = KBD_THAI_KEDMANEE, .xkb_layout = "th", .xkb_variant = 0},
	{.rdp_layout_code = KBD_THAI_KEDMANEE_NON_SHIFTLOCK, .xkb_layout = "th", .xkb_variant = "tis"},
	{.rdp_layout_code = KBD_TURKISH_Q, .xkb_layout = "tr", .xkb_variant = 0},
	{.rdp_layout_code = KBD_TURKISH_F, .xkb_layout = "tr", .xkb_variant = "f"},
	{.rdp_layout_code = KBD_URDU, .xkb_layout = "in", .xkb_variant = "urd-phonetic3"},
	{.rdp_layout_code = KBD_UKRAINIAN, .xkb_layout = "ua", .xkb_variant = 0},
	{.rdp_layout_code = KBD_BELARUSIAN, .xkb_layout = "by", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SLOVENIAN, .xkb_layout = "si", .xkb_variant = 0},
	{.rdp_layout_code = KBD_ESTONIAN, .xkb_layout = "ee", .xkb_variant = 0},
	{.rdp_layout_code = KBD_LATVIAN, .xkb_layout = "lv", .xkb_variant = 0},
	{.rdp_layout_code = KBD_LITHUANIAN, .xkb_layout = "lt", .xkb_variant = 0},
	{.rdp_layout_code = KBD_LITHUANIAN_IBM, .xkb_layout = "lt", .xkb_variant = "ibm"},
	{.rdp_layout_code = KBD_FARSI, .xkb_layout = "ir", .xkb_variant = "pes"},
	{.rdp_layout_code = KBD_PERSIAN, .xkb_layout = "af", .xkb_variant = "basic"},
	{.rdp_layout_code = KBD_VIETNAMESE, .xkb_layout = "vn", .xkb_variant = 0},
	{.rdp_layout_code = KBD_ARMENIAN_EASTERN, .xkb_layout = "am", .xkb_variant = 0},
	{.rdp_layout_code = KBD_AZERI_LATIN, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_FYRO_MACEDONIAN, .xkb_layout = "mk", .xkb_variant = 0},
	{.rdp_layout_code = KBD_GEORGIAN, .xkb_layout = "ge", .xkb_variant = 0},
	{.rdp_layout_code = KBD_FAEROESE, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_DEVANAGARI_INSCRIPT, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_MALTESE_47_KEY, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_NORWEGIAN_WITH_SAMI, .xkb_layout = "no", .xkb_variant = "smi"},
	{.rdp_layout_code = KBD_KAZAKH, .xkb_layout = "kz", .xkb_variant = 0},
	{.rdp_layout_code = KBD_KYRGYZ_CYRILLIC, .xkb_layout = "kg", .xkb_variant = "phonetic"},
	{.rdp_layout_code = KBD_TATAR, .xkb_layout = "ru", .xkb_variant = "tt"},
	{.rdp_layout_code = KBD_BENGALI, .xkb_layout = "bd", .xkb_variant = 0},
	{.rdp_layout_code = KBD_BENGALI_INSCRIPT, .xkb_layout = "bd", .xkb_variant = "probhat"},
	{.rdp_layout_code = KBD_PUNJABI, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_GUJARATI, .xkb_layout = "in", .xkb_variant = "guj"},
	{.rdp_layout_code = KBD_TAMIL, .xkb_layout = "in", .xkb_variant = "tam"},
	{.rdp_layout_code = KBD_TELUGU, .xkb_layout = "in", .xkb_variant = "tel"},
	{.rdp_layout_code = KBD_KANNADA, .xkb_layout = "in", .xkb_variant = "kan"},
	{.rdp_layout_code = KBD_MALAYALAM, .xkb_layout = "in", .xkb_variant = "mal"},
	{.rdp_layout_code = KBD_HINDI_TRADITIONAL, .xkb_layout = "in", .xkb_variant = 0},
	{.rdp_layout_code = KBD_MARATHI, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_MONGOLIAN_CYRILLIC, .xkb_layout = "mn", .xkb_variant = 0},
	{.rdp_layout_code = KBD_UNITED_KINGDOM_EXTENDED, .xkb_layout = "gb", .xkb_variant = "intl"},
	{.rdp_layout_code = KBD_SYRIAC, .xkb_layout = "syc", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SYRIAC_PHONETIC, .xkb_layout = "syc", .xkb_variant = "syc_phonetic"},
	{.rdp_layout_code = KBD_NEPALI, .xkb_layout = "np", .xkb_variant = 0},
	{.rdp_layout_code = KBD_PASHTO, .xkb_layout = "af", .xkb_variant = "ps"},
	{.rdp_layout_code = KBD_DIVEHI_PHONETIC, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_LUXEMBOURGISH, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_MAORI, .xkb_layout = "mao", .xkb_variant = 0},
	{.rdp_layout_code = KBD_CHINESE_SIMPLIFIED_US, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_SWISS_GERMAN, .xkb_layout = "ch", .xkb_variant = "de_nodeadkeys"},
	{.rdp_layout_code = KBD_UNITED_KINGDOM, .xkb_layout = "gb", .xkb_variant = 0},
	{.rdp_layout_code = KBD_LATIN_AMERICAN, .xkb_layout = "latam", .xkb_variant = 0},
	{.rdp_layout_code = KBD_BELGIAN_FRENCH, .xkb_layout = "be", .xkb_variant = 0},
	{.rdp_layout_code = KBD_BELGIAN_PERIOD, .xkb_layout = "be", .xkb_variant = "oss_sundeadkeys"},
	{.rdp_layout_code = KBD_PORTUGUESE, .xkb_layout = "pt", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SERBIAN_LATIN, .xkb_layout = "rs", .xkb_variant = 0},
	{.rdp_layout_code = KBD_AZERI_CYRILLIC, .xkb_layout = "az", .xkb_variant = "cyrillic"},
	{.rdp_layout_code = KBD_SWEDISH_WITH_SAMI, .xkb_layout = "se", .xkb_variant = "smi"},
	{.rdp_layout_code = KBD_UZBEK_CYRILLIC, .xkb_layout = "af", .xkb_variant = "uz"},
	{.rdp_layout_code = KBD_INUKTITUT_LATIN, .xkb_layout = "ca", .xkb_variant = "ike"},
	{.rdp_layout_code = KBD_CANADIAN_FRENCH_LEGACY, .xkb_layout = "ca", .xkb_variant = "fr-legacy"},
	{.rdp_layout_code = KBD_SERBIAN_CYRILLIC, .xkb_layout = "rs", .xkb_variant = 0},
	{.rdp_layout_code = KBD_CANADIAN_FRENCH, .xkb_layout = "ca", .xkb_variant = 0},
	{.rdp_layout_code = KBD_SWISS_FRENCH, .xkb_layout = "ch", .xkb_variant = "fr"},
	{.rdp_layout_code = KBD_BOSNIAN, .xkb_layout = "ba", .xkb_variant = 0},
	{.rdp_layout_code = KBD_IRISH, .xkb_layout = 0, .xkb_variant = 0},
	{.rdp_layout_code = KBD_BOSNIAN_CYRILLIC, .xkb_layout = "ba", .xkb_variant = "us"},
	{.rdp_layout_code = KBD_UNITED_STATES_DVORAK, .xkb_layout = "us", .xkb_variant = "dvorak"},
	{.rdp_layout_code = KBD_PORTUGUESE_BRAZILIAN_ABNT2, .xkb_layout = "br", .xkb_variant = "abnt2"},
	{.rdp_layout_code = KBD_CANADIAN_MULTILINGUAL_STANDARD, .xkb_layout = "ca", .xkb_variant = "multix"},
	{.rdp_layout_code = KBD_GAELIC, .xkb_layout = "ie", .xkb_variant = "CloGaelach"},

	{.rdp_layout_code = 0x00000000, .xkb_layout = 0, .xkb_variant = 0},
};

void wsland_backend_fetch_keyboard_layout(struct wsland_backend *backend, struct xkb_rule_names *rules) {
    memset(rules, 0, sizeof(*rules));

    rules->model = "pc105";
	uint32_t kt = freerdp_settings_get_uint32(backend->client->peer->context->settings, FreeRDP_KeyboardType);
	uint32_t kl = freerdp_settings_get_uint32(backend->client->peer->context->settings, FreeRDP_KeyboardLayout);
	uint32_t kst = freerdp_settings_get_uint32(backend->client->peer->context->settings, FreeRDP_KeyboardSubType);

	struct backend_keyboard_layout bkl;
    for (int idx = 0; (bkl = backend_keyboards[idx]).rdp_layout_code; idx++) {
        if (bkl.rdp_layout_code == kl) {
            rules->layout = bkl.xkb_layout;
            rules->variant = bkl.xkb_variant;
            break;
        }
    }

    if (kt == 8 && (kl & 0xFFFF) == 0x412) {
        if (kst == 0 || kst == 3) {
	        rules->variant = "kr104";
        } else if (kst == 6) {
	        rules->variant = "kr106";
        }
    } else if (kt != 7 && (kl & 0xFFFF) == 0x411) {
        rules->layout = "us";
        rules->variant = 0;
    }
}