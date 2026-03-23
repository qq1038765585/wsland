// ReSharper disable All
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <wlr/types/wlr_data_device.h>

#include "wsland/adapter.h"
#include "wsland/freerdp.h"
#include "wsland/utils/log.h"

#define CF_PRIVATE_RTF  49309
#define CF_PRIVATE_HTML 49405
#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])

struct clipboard_data_source;
typedef bool (*pfn_process_data)(struct clipboard_data_source *source, bool send);

struct clipboard_supported_format {
    uint32_t format_id;
    char *format_name;
    char *mime_type;

    pfn_process_data pfn;
};

static bool clipboard_process_text_utf8(struct clipboard_data_source *source, bool send);
static bool clipboard_process_text_raw(struct clipboard_data_source *source, bool send);
static bool clipboard_process_html(struct clipboard_data_source *source, bool send);
static bool clipboard_process_bmp(struct clipboard_data_source *source , bool send);

struct clipboard_supported_format clipboard_supported_formats[] = {
    { CF_UNICODETEXT,  NULL,               "text/plain;charset=utf-8", clipboard_process_text_utf8 },
    { CF_TEXT,         NULL,               "STRING",                   clipboard_process_text_raw  },
    { CF_DIB,          NULL,               "image/bmp",                clipboard_process_bmp       },
    { CF_PRIVATE_RTF,  "Rich Text Format", "text/rtf",                 clipboard_process_text_raw  },
    { CF_PRIVATE_HTML, "HTML Format",      "text/html",                clipboard_process_html      },
};

static const char clipboard_html_header[] = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:00000000\r\nEndFragment:00000000\r\n";
static const char clipboard_html_fragment_start[] = "<!--StartFragment-->\r\n";
static const char clipboard_html_fragment_end[] = "<!--EndFragment-->\r\n";

#define WSLAND_CLIPBOARD_FRAGMENT_START_OFFSET (53)
#define WSLAND_CLIPBOARD_FRAGMENT_END_OFFSET (75)

#define DIB_WIDTH_BYTES(bits) ((((bits) + 31) & ~31) >> 3)
#define DIB_HEADER_MARKER     ((WORD) ('M' << 8) | 'B')

#define WSLAND_NUM_CLIPBOARD_FORMATS ARRAY_LENGTH(clipboard_supported_formats)

enum clipboard_data_source_state {
    WSLAND_CLIPBOARD_SOURCE_ALLOCATED = 0,
    WSLAND_CLIPBOARD_SOURCE_FORMATLIST_READY,
    WSLAND_CLIPBOARD_SOURCE_PUBLISHED,
    WSLAND_CLIPBOARD_SOURCE_REQUEST_DATA,
    WSLAND_CLIPBOARD_SOURCE_RECEIVED_DATA,
    WSLAND_CLIPBOARD_SOURCE_TRANSFERING,
    WSLAND_CLIPBOARD_SOURCE_TRANSFERRED,
    WSLAND_CLIPBOARD_SOURCE_CANCEL_PENDING,
    WSLAND_CLIPBOARD_SOURCE_CANCELED,
    WSLAND_CLIPBOARD_SOURCE_RETRY,
    WSLAND_CLIPBOARD_SOURCE_FAILED,
};

typedef struct clipboard_data_source {
    struct wlr_data_source source;
    struct dispatch_task task;
    struct wl_array ctxs;

    int refcount;
    int format_index;
    int data_source_fd;
    uint32_t data_response_fail_count;
    uint32_t client_format_id_table[WSLAND_NUM_CLIPBOARD_FORMATS];
    struct wl_event_source *transfer_event_source;
    enum clipboard_data_source_state state;
    bool processed;
    bool canceled;

    void *inflight_data_to_write;
    uint32_t inflight_write_count;
    size_t inflight_data_size;

    void *processed_data_start;
    uint32_t processed_data_size;
    bool processed_send;

    wsland_peer *peer;
} clipboard_data_source;

typedef struct clipboard_data_request {
    struct dispatch_task task;
    uint32_t requested_format_index;
    wsland_peer *peer;
} clipboard_data_request;

