#include <assert.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "common/logger.h"
#include "backend.h"


#define AUDIO_LATENCY 5
#define AUDIO_CMD_VERSION 0
#define AUDIO_CMD_TRANSFER 1
#define AUDIO_CMD_GET_LATENCY 2
#define AUDIO_CMD_RESET_LATENCY 3
#define AUDIO_FRAMES_PER_RDP_PACKET (44100 * AUDIO_LATENCY / 1000)
#define SINK_INTERFACE_VERSION 1


struct audio_cmd_header {
	uint32_t cmd;

	union {
		uint32_t version;

		struct {
			uint32_t bytes;
			uint64_t timestamp;
		} transfer;

		// ReSharper disable once CppDeclaratorNeverUsed
		uint64_t reserved[8];
	};
};

static AUDIO_FORMAT audio_tx_supported_formats[] = {
    {.wFormatTag = WAVE_FORMAT_PCM, .nChannels = 2, .nSamplesPerSec = 44100, .nAvgBytesPerSec = 176400, .nBlockAlign = 4, .wBitsPerSample = 16, .cbSize = 0, .data = NULL},
};

static int audio_tx_setup_listener(void) {
	struct sockaddr_un s;

	int fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		backend_log(INFO, "couldn't create listener socket.");
		return -1;
	}

	char* sink_socket_path = getenv("PULSE_AUDIO_RDP_SINK");
	if (sink_socket_path == NULL || sink_socket_path[0] == '\0') {
		close(fd);
		backend_log(INFO, "environment variable `PULSE_AUDIO_RDP_SINK` not set.");
		return -1;
	}

	memset(&s, 0, sizeof(s));
	s.sun_family = AF_UNIX;
	int bytes = sizeof(s.sun_path) - 1;
	snprintf(s.sun_path, bytes, "%s", sink_socket_path);

	remove(s.sun_path);

	backend_log(INFO, "pulse audio sink listener socket on %s", s.sun_path);
	int error = bind(fd, (struct sockaddr*)&s, sizeof(struct sockaddr_un));
	if (error != 0) {
		close(fd);
		backend_log(INFO, "failed to bind to listener socket (%d).", error);
		return -1;
	}

	listen(fd, 100);
	return fd;
}

