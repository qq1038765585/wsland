#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "common/logger.h"
#include "backend.h"
#include "adapter.h"


#define CF_PRIVATE_RTF  49309
#define CF_PRIVATE_HTML 49405
#define ARRAY_LENGTH(a) (sizeof (a) / sizeof (a)[0])
// ReSharper disable CppUseInternalLinkage

struct clipboard_data_source;
typedef bool (*pfn_process_data)(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);

struct clipboard_supported_format {
    uint32_t format_id;
    char *format_name;
    char *mime_type;

    pfn_process_data pfn;
};

static bool clipboard_process_text_utf8(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_text_raw(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_html(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);
static bool clipboard_process_bmp(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count);

// ReSharper disable CppUseDesignatedInitializers
static struct clipboard_supported_format clipboard_supported_formats[] = {
    { CF_UNICODETEXT,  NULL,               "text/plain;charset=utf-8", clipboard_process_text_utf8 },
    { CF_TEXT,         NULL,               "STRING",                   clipboard_process_text_raw  },
    { CF_DIB,          NULL,               "image/bmp",                clipboard_process_bmp       },
    { CF_PRIVATE_RTF,  "Rich Text Format", "text/rtf",                 clipboard_process_text_raw  },
    { CF_PRIVATE_HTML, "HTML Format",      "text/html",                clipboard_process_html      },
};

// static const char clipboard_html_header[] = "Version:0.9\r\nStartHTML:-1\r\nEndHTML:-1\r\nStartFragment:00000000\r\nEndFragment:00000000\r\n";
// static const char clipboard_html_fragment_start[] = "<!--StartFragment-->\r\n";
// static const char clipboard_html_fragment_end[] = "<!--EndFragment-->\r\n";

#define WSLAND_CLIPBOARD_FRAGMENT_START_OFFSET (53)
#define WSLAND_CLIPBOARD_FRAGMENT_END_OFFSET (75)

#define DIB_WIDTH_BYTES(bits) ((((bits) + 31) & ~31) >> 3)
#define DIB_HEADER_MARKER     ((WORD) ('M' << 8) | 'B')

#define WSLAND_NUM_CLIPBOARD_FORMATS ARRAY_LENGTH(clipboard_supported_formats)

struct windows_provide_format_task {
    struct backend_task task;

    struct clipboard_response_source *source;
};

struct windows_response_data_task {
    struct backend_task task;

    struct windows_response_data_event *event;
};

struct windows_response_data_event {
    struct clipboard_response_source *source;

    uint8_t *data;
    uint32_t length;

    struct wl_event_source *write_source;
};

struct windows_request_data_task {
    struct backend_task task;

    struct clipboard_supported_format *format;
};

struct clipboard_request_source {
    struct clipboard_supported_format *format;
    int32_t read_fd, write_fd;

    struct wl_event_source *read_source;
    struct wl_list link;
};

struct clipboard_response_source {
    struct wlr_data_source source;
    uint32_t serial;

    struct {
        int32_t fd;
        struct clipboard_supported_format *format;
    } request;

    struct backend_client *client;
    struct wl_list link;
};

static int clipboard_find_supported(UINT32 format_id, const char *format_name) {
    for (uint32_t i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if ((format->format_name == NULL && format_id == format->format_id) ||
            (format->format_name && format_name && strcmp(format_name, format->format_name) == 0)) {
            return (int)i;
        }
    }
    return -1;
}

static struct clipboard_supported_format * clipboard_find_supported_by_id(UINT32 format_id) {
    for (uint32_t i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if (format_id == format->format_id) {
            return format;
        }
    }
    return NULL;
}

static struct clipboard_supported_format * clipboard_find_supported_by_type(const char *mime_type) {
    for (uint32_t i = 0; i < WSLAND_NUM_CLIPBOARD_FORMATS; i++) {
        struct clipboard_supported_format *format = &clipboard_supported_formats[i];

        if (strcmp(mime_type, format->mime_type) == 0) {
            return format;
        }
    }
    return NULL;
}

static bool clipboard_process_text_utf8(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    if (to_utf8) {
        *target_data = ConvertWCharNToUtf8Alloc(source_data, source_count, target_count);
    } else {
        size_t length = 0;
        *target_data = ConvertUtf8NToWCharAlloc(source_data, source_count, &length);
        *target_count = (length + 1) * sizeof(WCHAR);
    }

    free(source_data);
    return *target_data == NULL ? false : true;
}

static bool clipboard_process_text_raw(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    free(source_data);
    return false;
}

static bool clipboard_process_html(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    free(source_data);
    return false;
}

static bool clipboard_process_bmp(bool to_utf8, void *source_data, size_t source_count, void **target_data, size_t *target_count) {
    free(source_data);
    return false;
}

static void clipboard_send_format_data_response_fail(struct backend_client *client) {
    CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
    formatDataResponse.common.msgType = CB_FORMAT_DATA_RESPONSE;
    formatDataResponse.common.msgFlags = CB_RESPONSE_FAIL;
    formatDataResponse.common.dataLen = 0;
    formatDataResponse.requestedFormatData = NULL;
    client->clipboard_ctx->ServerFormatDataResponse(client->clipboard_ctx, &formatDataResponse);
}

static UINT clipboard_client_temp_directory(CliprdrServerContext *context, const CLIPRDR_TEMP_DIRECTORY *temp_dir) {
    backend_log(DEBUG, "invoke ==> clipboard_client_temp_directory");
    return CHANNEL_RC_OK;
}

static UINT clipboard_client_capabilities(CliprdrServerContext *context, const CLIPRDR_CAPABILITIES *capabilities) {
    backend_log(DEBUG, "invoke ==> clipboard_client_capabilities");
    return CHANNEL_RC_OK;
}

static void handle_next_windows_response(struct backend_client *client) {
    if (client->clipboard.response_flight || wl_list_empty(&client->event.clipboard_response)) {
        return;
    }

    struct clipboard_response_source *data_source = wl_container_of(client->event.clipboard_response.next, data_source, link);
    wl_list_remove(&data_source->link);
    wl_list_init(&data_source->link);

    client->clipboard.response_flight = data_source;
    CLIPRDR_FORMAT_DATA_REQUEST formatDataRequest = {0};
    formatDataRequest.common.msgType = CB_FORMAT_DATA_REQUEST;
    formatDataRequest.requestedFormatId = data_source->request.format->format_id;
    formatDataRequest.common.dataLen = 4;

    if (client->clipboard_ctx->ServerFormatDataRequest(client->clipboard_ctx, &formatDataRequest) != 0) {
        client->clipboard.response_flight = NULL;

        close(data_source->request.fd);
        free(data_source);

        handle_next_windows_response(client);
    }
}

static void windows_clipboard_destroy(struct wlr_data_source* user_data) {
    struct clipboard_response_source *source = wl_container_of(user_data, source, source);

    free(source);
}

static void windows_clipboard_accept(struct wlr_data_source *user_data, uint32_t serial, const char *mime_type) {
    struct clipboard_response_source *source = wl_container_of(user_data, source, source);

    if (source->serial != serial) {
        source->serial = 0;
    }
}

static void windows_clipboard_send(struct wlr_data_source* user_data, const char* mime_type, int32_t fd) {
    struct clipboard_response_source *source = wl_container_of(user_data, source, source);

    struct clipboard_supported_format *format = clipboard_find_supported_by_type(mime_type);
    if (!source->serial || !format) {
        close(fd);
        return;
    }

    source->request.fd = fd;
    source->request.format = format;
    wl_list_insert(source->client->event.clipboard_response.prev, &source->link);
    handle_next_windows_response(source->client);
}

static struct wlr_data_source_impl windows_clipboard_impl = {
    .destroy = windows_clipboard_destroy,
    .accept = windows_clipboard_accept,
    .send = windows_clipboard_send
};

static void windows_provide_format_callback(bool free_only, void *user_data) {
    struct windows_provide_format_task *task = wl_container_of(user_data, task, task);

    if (!free_only) {
        struct clipboard_response_source *source = task->source;
        struct wsland_adapter *adapter = source->client->backend->adapter;

        struct wlr_seat *seat = adapter->handle->server_seat_fetch(adapter->server);
        source->serial = wl_display_next_serial(seat->display);
        wlr_seat_set_selection(seat, &source->source, source->serial);
    }

    free(task);
}

static UINT windows_provide_format(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST *format_list) {
    struct backend_client *client = context->custom;

    struct windows_provide_format_task *task = malloc(sizeof(*task));
    task->source = malloc(sizeof(*task->source));

    wlr_data_source_init(&task->source->source, &windows_clipboard_impl);
    task->source->client = client;

    for (uint32_t idx = 0; idx < format_list->numFormats; ++idx) {
        CLIPRDR_FORMAT *format = &format_list->formats[idx];
        int format_idx = clipboard_find_supported(format->formatId, format->formatName);

        if (format_idx >= 0) {
            char *mime_type = clipboard_supported_formats[format_idx].mime_type;
            if (!mime_type) { continue; }

            char **mime_type_ptr = wl_array_add(&task->source->source.mime_types, sizeof(*mime_type_ptr));
            *mime_type_ptr = strdup(mime_type);
        }
    }

    backend_task_dispatch(client, windows_provide_format_callback, &task->task);

    CLIPRDR_FORMAT_LIST_RESPONSE formatListResponse = {0};
    formatListResponse.common.msgType = CB_FORMAT_LIST_RESPONSE;
    formatListResponse.common.msgFlags = CB_RESPONSE_OK;
    formatListResponse.common.dataLen = 0;

    if (client->clipboard_ctx->ServerFormatListResponse(client->clipboard_ctx, &formatListResponse) != 0) {
        return -1;
    }

    return CHANNEL_RC_OK;
}

static UINT clipboard_client_format_list_response(CliprdrServerContext *context, const CLIPRDR_FORMAT_LIST_RESPONSE *response) {
    backend_log(DEBUG, "invoke ==> clipboard_client_format_list_response");
    return CHANNEL_RC_OK;
}

static bool read_fd_data(int32_t fd, void **data, size_t *count) {
    size_t capacity = 4096;
    size_t size = 0;

    uint8_t *buffer = malloc(capacity);
    if (!buffer) { return false; }

    while (true) {
        if (size == capacity) {
            capacity *= 2;
            uint8_t *temp = realloc(buffer, capacity);
            if (!temp) {
                free(buffer);
                return false;
            }
            buffer = temp;
        }

        ssize_t length = read(fd, buffer + size, capacity - size);
        if (length > 0) {
            size += length;
            continue;
        }

        if (length == 0) { break; }

        if (errno == EINTR) { continue; }

        if (errno == EAGAIN || errno == EWOULDBLOCK) { break; }

        free(buffer);
        return false;
    }

    *data = buffer;
    *count = size;
    return true;
}

static void handle_next_clipboard_request(struct backend_client *client);
static int clipboard_request_data_read(int fd, uint32_t mask, void *user_data) {
    struct backend_client *client = user_data;
    struct clipboard_request_source *source = client->clipboard.request_flight;

    wl_event_source_remove(source->read_source);
    client->clipboard.request_flight = NULL;

    if (source) {
        void *source_data;
        size_t source_count;

        if (read_fd_data(source->read_fd, &source_data, &source_count)) {
            void *target_data;
            size_t target_count;

            if (source->format->pfn(false, source_data, source_count, &target_data, &target_count)) {
                CLIPRDR_FORMAT_DATA_RESPONSE formatDataResponse = {0};
                formatDataResponse.common.msgType = CB_FORMAT_DATA_RESPONSE;
                formatDataResponse.common.msgFlags = CB_RESPONSE_OK;
                formatDataResponse.requestedFormatData = target_data;
                formatDataResponse.common.dataLen = target_count;
                client->clipboard_ctx->ServerFormatDataResponse(client->clipboard_ctx, &formatDataResponse);
                free(target_data);
            }
        } else {
            clipboard_send_format_data_response_fail(client);
        }
        free(source);
    }

    handle_next_clipboard_request(client);
    close(fd);
    return 0;
}

static void handle_next_clipboard_request(struct backend_client *client) {
    if (client->clipboard.request_flight || wl_list_empty(&client->event.clipboard_request)) {
        return;
    }

    struct clipboard_request_source *event = wl_container_of(client->event.clipboard_request.next, event, link);
    wl_list_remove(&event->link);
    wl_list_init(&event->link);

    struct wlr_seat *seat = client->backend->adapter->handle->server_seat_fetch(client->backend->adapter->server);
    struct wlr_data_source *source = seat->selection_source;

    if (!source) {
        close(event->write_fd);
        close(event->read_fd);
        free(event);

        clipboard_send_format_data_response_fail(client);
        handle_next_clipboard_request(client);
        return;
    }

    client->clipboard.request_flight = event;
    struct wsland_backend *backend = client->backend;
    event->read_source = backend->adapter->handle->server_dispatch_event_source(
        backend->adapter, event->read_fd, WL_EVENT_READABLE, clipboard_request_data_read, client
    );

    wlr_data_source_send(source, event->format->mime_type, event->write_fd);
}

static void windows_request_data_callback(bool free_only, void *user_data) {
    struct windows_request_data_task *task = wl_container_of(user_data, task, task);

    if (!free_only) {
        struct backend_client *client = task->task.client;

        int p[2] = {0};
        if (pipe2(p, O_CLOEXEC) == -1) {
            clipboard_send_format_data_response_fail(client);
            goto release;
        }

        struct clipboard_request_source *source = malloc(sizeof(*source));
        source->format = task->format;
        source->write_fd = p[1];
        source->read_fd = p[0];

        wl_list_insert(client->event.clipboard_request.prev, &source->link);
        handle_next_clipboard_request(client);
    }

    release:
        free(task);
}

static UINT windows_request_data(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_REQUEST *data_request) {
    struct backend_client *client = context->custom;

    struct clipboard_supported_format *format = clipboard_find_supported_by_id(data_request->requestedFormatId);
    if (!format) {
        clipboard_send_format_data_response_fail(client);
        return CHANNEL_RC_OK;
    }

    struct windows_request_data_task *task = malloc(sizeof(*task));
    task->format = format;

    backend_task_dispatch(client, windows_request_data_callback, &task->task);
    return CHANNEL_RC_OK;
}

static int windows_response_data_write(int fd, uint32_t mask, void *user_data) {
    struct windows_response_data_event *event = user_data;

    wl_event_source_remove(event->write_source);
    write(fd, event->data, event->length);

    free(event->data);
    free(event);
    close(fd);
    return 0;
}

static void windows_response_data_callback(bool free_only, void *user_data) {
    struct windows_response_data_task *task = wl_container_of(user_data, task, task);

    if (!free_only) {
        struct backend_client *client = task->task.client;
        struct windows_response_data_event *event = task->event;
        struct clipboard_response_source *source = client->clipboard.response_flight;
        client->clipboard.response_flight = NULL;

        if (source) {
            event->source = source;

            void *target_data;
            size_t target_count;
            if (source->request.format->pfn(true, event->data, event->length, &target_data, &target_count)) {
                event->length = target_count;
                event->data = target_data;

                struct wsland_backend *backend = client->backend;
                event->write_source = backend->adapter->handle->server_dispatch_event_source(
                    backend->adapter, source->request.fd, WL_EVENT_WRITABLE, windows_response_data_write, event
                );

                if (!event->write_source) {
                    close(source->request.fd);
                    free(event->data);
                    free(event);
                }
            } else {
                close(source->request.fd);
                free(event);
            }
        } else {
            close(source->request.fd);
            free(event->data);
            free(event);
        }

        handle_next_windows_response(client);
    }

    free(task);
}

static UINT windows_response_data(CliprdrServerContext *context, const CLIPRDR_FORMAT_DATA_RESPONSE *response) {
    struct backend_client *client = context->custom;

    struct windows_response_data_task *task = malloc(sizeof(*task));

    task->event = malloc(sizeof(*task->event));
    task->event->data = malloc(response->common.dataLen);
    memcpy(task->event->data, response->requestedFormatData, response->common.dataLen);
    task->event->length = response->common.dataLen;

    backend_task_dispatch(client, windows_response_data_callback, &task->task);
    return CHANNEL_RC_OK;
}

void linux_provide_format(struct wsland_adapter *adapter, struct wlr_seat *seat) {
    struct backend_client *client = adapter->backend->client;

    struct wlr_data_source *source = seat->selection_source;
    if (!source || source->impl->accept == windows_clipboard_accept) {
        return;
    }

    int num_supported_format = 0;
    CLIPRDR_FORMAT formats[WS_CLIPBOARD_MAX_FORMATS] = {0};

    char **mime_type;
    client->clipboard.count = 0;
    wl_array_for_each(mime_type, &source->mime_types) {
        struct clipboard_supported_format *format = clipboard_find_supported_by_type(*mime_type);

        if (format) {
            CLIPRDR_FORMAT *clip_format = &formats[num_supported_format++];
            clip_format->formatId = format->format_id;
            clip_format->formatName = format->format_name;
            client->clipboard.format_ids[client->clipboard.count++] = clip_format->formatId;
        }
    }

    if (num_supported_format) {
        CLIPRDR_FORMAT_LIST formatList = {0};
        formatList.common.msgType = CB_FORMAT_LIST;
        formatList.numFormats = num_supported_format;
        formatList.formats = &formats[0];

        client->clipboard_ctx->ServerFormatList(client->clipboard_ctx, &formatList);
    }
}

bool backend_client_clipboard_init(struct backend_client *client) {
    CliprdrServerContext *clipboard = cliprdr_server_context_new(client->vcm);
    if (!clipboard) {
        return false;
    }
    client->clipboard_ctx = clipboard;

    clipboard->custom = client;
    clipboard->TempDirectory = clipboard_client_temp_directory;
    clipboard->ClientCapabilities = clipboard_client_capabilities;
    clipboard->ClientFormatListResponse = clipboard_client_format_list_response;
    clipboard->ClientFormatDataResponse = windows_response_data;
    clipboard->ClientFormatDataRequest = windows_request_data;
    clipboard->ClientFormatList = windows_provide_format;
    clipboard->useLongFormatNames = FALSE;
    clipboard->streamFileClipEnabled = FALSE;
    clipboard->fileClipNoFilePaths = FALSE;
    clipboard->canLockClipData = TRUE;

    if (clipboard->Start(client->clipboard_ctx) != 0) {
        cliprdr_server_context_free(client->clipboard_ctx);
        client->clipboard_ctx = NULL;
        return false;
    }
    return true;
}

void backend_client_clipboard_source_destroy(struct backend_client *client) {
    if (client->clipboard_ctx) {
        client->clipboard_ctx->Stop(client->clipboard_ctx);
        cliprdr_server_context_free(client->clipboard_ctx);
        client->clipboard_ctx = NULL;
    }
}