static int clipboard_find_supported(UINT32 format_id, const char *format_name) {
    uint32_t i;

    for (i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if ((format->format_name == NULL && format_id == format->format_id) ||
            (format->format_name && format_name && strcmp(format_name, format->format_name) == 0)) {
            return i;
        }
    }
    return -1;
}

static int clipboard_find_supported_by_id(UINT32 format_id) {
    uint32_t i;

    for (i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if (format_id == format->format_id) {
            return i;
        }
    }
    return -1;
}

static int clipboard_find_supported_by_type(const char *mime_type) {
    uint32_t i;

    for (i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if (strcmp(mime_type, format->mime_type) == 0) {
            return i;
        }
    }
    return -1;
}

static void clipboard_data_source_unref(struct clipboard_data_source *source) {
    source->refcount--;
    if (source->refcount > 0) {
        return;
    }

    if (source->transfer_event_source) {
        wl_event_source_remove(source->transfer_event_source);
    }

    if (source->data_source_fd != -1) {
        close(source->data_source_fd);
    }

    wl_array_release(&source->ctxs);
    free(source);
}

static bool clipboard_process_text_utf8(struct clipboard_data_source *source, bool send) {
    struct wl_array ctxs;
    wl_array_init(&ctxs);

    if (send) {
        char *data = source->ctxs.data;
        size_t data_size, data_size_in_char;

        data[source->ctxs.size] = '\0';
        source->ctxs.size++;

        data_size = MultiByteToWideChar(CP_UTF8, 0, data, source->ctxs.size, NULL, 0);
        if (data_size < 1) {
            goto release;
        }

        data_size *= 2;
        if (!wl_array_add(&ctxs, data_size)) {
            goto release;
        }

        data_size_in_char = MultiByteToWideChar(CP_UTF8, 0, data, source->ctxs.size, ctxs.data, data_size);
        assert(ctxs.size == (data_size_in_char * 2));
    } else {
        size_t data_size;
        LPWSTR data = source->ctxs.data;
        size_t data_size_in_char = source->ctxs.size / 2;

        while (data_size_in_char && ((data[data_size_in_char-1] == L'\0') || (data[data_size_in_char-1] == L'\n'))) {
            data_size_in_char -= 1;
        }
        if (!data_size_in_char) {
            goto release;
        }

        data_size = WideCharToMultiByte(CP_UTF8, 0, source->ctxs.data, data_size_in_char, NULL, 0, NULL, NULL);
        if (data_size < 1) {
            goto release;
        }

        if (!wl_array_add(&ctxs, data_size)) {
            goto release;
        }

        data_size = WideCharToMultiByte(CP_UTF8, 0, source->ctxs.data, data_size_in_char, ctxs.data, data_size, NULL, NULL);
        if (ctxs.size != data_size) {
            goto release;
        }
    }

    wl_array_release(&source->ctxs);
    source->ctxs = ctxs;
    source->processed = true;
    source->processed_data_start = source->ctxs.data;
    source->processed_data_size = source->ctxs.size;
    return true;

release:
    source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
    wl_array_release(&ctxs);
    return false;
}

static bool clipboard_process_text_raw(struct clipboard_data_source *source, bool send) {
    char *data = source->ctxs.data;
    size_t data_size = source->ctxs.size;

    if (send) {
        data[data_size] = '\0';
        source->ctxs.size++;
    } else {
        while (data_size && ((data[data_size-1] == '\0') || (data[data_size-1] == '\n'))) {
            data_size -= 1;
        }
        source->ctxs.size = data_size;
    }

    source->processed = true;
    source->processed_data_start = source->ctxs.data;
    source->processed_data_size = source->ctxs.size;
    return true;
}