static UINT64 audio_tx_timestamp(void) {
	struct timespec time;
	clock_gettime(CLOCK_REALTIME, &time);
	return time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

static UINT audio_tx_client_confirm_block(RdpsndServerContext* context, BYTE confirmBlockNum, UINT16 wtimestamp) {
	struct client_audio_tx *tx = context->data;

	struct audio_block_info *abi = &tx->blockInfo[confirmBlockNum];
	if (abi->ackReceivedTime != 0) {
		assert(abi->ackPlayedTime == 0);
		abi->ackPlayedTime = audio_tx_timestamp();

		if (tx->nextValidBlock == -1 || tx->nextValidBlock == confirmBlockNum) {
			tx->nextValidBlock = -1;

			tx->accumulatedRenderedLatency += abi->ackPlayedTime - abi->submissionTime;
			tx->accumulatedRenderedLatencyCount++;
		}

		uint64_t one = 1;
		if (write(tx->audioSem, &one, sizeof(one)) != sizeof(uint64_t)) {
			backend_log(INFO, "rdp audio error at confirm_block while writing to audioSem (%s)\n", strerror(errno));
			return ERROR_INTERNAL_ERROR;
		}

	} else {
		abi->ackReceivedTime = audio_tx_timestamp();

		tx->accumulatedNetworkLatency += abi->ackReceivedTime - abi->submissionTime;
		tx->accumulatedNetworkLatencyCount++;
	}

	return 0;
}

static int audio_tx_handle_version(struct client_audio_tx *tx, UINT PAVersion) {
	uint32_t version = SINK_INTERFACE_VERSION;

	tx->PAVersion = PAVersion;
	backend_log(INFO, "audio sink version (%d - %d)", PAVersion, version);
	ssize_t sizeSent = send(tx->pulseAudioSinkFd, &version, sizeof(version), MSG_DONTWAIT);
	if (sizeSent != sizeof(version)) {
		backend_log(INFO, "audio tx error responding to version request sent:%ld. %s", sizeSent, strerror(errno));
		return -1;
	}

	return 0;
}

static int audio_tx_handle_transfer(struct client_audio_tx *tx, UINT bytesLeft, UINT64 timestamp) {
	int nbFrames = (int)bytesLeft / tx->bytesPerFrame;
	ssize_t sizeRead = 0;
	UINT bytesRead = 0;

	if (!tx->audioBuffer || bytesLeft > tx->audioBufferSize) {
		if(tx->audioBuffer) {
			free(tx->audioBuffer);
		}

		tx->audioBuffer = malloc(bytesLeft);
		if (!tx->audioBuffer) {
			backend_log(INFO, "audio tx error malloc(%d) failed.", bytesLeft);
			return -1;
		}
		tx->audioBufferSize = bytesLeft;
	}

	assert(bytesLeft % tx->bytesPerFrame == 0);
	while (bytesLeft > 0) {
		sizeRead = read(tx->pulseAudioSinkFd, tx->audioBuffer + bytesRead, bytesLeft);
		if (sizeRead <= 0) {
			backend_log(INFO, "audio tx error while reading data from sink socket sizeRead:%ld. %s", sizeRead, strerror(errno));
			return -1;
		}
		bytesRead += sizeRead;
		bytesLeft -= sizeRead;
	}

	BYTE* audioBuffer = tx->audioBuffer;
	while (nbFrames > 0) {
		uint64_t dummy;
		if (read(tx->audioSem, &dummy, sizeof(dummy)) != sizeof(uint64_t)) {
			backend_log(INFO, "audio tx error at handle_transfer while reading from audioSem (%s)", strerror(errno));
			return -1;
		}

		BYTE block_no = tx->tx_ctx->block_no;
		tx->blockInfo[block_no].submissionTime = timestamp;
		tx->blockInfo[block_no].ackReceivedTime = 0;
		tx->blockInfo[block_no].ackPlayedTime = 0;
		if (tx->tx_ctx->SendSamples(tx->tx_ctx, audioBuffer, MIN(nbFrames, AUDIO_FRAMES_PER_RDP_PACKET), 0) != 0) {
			backend_log(INFO, "audio tx error while SendSamples.");
			return -1;
		}

		if (block_no == tx->tx_ctx->block_no) {
			uint64_t one = 1;
			if (write(tx->audioSem, &one, sizeof(one)) != sizeof(uint64_t)) {
				backend_log(INFO, "audio tx error at handle_transfer while writing to audioSem (%s)", strerror(errno));
				return -1;
			}
		} else {
			assert(block_no > tx->tx_ctx->block_no || block_no+1 == tx->tx_ctx->block_no);
			assert(block_no < tx->tx_ctx->block_no || (block_no==255 && tx->tx_ctx->block_no==0));
		}

		audioBuffer += AUDIO_FRAMES_PER_RDP_PACKET * tx->bytesPerFrame;
		nbFrames -= AUDIO_FRAMES_PER_RDP_PACKET;
	}

	return 0;
}

static int audio_tx_handle_get_latency(struct client_audio_tx *tx) {
	UINT networkLatency;
	UINT renderedLatency;
	if (tx->accumulatedNetworkLatencyCount > 0) {
		networkLatency = tx->accumulatedNetworkLatency / tx->accumulatedNetworkLatencyCount;
		tx->lastNetworkLatency = networkLatency;
		tx->accumulatedNetworkLatency = 0;
		tx->accumulatedNetworkLatencyCount = 0;
	} else {
		networkLatency = tx->lastNetworkLatency;
	}

	if (tx->accumulatedRenderedLatencyCount > 0) {
		renderedLatency = tx->accumulatedRenderedLatency / tx->accumulatedRenderedLatencyCount;
		tx->lastRenderedLatency = renderedLatency;
		tx->accumulatedRenderedLatency = 0;
		tx->accumulatedRenderedLatencyCount = 0;
	} else {
		renderedLatency = tx->lastRenderedLatency;
	}

	if (renderedLatency > networkLatency) {
		renderedLatency -= networkLatency;
	}

	ssize_t sizeSent = send(tx->pulseAudioSinkFd, &renderedLatency, sizeof(renderedLatency), MSG_DONTWAIT);
	if (sizeSent != sizeof(renderedLatency)) {
		backend_log(INFO, "rdp audio error responding to latency request sent:%ld. %s", sizeSent, strerror(errno));
		return -1;
	}

	return 0;
}

static void * audio_tx_pulse_audio_sink_thread(void *context) {
	struct client_audio_tx *tx = context;

	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL);

	assert(tx->pulseAudioSinkListenerFd != 0);
	while (!tx->audioExitSignal) {
		assert(tx->pulseAudioSinkFd < 0);
		tx->pulseAudioSinkFd = accept(tx->pulseAudioSinkListenerFd, NULL, NULL);
		if (tx->pulseAudioSinkFd < 0) {
			backend_log(INFO, "audio sink thread: listener connection error (%s)", strerror(errno));
			continue;
		}

		for (;;) {
			struct audio_cmd_header header;

			ssize_t sizeRead = read(tx->pulseAudioSinkFd, &header, sizeof(header));
			if (sizeRead != sizeof(header)) {
				backend_log(INFO, "audio sink thread: error while reading from sink socket sizeRead:%ld. %s", sizeRead, strerror(errno));
				break; // ReSharper disable once CppRedundantElseKeywordInsideCompoundStatement
			} else if (header.cmd == AUDIO_CMD_VERSION) {
				if (audio_tx_handle_version(tx, header.version) < 0) {
					break;
				}
			} else if (header.cmd == AUDIO_CMD_TRANSFER) {
				if (audio_tx_handle_transfer(tx, header.transfer.bytes, header.transfer.timestamp) < 0) {
					break;
				}
			} else if (header.cmd == AUDIO_CMD_GET_LATENCY) {
				if (audio_tx_handle_get_latency(tx) < 0) {
					break;
				}
			} else if (header.cmd == AUDIO_CMD_RESET_LATENCY) {
				tx->nextValidBlock = tx->tx_ctx->block_no;
				tx->lastNetworkLatency = 0;
				tx->accumulatedNetworkLatency = 0;
				tx->accumulatedNetworkLatencyCount = 0;
				tx->lastRenderedLatency = 0;
				tx->accumulatedRenderedLatency = 0;
				tx->accumulatedRenderedLatencyCount = 0;
			} else {
				backend_log(INFO, "audio sink thread: unknown command from sink.");
				break;
			}
		}

		close(tx->pulseAudioSinkFd);
		tx->pulseAudioSinkFd = -1;
	}

	assert(tx->pulseAudioSinkFd < 0);
	return NULL;
}

