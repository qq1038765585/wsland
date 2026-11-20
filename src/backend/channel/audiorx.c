#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "common/logger.h"
#include "backend.h"


static AUDIO_FORMAT audio_rx_supported_formats[] = {
    {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 1, .nSamplesPerSec = 44100, .nAvgBytesPerSec = 88200, .nBlockAlign = 2, .wBitsPerSample = 16, .cbSize = 0, .data = NULL},
};

static int audio_rx_setup_listener(struct client_audio_rx *rx) {
    struct sockaddr_un s;

    int fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        backend_log(INFO, "couldn't create audio rx listener socket.");
        return -1;
    }

    char* source_socket_path = getenv("PULSE_AUDIO_RDP_SOURCE");
    if (source_socket_path == NULL || source_socket_path[0] == '\0') {
        close(fd);
        backend_log(INFO, "environment variable `PULSE_AUDIO_RDP_SOURCE` not set.");
        return -1;
    }

    memset(&s, 0, sizeof(s));
    s.sun_family = AF_UNIX;
    int bytes = sizeof(s.sun_path) - 1;
    snprintf(s.sun_path, bytes, "%s", source_socket_path);

    remove(s.sun_path);
    int error = bind(fd, (struct sockaddr*)&s, sizeof(struct sockaddr_un));
    if (error != 0) {
        close(fd);
        backend_log(INFO, "failed to bind to listener socket for audio rx (%d).", error);
        return -1;
    }

    listen(fd, 100);
    return fd;
}

static void * audio_rx_source_thread(void *context) {
    struct client_audio_rx *rx = context;

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

    assert(rx->closeAudioSourceFd != -1);
    assert(rx->pulseAudioSourceListenerFd != -1);

    while (!rx->audioInExitSignal) {
        rx->pulseAudioSourceFd = accept(rx->pulseAudioSourceListenerFd, NULL, NULL);
        if (rx->pulseAudioSourceFd < 0) {
            backend_log(INFO, "client audio rx - source thread: listener connection error (%s)", strerror(errno));
        } else {
            if (rx->rx_ctx->Open(rx->rx_ctx)) {
                uint64_t dummy;
                if (read(rx->closeAudioSourceFd, &dummy, sizeof(dummy)) != sizeof(uint64_t)) {
                    backend_log(INFO, "client audio rx - wait on eventfd failed. thread exiting. %s", strerror(errno));
                    break;
                }
                rx->rx_ctx->Close(rx->rx_ctx);
            } else {
                backend_log(INFO, "client audio rx - failed to open audio in connection with rdp client.");
            }

            close(rx->pulseAudioSourceFd);
            rx->pulseAudioSourceFd = -1;
        }
    }

    if (rx->rx_ctx->IsOpen(rx->rx_ctx))
        rx->rx_ctx->Close(rx->rx_ctx);

    if (rx->pulseAudioSourceFd != -1) {
        close(rx->pulseAudioSourceFd);
        rx->pulseAudioSourceFd = -1;
    }
    return NULL;
}

static UINT audio_rx_client_receive_formats(audin_server_context* context, const SNDIN_FORMATS* formats) {
    struct client_audio_rx* rx = context->userdata;
    const AUDIO_FORMAT* server_format = &audio_rx_supported_formats[0];
    UINT32 matched_index = UINT32_MAX;
    SNDIN_OPEN open = {0};

    for (UINT32 i = 0; i < formats->NumFormats; i++) {
        const AUDIO_FORMAT* cf = &formats->SoundFormats[i];

        if (cf->wFormatTag == server_format->wFormatTag &&
            cf->nChannels == server_format->nChannels &&
            cf->nSamplesPerSec == server_format->nSamplesPerSec &&
            cf->wBitsPerSample == server_format->wBitsPerSample) {
            matched_index = i;
            break;
        }
    }

    if (matched_index == UINT32_MAX) {
        return ERROR_INVALID_DATA;
    }

    /*
     * 10ms of audio per packet — matches the 2.x frames_per_packet
     * setting (nSamplesPerSec / 100).
     */
    open.FramesPerPacket = server_format->nSamplesPerSec / 100;
    open.initialFormat = matched_index;
    open.captureFormat = *server_format;

    rx->isAudioInStreamOpened = TRUE;

    return context->SendOpen(context, &open);
}

static UINT audio_rx_client_data(audin_server_context* context, const SNDIN_DATA* data) {
    struct client_audio_rx* rx = context->userdata;
    const AUDIO_FORMAT* server_format = &audio_rx_supported_formats[0];

    if (!rx->isAudioInStreamOpened || rx->pulseAudioSourceFd == -1) {
        return CHANNEL_RC_OK;
    }

    size_t bytes = Stream_Length(data->Data);
    if (bytes == 0) {
        return CHANNEL_RC_OK;
    }

    const BYTE* buffer = Stream_Buffer(data->Data);
    assert(buffer != NULL);
    assert(server_format->wFormatTag == WAVE_FORMAT_PCM);
    assert(server_format->nChannels == 1);
    assert(server_format->nSamplesPerSec == 44100);
    assert(server_format->wBitsPerSample == 16);

    ssize_t sent = send(rx->pulseAudioSourceFd, buffer, bytes, 0);
    if (sent != (ssize_t)bytes) {
        /* Unblock worker thread to close pipe to pulseaudio */
        uint64_t one = 1;
        if (write(rx->closeAudioSourceFd, &one, sizeof(one)) != sizeof(uint64_t)) {
            return ERROR_INTERNAL_ERROR;
        }

        if (sent <= 0) {
            return ERROR_INTERNAL_ERROR;
        }
    }

    return CHANNEL_RC_OK;
}