static bool clipboard_process_html(struct clipboard_data_source *source, bool send) {
    struct wl_array ctxs;
    char *cur = source->ctxs.data;

    ((char *)(source->ctxs.data))[source->ctxs.size] = '\0';
    wl_array_init(&ctxs);

    cur = strstr(cur, "<html");
    if (!cur) {
        goto release;
    }

    if (!send) {
        size_t data_size = source->ctxs.size - (cur - (char *)source->ctxs.data);
        while (data_size && ((cur[data_size-1] == '\0') || (cur[data_size-1] == '\n'))) {
            data_size -= 1;
        }
        if (!data_size) {
            goto release;
        }
        if (!wl_array_add(&ctxs, data_size+1)) {
            goto release;
        }
        memcpy(ctxs.data, cur, data_size);
        ((char *)(ctxs.data))[data_size] = '\0';
        ctxs.size = data_size;
    } else {
        char *last, *buf;
        uint32_t fragment_start, fragment_end;

        if (!wl_array_add(&ctxs, source->ctxs.size+200)) {
            goto release;
        };
        buf = ctxs.data;
        strcpy(buf, clipboard_html_header);
        last = cur;
        cur = strstr(cur, "<body");
        if (!cur) {
            goto release;
        }
        cur += 5;
        while (*cur != '>' && *cur != '\0') {
            cur++;
        }
        if (*cur == '\0') {
            goto release;
        }
        cur++; /* include '>' */
        strncat(buf, last, cur-last);
        last = cur;
        fragment_start = strlen(buf);
        strcat(buf, clipboard_html_fragment_start);
        cur = strstr(cur, "</body");
        if (!cur)
            goto release;
        strncat(buf, last, cur-last);
        fragment_end = strlen(buf);
        strcat(buf, clipboard_html_fragment_end);
        strcat(buf, cur);

        cur = buf + WSLAND_CLIPBOARD_FRAGMENT_START_OFFSET;
        sprintf(cur, "%08u", fragment_start);
        *(cur+8) = '\r';
        cur = buf + WSLAND_CLIPBOARD_FRAGMENT_END_OFFSET;
        sprintf(cur, "%08u", fragment_end);
        *(cur+8) = '\r';

        ctxs.size = strlen(buf) + 1;
    }

    wl_array_release(&source->ctxs);
    source->ctxs = ctxs;

    source->processed = true;
    source->processed_data_start = source->ctxs.data;
    source->processed_data_size = source->ctxs.size;
    return true;

release:
    source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
    wl_array_release(&ctxs);
    return false;
}

static bool clipboard_process_bmp(struct clipboard_data_source *source , bool send) {
    struct wl_array ctxs;
    wl_array_init(&ctxs);

    BITMAPFILEHEADER *bmfh = NULL;
    BITMAPINFOHEADER *bmih = NULL;
    if (send) {
        if (source->ctxs.size <= sizeof(*bmfh)) {
            goto release;
        }

        bmfh = source->ctxs.data;
        bmih = (BITMAPINFOHEADER *)(bmfh + 1);

        source->processed = true;
        source->processed_data_start = bmih;
        source->processed_data_size = source->ctxs.size - sizeof(*bmfh);
    } else {
        BITMAPFILEHEADER _bmfh = {0};
        if (source->ctxs.size <= sizeof(*bmih)) {
            goto release;
        }

        uint32_t color_table_size = 0;
        bmih = source->ctxs.data;
        bmfh = &_bmfh;
        if (bmih->biCompression == BI_BITFIELDS) {
            color_table_size = sizeof(RGBQUAD) * 3;
        }
        else {
            color_table_size = sizeof(RGBQUAD) * bmih->biClrUsed;
        }

        bmfh->bfType = DIB_HEADER_MARKER;
        bmfh->bfOffBits = sizeof(*bmfh) + bmih->biSize + color_table_size;
        if (bmih->biSizeImage) {
            bmfh->bfSize = bmfh->bfOffBits + bmih->biSizeImage;
        }
        else if (bmih->biCompression == BI_BITFIELDS || bmih->biCompression == BI_RGB) {
            bmfh->bfSize = bmfh->bfOffBits +
                (DIB_WIDTH_BYTES(bmih->biWidth * bmih->biBitCount) * abs(bmih->biHeight));
        }
        else {
            goto release;
        }

        if (source->ctxs.size < (bmfh->bfSize - sizeof(*bmfh))) {
            goto release;
        }

        if (!wl_array_add(&ctxs, bmfh->bfSize)) {
            goto release;
        }
        assert(ctxs.size == bmfh->bfSize);

        memcpy(ctxs.data, bmfh, sizeof(*bmfh));
        memcpy((char *)ctxs.data + sizeof(*bmfh), source->ctxs.data, bmfh->bfSize - sizeof(*bmfh));

        wl_array_release(&source->ctxs);
        source->ctxs = ctxs;
        source->processed = true;
        source->processed_data_start = source->ctxs.data;
        source->processed_data_size = source->ctxs.size;
    }
    return true;

release:
    source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
    wl_array_release(&ctxs);
    return false;
}