static void audio_tx_client_activated(RdpsndServerContext* context) {
	struct client_audio_tx *tx = context->data;
	int format = -1;

	for (int i = 0; i < context->num_client_formats; i++) {
		AUDIO_FORMAT *cf = &context->client_formats[i];

		for (int j = 0; j < (int)context->num_server_formats; j++) {
			AUDIO_FORMAT *sf = &context->server_formats[j];

			if (cf->wFormatTag == sf->wFormatTag && cf->nChannels == sf->nChannels && cf->nSamplesPerSec == sf->nSamplesPerSec) {
				format = i;
				break;
			}
		}
	}

	if (format != -1) {
		tx->nextValidBlock = -1;
		tx->bytesPerFrame = context->client_formats[format].wBitsPerSample / 8 * context->client_formats[format].nChannels;
		context->latency = AUDIO_LATENCY;
		context->SelectFormat(context, format);
		context->SetVolume(context, 0x7FFF, 0x7FFF);

		tx->pulseAudioSinkListenerFd = audio_tx_setup_listener();
		if (tx->pulseAudioSinkListenerFd < 0) {
			backend_log(INFO, "audio_tx_client_activated - failed to create listener socket.");
		} else if (pthread_create(&tx->pulseAudioSinkThread, NULL, audio_tx_pulse_audio_sink_thread, tx) < 0) { // NOLINT(*-posix-return)
			backend_log(INFO, "audio_tx_client_activated - failed to start pulse audio sink thread. no audio will be available.");
		}
	} else {
		backend_log(INFO, "audio_tx_client_activated - no agreed format.");
	}
}

