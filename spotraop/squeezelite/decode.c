/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  (c) Philippe, philippe_44@outlook.com for raop/multi-instance modifications
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// decode thread

#include "squeezelite.h"

#define READ_SIZE  512
#define WRITE_SIZE 32 * 1024

extern log_level 	decode_loglevel;
static log_level 	*loglevel = &decode_loglevel;

struct codec	*codecs[MAX_CODECS];

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)

#if PROCESS
#define IF_DIRECT(x)    if (ctx->decode.direct) { x }
#define IF_PROCESS(x)   if (!ctx->decode.direct) { x }
#define MAY_PROCESS(x)  { x }
#else
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#define MAY_PROCESS(x)
#endif


/*---------------------------------------------------------------------------*/
static void *decode_thread(struct thread_ctx_s *ctx) {
	while (ctx->decode_running) {
		size_t bytes, space, min_space;
		bool toend;
		bool ran = false;

		LOCK_S;
		bytes = _buf_used(ctx->streambuf);
		toend = (ctx->stream.state <= DISCONNECT);
		UNLOCK_S;
		LOCK_O;
		space = _buf_space(ctx->outputbuf);
		UNLOCK_O;

		LOCK_D;

		if (ctx->decode.state == DECODE_RUNNING && ctx->codec) {

			LOG_SDEBUG("streambuf bytes: %u outputbuf space: %u", bytes, space);

			IF_DIRECT(
				min_space = ctx->codec->min_space;
			);
			IF_PROCESS(
				min_space = ctx->process.max_out_frames * BYTES_PER_FRAME;
			);

			if (space > min_space && (bytes > ctx->codec->min_read_bytes || toend)) {

				ctx->decode.state = ctx->codec->decode(ctx);

				IF_PROCESS(
					if (ctx->process.in_frames) {
						process_samples(ctx);
					}

					if (ctx->decode.state == DECODE_COMPLETE) {
						process_drain(ctx);
					}
				);

				if (ctx->decode.state != DECODE_RUNNING) {

					LOG_INFO("decode %s", ctx->decode.state == DECODE_COMPLETE ? "complete" : "error");

					LOCK_O;
					if (ctx->output.fade_mode) _checkfade(false, ctx);
					UNLOCK_O;

					wake_controller(ctx);
				}

				ran = true;
			}
		}

		UNLOCK_D;

		if (!ran) {
			usleep(100000);
		}
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
void decode_init(void) {
	int i = 0;

	codecs[i++] = register_pcm();
	codecs[i++] = register_mad();
	codecs[i++] = register_alac();
	codecs[i++] = register_flac();
	codecs[i++] = register_faad();
	codecs[i++] = register_vorbis();
	codecs[i++] = register_opus();
#if RESAMPLE
	register_soxr();
#endif
}


/*---------------------------------------------------------------------------*/
void decode_end(void) {
	deregister_pcm();
	deregister_mad();
	deregister_flac();
	deregister_alac();
	deregister_faad();
	deregister_vorbis();
	deregister_opus();
#if RESAMPLE
	deregister_soxr();
#endif
}


/*---------------------------------------------------------------------------*/
void decode_thread_init(struct thread_ctx_s *ctx) {
	pthread_attr_t attr;

	LOG_DEBUG("[%p]: init decode", ctx);
	mutex_create(ctx->decode.mutex);

	ctx->decode_running = true;
	ctx->decode.new_stream = true;
	ctx->decode.state = DECODE_STOPPED;
	ctx->decode.handle = NULL;
#if PROCESS
	ctx->decode.process_handle = NULL;
#endif

	MAY_PROCESS(
		ctx->decode.direct = true;
		ctx->decode.process = false;
	);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + DECODE_THREAD_STACK_SIZE);
	pthread_create(&ctx->decode_thread, &attr, (void *(*)(void*)) decode_thread, ctx);
	pthread_attr_destroy(&attr);
}

/*---------------------------------------------------------------------------*/
void decode_close(struct thread_ctx_s *ctx) {

	LOG_DEBUG("close decode", NULL);
	LOCK_D;
	if (ctx->codec) {
		ctx->codec->close(ctx);
		ctx->codec = NULL;
	}
	ctx->decode_running = false;
	UNLOCK_D;
	pthread_join(ctx->decode_thread, NULL);
	mutex_destroy(ctx->decode.mutex);
}

/*---------------------------------------------------------------------------*/
void decode_flush(struct thread_ctx_s *ctx) {

	LOG_DEBUG("[%p]: decode flush", ctx);
	LOCK_D;
	ctx->decode.state = DECODE_STOPPED;
	IF_PROCESS(
		process_flush(ctx);
	);
	UNLOCK_D;
}

/*---------------------------------------------------------------------------*/
unsigned decode_newstream(unsigned sample_rate, int supported_rates[], struct thread_ctx_s *ctx) {
	// called with O locked to get sample rate for potentially processed output stream
	// release O mutex during process_newstream as it can take some time

	MAY_PROCESS(
		if (ctx->decode.process) {
			UNLOCK_O;
			sample_rate = process_newstream(&ctx->decode.direct, sample_rate, supported_rates, ctx);
			LOCK_O;
		}
	);

	return sample_rate;
}

/*---------------------------------------------------------------------------*/
bool codec_open(u8_t codec, u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	int i;

	LOG_DEBUG("codec open: '%c'", codec);

	LOCK_D;

	ctx->decode.new_stream = true;
	ctx->decode.state = DECODE_STOPPED;

	MAY_PROCESS(
		ctx->decode.direct = true; // potentially changed within codec when processing enabled
	);

	// find the required codec
	for (i = 0; i < MAX_CODECS; ++i) {

		if (codecs[i] && codecs[i]->id == codec) {

			if (ctx->codec && ctx->codec != codecs[i]) {
				LOG_DEBUG("closing codec: '%c'", ctx->codec->id);
				ctx->codec->close(ctx);
			}

			ctx->codec = codecs[i];
			ctx->codec->open(sample_size, sample_rate, channels, endianness, ctx);
			ctx->decode.state = DECODE_READY;

			UNLOCK_D;
			return true;
		}
	}

	UNLOCK_D;

	LOG_ERROR("codec not found", NULL);

	return false;
}