static int wsland_wl_array_read_fd(struct wl_array *array, int fd) {
    int len, size;
    char *data;

    if (array->alloc - array->size < 1024) {
        if (!wl_array_add(array, 1024)) {
            errno = ENOMEM;
            return -1;
        }
        array->size -= 1024;
    }

    data = (char *)array->data + array->size;
    size = array->alloc - array->size - 1;
    do {
        len = read(fd, data, size);
    } while (len == -1 && errno == EINTR);

    if (len == -1) {
        return -1;
    }

    array->size += len;
    return len;
}

static void clipboard_send_format_data_response(wsland_peer *peer, struct clipboard_data_source *source) {
    CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
    formatDataResponse.msgType = CB_FORMAT_DATA_RESPONSE;
    formatDataResponse.msgFlags = CB_RESPONSE_OK;
    formatDataResponse.dataLen = source->processed_data_size;
    formatDataResponse.requestedFormatData = source->processed_data_start;
    peer->ctx_server_clipboard->ServerFormatDataResponse(peer->ctx_server_clipboard, &formatDataResponse);
}

static void clipboard_send_format_data_response_fail(wsland_peer *peer, clipboard_data_source *source) {
    if (source) {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        source->data_response_fail_count++;
    }

    CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
    formatDataResponse.msgType = CB_FORMAT_DATA_RESPONSE;
    formatDataResponse.msgFlags = CB_RESPONSE_FAIL;
    formatDataResponse.dataLen = 0;
    formatDataResponse.requestedFormatData = NULL;
    peer->ctx_server_clipboard->ServerFormatDataResponse(peer->ctx_server_clipboard, &formatDataResponse);
}

static bool clipboard_process_source(struct clipboard_data_source *source, bool send) {
    if (source->processed) {
        return true;
    }

    source->processed_data_start = NULL;
    source->processed_data_size = 0;

    if (clipboard_supported_formats[source->format_index].pfn) {
        return clipboard_supported_formats[source->format_index].pfn(source, send);
    }

    source->processed = true;
    source->processed_data_start = source->ctxs.data;
    source->processed_data_size = source->ctxs.size;
    source->processed_send = send;
    return true;
}

static int clipboard_data_source_read(int fd, uint32_t mask, void *user_data) {
    clipboard_data_source *source = wl_container_of(user_data, source, source);
    source->state = WSLAND_CLIPBOARD_SOURCE_TRANSFERING;

    bool failed = true;
    int len = wsland_wl_array_read_fd(&source->ctxs, fd);
    if (len < 0) {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        goto release;
    }

    if (len > 0) {
        return 0;
    }

    source->state = WSLAND_CLIPBOARD_SOURCE_TRANSFERRED;
    if (!source->ctxs.size) {
        goto release;
    }
    if (!clipboard_process_source(source, true)) {
        goto release;
    }

    clipboard_send_format_data_response(source->peer, source);
    failed = false;

release:
    if (failed) {
        clipboard_send_format_data_response_fail(source->peer, source);
    }
    clipboard_data_source_unref(source);
    return 0;
}