bool backend_client_audiotx_init(struct backend_client *client) {
    struct client_audio_tx *tx = malloc(sizeof(*tx));
    tx->tx_ctx = rdpsnd_server_context_new(client->vcm);
    if (!tx->tx_ctx) {
        backend_log(INFO, "client audio tx - couldn't initialize audio virtual channel.");

        free(tx);
        return true;
    }

    tx->audioExitSignal = FALSE;
    tx->pulseAudioSinkThread = 0;
    tx->pulseAudioSinkListenerFd = -1;
    tx->pulseAudioSinkFd = -1;
    tx->audioBuffer = NULL;

    tx->audioSem = eventfd(256, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (!tx->audioSem) {
        backend_log(INFO, "client audio tx - couldn't initialize event semaphore.");

        if (tx->tx_ctx) {
            rdpsnd_server_context_free(tx->tx_ctx);
            tx->tx_ctx = NULL;
        }
        free(tx);
        return true;
    }

    AUDIO_FORMAT *audio_formats = malloc(sizeof audio_tx_supported_formats);
    if (!audio_formats) {
        backend_log(INFO, "client audio tx - couldn't allocate memory for audio formats.");

        if (tx->audioSem != -1) {
            close(tx->audioSem);
            tx->audioSem = -1;
        }
        if (tx->tx_ctx) {
            rdpsnd_server_context_free(tx->tx_ctx);
            tx->tx_ctx = NULL;
        }
        free(tx);
        return true;
    }
    memcpy(audio_formats, audio_tx_supported_formats, sizeof audio_tx_supported_formats);

    tx->tx_ctx->data = (void*)tx;
    tx->tx_ctx->Activated = audio_tx_client_activated;
    tx->tx_ctx->ConfirmBlock = audio_tx_client_confirm_block;
    tx->tx_ctx->num_server_formats = ARRAYSIZE(audio_tx_supported_formats);
    tx->tx_ctx->server_formats = audio_formats;
    tx->tx_ctx->src_format = &audio_tx_supported_formats[0];
    tx->tx_ctx->use_dynamic_virtual_channel = TRUE;

    if (tx->tx_ctx->Initialize(tx->tx_ctx, TRUE) != 0) {
        if (tx->audioSem != -1) {
            close(tx->audioSem);
            tx->audioSem = -1;
        }
        if (tx->tx_ctx) {
            rdpsnd_server_context_free(tx->tx_ctx);
            tx->tx_ctx = NULL;
        }
        free(tx);
    	return true;
    }

	client->audio_tx = tx;
    return true;
}

void backend_client_audio_tx_destroy(struct backend_client *client) {
	if (client->audio_tx->tx_ctx) {
		if (client->audio_tx->pulseAudioSinkThread) {
			client->audio_tx->audioExitSignal = TRUE;
			shutdown(client->audio_tx->pulseAudioSinkListenerFd, SHUT_RDWR);
			shutdown(client->audio_tx->pulseAudioSinkFd, SHUT_RDWR);
			pthread_cancel(client->audio_tx->pulseAudioSinkThread);
			pthread_join(client->audio_tx->pulseAudioSinkThread, NULL);

			if (client->audio_tx->pulseAudioSinkListenerFd != -1) {
				close(client->audio_tx->pulseAudioSinkListenerFd);
				client->audio_tx->pulseAudioSinkListenerFd = -1;
			}

			if (client->audio_tx->pulseAudioSinkFd != -1) {
				close(client->audio_tx->pulseAudioSinkFd);
				client->audio_tx->pulseAudioSinkFd = -1;
			}

			if (client->audio_tx->audioBuffer) {
				free(client->audio_tx->audioBuffer);
				client->audio_tx->audioBuffer = NULL;
			}

			client->audio_tx->pulseAudioSinkThread = 0;
		}

		assert(client->audio_tx->pulseAudioSinkListenerFd < 0);
		assert(client->audio_tx->pulseAudioSinkFd < 0);
		assert(client->audio_tx->audioBuffer == NULL);

		client->audio_tx->tx_ctx->Close(client->audio_tx->tx_ctx);
		client->audio_tx->tx_ctx->Stop(client->audio_tx->tx_ctx);

		if (client->audio_tx->audioSem != -1) {
			close(client->audio_tx->audioSem);
			client->audio_tx->audioSem = -1;
		}

		rdpsnd_server_context_free(client->audio_tx->tx_ctx);
		client->audio_tx->tx_ctx = NULL;
	}
	free(client->audio_tx);
}