bool backend_client_audiorx_init(struct backend_client *client) {
    struct client_audio_rx *rx = malloc(sizeof(*rx));
    rx->rx_ctx = audin_server_context_new(client->vcm);
    if (!rx->rx_ctx) {
        backend_log(INFO, "client audio rx - couldn't initialize audio virtual channel.");
        return true;
    }

    rx->audioInExitSignal = FALSE;
    rx->pulseAudioSourceThread = 0;
    rx->pulseAudioSourceListenerFd = -1;
    rx->pulseAudioSourceFd = -1;
    rx->closeAudioSourceFd = -1;

    AUDIO_FORMAT *audio_formats = malloc(sizeof audio_rx_supported_formats);
    if (!audio_formats) {
        backend_log(INFO, "client audio rx - couldn't allocate memory for audio formats.");

        if (rx->rx_ctx) {
            audin_server_context_free(rx->rx_ctx);
            rx->rx_ctx = NULL;
        }
        free(rx);
        return true;
    }
    memcpy(audio_formats, audio_rx_supported_formats, sizeof audio_rx_supported_formats);

    rx->rx_ctx->userdata = (void*)rx;
    rx->rx_ctx->ReceiveFormats = audio_rx_client_receive_formats;
    rx->rx_ctx->Data = audio_rx_client_data;

    rx->closeAudioSourceFd = eventfd(0, EFD_CLOEXEC);
    if (rx->closeAudioSourceFd < 0) {
        backend_log(INFO, "client audio rx - couldn't initialize eventfd.");

        if (rx->rx_ctx) {
            audin_server_context_free(rx->rx_ctx);
            rx->rx_ctx = NULL;
        }
        free(rx);
        return true;
    }

    rx->pulseAudioSourceListenerFd = audio_rx_setup_listener(rx);
    if (rx->pulseAudioSourceListenerFd < 0) {
        backend_log(INFO, "client audio rx - audio_rx_setup_listener failed.");

        if (rx->closeAudioSourceFd != -1) {
            close(rx->closeAudioSourceFd);
            rx->closeAudioSourceFd = -1;
        }
        if (rx->rx_ctx) {
            audin_server_context_free(rx->rx_ctx);
            rx->rx_ctx = NULL;
        }
        free(rx);
        return true;
    }

    if (pthread_create(&rx->pulseAudioSourceThread, NULL, audio_rx_source_thread, rx) < 0) { // NOLINT(*-posix-return)
        backend_log(INFO, "client audio rx - failed to start pulse audio source thread. no audio in will be available.");

        if (rx->pulseAudioSourceListenerFd != -1) {
            close(rx->pulseAudioSourceListenerFd);
            rx->pulseAudioSourceListenerFd = -1;
        }
        if (rx->closeAudioSourceFd != -1) {
            close(rx->closeAudioSourceFd);
            rx->closeAudioSourceFd = -1;
        }
        if (rx->rx_ctx) {
            audin_server_context_free(rx->rx_ctx);
            rx->rx_ctx = NULL;
        }
        free(rx);
        return true;
    }

    client->audio_rx = rx;
    return true;
}

void backend_client_audio_rx_destroy(struct backend_client *client) {
    if (client->audio_rx->rx_ctx) {
        if (client->audio_rx->pulseAudioSourceThread) {
            client->audio_rx->audioInExitSignal = TRUE;
            shutdown(client->audio_rx->pulseAudioSourceListenerFd, SHUT_RDWR);
            shutdown(client->audio_rx->closeAudioSourceFd, SHUT_RDWR);
            pthread_cancel(client->audio_rx->pulseAudioSourceThread);
            pthread_join(client->audio_rx->pulseAudioSourceThread, NULL);

            if (client->audio_rx->pulseAudioSourceListenerFd != -1) {
                close(client->audio_rx->pulseAudioSourceListenerFd);
                client->audio_rx->pulseAudioSourceListenerFd = -1;
            }

            if (client->audio_rx->closeAudioSourceFd != -1) {
                close(client->audio_rx->closeAudioSourceFd);
                client->audio_rx->closeAudioSourceFd = -1;
            }

            client->audio_rx->pulseAudioSourceThread = 0;
        }

        assert(client->audio_rx->pulseAudioSourceListenerFd < 0);
        assert(client->audio_rx->closeAudioSourceFd < 0);

        assert(!client->audio_rx->rx_ctx->IsOpen(client->audio_rx->rx_ctx));
        audin_server_context_free(client->audio_rx->rx_ctx);
        client->audio_rx->rx_ctx = NULL;
    }
    free(client->audio_rx);
}