static int clipboard_data_source_write(int fd, uint32_t mask, void *user_data) {
    clipboard_data_source *source = wl_container_of(user_data, source, source);

    if (source->canceled) {
        source->state = WSLAND_CLIPBOARD_SOURCE_CANCELED;
        goto release;
    }

    if (!source->ctxs.data || !source->ctxs.size) {
        goto release;
    }

    ssize_t size;
    size_t data_size;
    void *data_to_write;
    if (source->inflight_data_to_write) {
        data_to_write = source->inflight_data_to_write;
        data_size = source->inflight_data_size;
    } else {
        fcntl(source->data_source_fd, F_SETFL, O_WRONLY | O_NONBLOCK);
        clipboard_process_source(source, false);
        data_to_write = source->processed_data_start;
        data_size = source->processed_data_size;
    }

    while (data_to_write && data_size) {
        source->state = WSLAND_CLIPBOARD_SOURCE_TRANSFERING;

        do {
            size = write(source->data_source_fd, data_to_write, data_size);
        } while (size == -1 && errno == EINTR);

        if (size <= 0) {
            if (errno != EAGAIN) {
                source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
                break;
            }
            source->inflight_data_to_write = data_to_write;
            source->inflight_data_size = data_size;
            source->inflight_write_count++;
            return 0;
        } else {
            data_size -= size;
            data_to_write = (char *)data_to_write + size;

            if (!data_size) {
                source->state = WSLAND_CLIPBOARD_SOURCE_TRANSFERRED;
            }
        }
    }

release:
    close(source->data_source_fd);
    source->data_source_fd = -1;
    source->peer->clipboard_inflight_source = NULL;
    wl_event_source_remove(source->transfer_event_source);
    source->transfer_event_source = NULL;
    source->inflight_data_to_write = NULL;
    source->inflight_write_count = 0;
    source->inflight_data_size = 0;
    clipboard_data_source_unref(source);
    return 0;
}

static void clipboard_data_source_fail(bool free_only, void *user_data) {
    clipboard_data_source *source = wl_container_of(user_data, source, task);

    if (!source->ctxs.size) {
        source->format_index = -1;
    }

    close(source->data_source_fd);
    source->data_source_fd = -1;
    source->peer->clipboard_inflight_source = NULL;
    clipboard_data_source_unref(source);
}

static void clipboard_data_source_publish(bool free_only, void *user_data) {
    clipboard_data_source *source = wl_container_of(user_data, source, task);
    wsland_server *server = source->peer->freerdp->adapter->server;
    wsland_peer *peer = source->peer;

    clipboard_data_source *before = peer->clipboard_source;
    if (!free_only) {
        peer->clipboard_source = source;
        source->state = WSLAND_CLIPBOARD_SOURCE_PUBLISHED;
        source->transfer_event_source = NULL;

        int serial = wl_display_next_serial(server->display);
        wlr_seat_set_selection(server->seat, &source->source, serial);
    } else {
        peer->clipboard_source = NULL;
        clipboard_data_source_unref(source);
    }

    if (before) {
        clipboard_data_source_unref(before);
    }
}

static void clipboard_data_source_send(struct wlr_data_source* user_data, const char* mime_type, int32_t fd) {
    clipboard_data_source *source = wl_container_of(user_data, source, source);
    wsland_server *server = source->peer->freerdp->adapter->server;

    if (source->peer->clipboard_inflight_source) {
        if (source == source->peer->clipboard_inflight_source) {
            source->state = WSLAND_CLIPBOARD_SOURCE_RETRY;
            source->peer->clipboard_inflight_source->data_source_fd = fd;
            return;
        } else {
            source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
            close(fd);
            return;
        }
    }

    if (source->source.mime_types.size == 0) {
        close(fd);
        return;
    }

    int index = clipboard_find_supported_by_type(mime_type);
    if (index >= 0 && source->client_format_id_table[index]) {
        source->peer->clipboard_inflight_source = source;
        source->data_source_fd = fd;
        source->refcount++;

        if (index == source->format_index) {
            source->state = WSLAND_CLIPBOARD_SOURCE_RECEIVED_DATA;

            source->transfer_event_source = wl_event_loop_add_fd(
                server->event_loop, fd, WL_EVENT_WRITABLE, clipboard_data_source_write, source
            );
            if (!source->transfer_event_source) {
                source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
                source->peer->clipboard_inflight_source = NULL;
                source->data_source_fd = -1;
                clipboard_data_source_unref(source);
                close(fd);
                return;
            }
        } else {
            wl_array_release(&source->ctxs);
            wl_array_init(&source->ctxs);
            source->processed = false;
            source->format_index = index;

            CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest = {0};
            formatDataRequest.dataLen = 4;
            formatDataRequest.msgType = CB_FORMAT_DATA_REQUEST;
            formatDataRequest.requestedFormatId = source->client_format_id_table[index];
            source->state = WSLAND_CLIPBOARD_SOURCE_REQUEST_DATA;
            if (source->peer->ctx_server_clipboard->ServerFormatDataRequest(source->peer->ctx_server_clipboard, &formatDataRequest) != 0) {
                source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
                source->peer->clipboard_inflight_source = NULL;
                source->data_source_fd = -1;
                clipboard_data_source_unref(source);
                close(fd);
                return;
            }
        }
    } else {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        close(fd);
        return;
    }
}

static void clipboard_data_source_accept(struct wlr_data_source* user_data, uint32_t serial, const char* mime_type) {
    clipboard_data_source *source = wl_container_of(user_data, source, source);
}

static void clipboard_data_source_destroy(struct wlr_data_source* user_data) {
    clipboard_data_source *source = wl_container_of(user_data, source, source);

    if (source == source->peer->clipboard_inflight_source) {
        source->state = WSLAND_CLIPBOARD_SOURCE_CANCEL_PENDING;
        source->canceled = true;
        return;
    }

    source->state = WSLAND_CLIPBOARD_SOURCE_CANCELED;
    wl_array_release(&source->ctxs);
    wl_array_init(&source->ctxs);
    source->processed = false;
    source->format_index = -1;
    memset(source->client_format_id_table, 0, sizeof(source->client_format_id_table));
    source->inflight_write_count = 0;
    source->inflight_data_to_write = NULL;
    source->inflight_data_size = 0;
    if (source->data_source_fd != -1) {
        close(source->data_source_fd);
        source->data_source_fd = -1;
    }
}

struct wlr_data_source_impl clipboard_data_source_impl = {
    .destroy = clipboard_data_source_destroy,
    .accept = clipboard_data_source_accept,
    .send = clipboard_data_source_send
};

static void clipboard_data_source_request(bool free_only, void *user_data) {
    clipboard_data_request *request = wl_container_of(user_data, request, task);
    wsland_server *server = request->peer->freerdp->adapter->server;
    struct wlr_data_source *data_source = server->seat->selection_source;

    if (free_only || !data_source) {
        goto release;
    }

    int index = request->requested_format_index;
    const char *requested_mime_type = clipboard_supported_formats[index].mime_type;

    const char **mime_type;
    bool found_requested_format = FALSE;
    wl_array_for_each(mime_type, &data_source->mime_types) {
        if (strcmp(requested_mime_type, *mime_type) == 0) {
            found_requested_format = true;
            break;
        }
    }

    if (!found_requested_format) {
        goto release_response;
    }

    clipboard_data_source *source = malloc(sizeof(*source));
    wlr_data_source_init(&source->source, &clipboard_data_source_impl);
    source->state = WSLAND_CLIPBOARD_SOURCE_PUBLISHED;
    wl_array_init(&source->ctxs);
    source->processed = false;
    source->peer = request->peer;
    source->refcount = 1;
    source->data_source_fd = -1;
    source->format_index = index;

    int p[2] = {0};
    if (pipe2(p, O_CLOEXEC) == -1) {
        goto release_source;
    }

    source->data_source_fd = p[0];
    source->state = WSLAND_CLIPBOARD_SOURCE_REQUEST_DATA;
    wlr_data_source_send(data_source, requested_mime_type, p[1]);

    source->transfer_event_source = wl_event_loop_add_fd(server->event_loop, p[0], WL_EVENT_READABLE, clipboard_data_source_read, source);
    if (!source->transfer_event_source) {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        goto release_source;
    }

    free(request);
    return;

release_source:
    clipboard_data_source_unref(source);
release_response:
        clipboard_send_format_data_response_fail(request->peer, NULL);
    release:
        free(request);
}

static UINT clipboard_client_temp_directory(CliprdrServerContext *context, const CLIPRDR_TEMP_DIRECTORY *temp_dir) {
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_capabilities(CliprdrServerContext *context, const CLIPRDR_CAPABILITIES *capabilities) {
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_list(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *format_list) {
    clipboard_data_source *source = malloc(sizeof(*source));
    source->state = WSLAND_CLIPBOARD_SOURCE_ALLOCATED;
    source->peer = context->custom;

    wlr_data_source_init(&source->source, &clipboard_data_source_impl);
    wl_array_init(&source->ctxs);
    source->data_source_fd = -1;
    source->format_index = -1;
    source->refcount = 1;

    char **mime_type_addr, *mime_type;
    for (uint32_t i = 0; i < format_list->numFormats; i++) {
        CLIPRDR_FORMAT *format = &format_list->formats[i];
        int index = clipboard_find_supported(format->formatId, format->formatName);

        if (index >= 0) {
            source->client_format_id_table[index] = format->formatId;
            mime_type = strdup(clipboard_supported_formats[index].mime_type);

            if (mime_type) {
                mime_type_addr = wl_array_add(&source->source.mime_types, sizeof(*mime_type_addr));

                if (mime_type_addr) {
                    *mime_type_addr = mime_type;
                } else {
                    free(mime_type);
                }
            }
        }
    }

    source->state = WSLAND_CLIPBOARD_SOURCE_FORMATLIST_READY;
    dispatch_to_display(context->custom, clipboard_data_source_publish, &source->task);

    CLIPRDR_FORMAT_LIST_RESPONSE formatListResponse = {0};
    formatListResponse.msgType = CB_FORMAT_LIST_RESPONSE;
    formatListResponse.msgFlags = source ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
    formatListResponse.dataLen = 0;

    if (source->peer->ctx_server_clipboard->ServerFormatListResponse(source->peer->ctx_server_clipboard, &formatListResponse) != 0) {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        return -1;
    }
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_list_response(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *response) {
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_data_request(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *data_request) {
    wsland_peer *peer = context->custom;

    int index = clipboard_find_supported_by_id(data_request->requestedFormatId);

    if (index < 0) {
        goto release;
    }

    clipboard_data_request *request = malloc(sizeof(*request));
    request->peer = peer;
    request->requested_format_index = index;

    dispatch_to_display(peer, clipboard_data_source_request, &request->task);
    return CHANNEL_RC_OK;

release:
    clipboard_send_format_data_response_fail(peer, NULL);
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_data_response(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
    wsland_peer *peer = context->custom;
    clipboard_data_source *source = peer->clipboard_inflight_source;
    wsland_server *server = source->peer->freerdp->adapter->server;

    if (!source) {
        return -1;
    }

    if (source->transfer_event_source || (source->inflight_write_count != 0)) {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        return -1;
    }

    bool success = false;
    if (response->msgFlags == CB_RESPONSE_OK) {
        if (wl_array_add(&source->ctxs, response->dataLen+1)) {
            memcpy(source->ctxs.data, response->requestedFormatData, response->dataLen);
            source->ctxs.size = response->dataLen;
            ((char *)source->ctxs.data)[source->ctxs.size] = '\0';
            source->state = WSLAND_CLIPBOARD_SOURCE_RECEIVED_DATA;
            success = true;
        } else {
            source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        }
    } else {
        source->state = WSLAND_CLIPBOARD_SOURCE_FAILED;
        source->data_response_fail_count++;
    }

    if (success) {
        source->transfer_event_source = wl_event_loop_add_fd(
            server->event_loop, source->data_source_fd, WL_EVENT_WRITABLE, clipboard_data_source_write, source
        );
        if (!source->transfer_event_source) {
            return -1;
        }
    } else {
        dispatch_to_display(peer, clipboard_data_source_fail, &source->task);
    }

    return CHANNEL_RC_OK;
}


static void set_selection(struct wl_listener *listener, void *user_data) {
    wsland_adapter *adapter = wl_container_of(listener, adapter, events.set_selection);
    wsland_peer *peer = adapter->freerdp->peer;

    struct wlr_seat *seat = user_data;
    struct wlr_data_source *data_source = seat->selection_source;

    if (!peer || !data_source) {
        return;
    }

    if (data_source->impl->accept == clipboard_data_source_accept) {
        return;
    }

    clipboard_data_source *source;
    if (peer->clipboard_source) {
        source = peer->clipboard_source;
        peer->clipboard_source = NULL;
        clipboard_data_source_unref(source);
    }

    int index = 0;
    const char **mime_type;
    int num_supported_format = 0;
    CLIPRDR_FORMAT formats[WSLAND_NUM_CLIPBOARD_FORMATS] = {0};
    wl_array_for_each(mime_type, &data_source->mime_types) {
        index = clipboard_find_supported_by_type(*mime_type);

        if (index >= 0) {
            CLIPRDR_FORMAT *format = &formats[num_supported_format];

            format->formatId = clipboard_supported_formats[index].format_id;
            format->formatName = clipboard_supported_formats[index].format_name;
            num_supported_format++;
        }
    }

    if (num_supported_format) {
        CLIPRDR_FORMAT_LIST formatList = {0};
        formatList.msgType = CB_FORMAT_LIST;
        formatList.numFormats = num_supported_format;
        formatList.formats = &formats[0];

        peer->ctx_server_clipboard->ServerFormatList(peer->ctx_server_clipboard, &formatList);
    }
}

bool rail_clipboard_init(wsland_peer *peer) {
    CliprdrServerContext *clipboard = cliprdr_server_context_new(peer->vcm);
    if (!clipboard) {
        return false;
    }
    peer->ctx_server_clipboard = clipboard;

    clipboard->custom = peer;
    clipboard->TempDirectory = clipboard_client_temp_directory;
    clipboard->ClientCapabilities = clipboard_client_capabilities;
    clipboard->ClientFormatList = clipboard_client_format_list;
    clipboard->ClientFormatListResponse = clipboard_client_format_list_response;
    clipboard->ClientFormatDataRequest = clipboard_client_format_data_request;
    clipboard->ClientFormatDataResponse = clipboard_client_format_data_response;
    clipboard->useLongFormatNames = FALSE;
    clipboard->streamFileClipEnabled = FALSE;
    clipboard->fileClipNoFilePaths = FALSE;
    clipboard->canLockClipData = TRUE;

    if (clipboard->Start(peer->ctx_server_clipboard) != 0) {
        cliprdr_server_context_free(peer->ctx_server_clipboard);
        peer->ctx_server_clipboard = NULL;
        return false;
    }

    wsland_adapter *adapter = peer->freerdp->adapter;
    LISTEN(&adapter->server->seat->events.set_selection, &adapter->events.set_selection, set_selection);
    return true;
}

void rail_clipboard_destroy(wsland_peer *peer) {
    if (peer->freerdp->adapter->events.set_selection.notify) {
        wl_list_remove(&peer->freerdp->adapter->events.set_selection.link);
        peer->freerdp->adapter->events.set_selection.notify = NULL;
    }

    clipboard_data_source *source;
    if (peer->clipboard_inflight_source) {
        source = peer->clipboard_inflight_source;
        peer->clipboard_inflight_source = NULL;
        clipboard_data_source_unref(source);
    }

    if (peer->clipboard_source) {
        source = peer->clipboard_source;
        peer->clipboard_source = NULL;
        clipboard_data_source_unref(source);
    }

    if (peer->ctx_server_clipboard) {
        peer->ctx_server_clipboard->Stop(peer->ctx_server_clipboard);
        cliprdr_server_context_free(peer->ctx_server_clipboard);
        peer->ctx_server_clipboard = NULL;
    }
}