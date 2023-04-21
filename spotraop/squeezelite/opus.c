 /*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2017, ralph_irving@hotmail.com
 *      Philippe, philippe_44@outlook.com for raop/multi-instance modifications
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

#include "squeezelite.h"

#include <opusfile.h>

#if !LINKALL
static struct {
	void *handle;
	// opus symbols to be dynamically loaded
	void (*op_free)(OggOpusFile *_of);
	int  (*op_read)(OggOpusFile *_of, opus_int16 *_pcm, int _buf_size, int *_li);
	const OpusHead* (*op_head)(OggOpusFile *_of, int _li);
	OggOpusFile*  (*op_open_callbacks) (void *_source, OpusFileCallbacks *_cb, unsigned char *_initial_data, size_t _initial_bytes, int *_error);
} gu;
#endif

struct opus {
	struct OggOpusFile *of;
	int channels;
};

extern log_level decode_loglevel;
static log_level *loglevel = &decode_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (ctx->decode.direct) mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct if (ctx->decode.direct) mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    if (ctx->decode.direct) { x }
#define IF_PROCESS(x)   if (!ctx->decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(ctx->outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif


#if LINKALL
#define OP(h, fn, ...) (op_ ## fn)(__VA_ARGS__)
#else
#define OP(h, fn, ...) (h)->op_ ## fn(__VA_ARGS__)
#endif

// called with mutex locked within vorbis_decode to avoid locking O before S
static int _read_cb(void *datasource, char *ptr, int size) {
	size_t bytes;
	struct thread_ctx_s *ctx = datasource;

	while (1) {
		LOCK_S;
		bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
		bytes = min(bytes, size);
		if (bytes || ctx->stream.state <= DISCONNECT) break;

		UNLOCK_S;
		usleep(50 * 1000);
	}

	memcpy(ptr, ctx->streambuf->readp, bytes);
	_buf_inc_readp(ctx->streambuf, bytes);
	UNLOCK_S;

	return bytes;
}

static decode_state opus_decompress( struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;
	frames_t frames;
	int n;
	u8_t *write_buf = NULL;

	if (ctx->decode.new_stream) {
		struct OpusFileCallbacks cbs;
		const struct OpusHead *info;
		int err;

		cbs.read = (op_read_func) _read_cb;
		cbs.seek = NULL; cbs.tell = NULL; cbs.close = NULL;

		if ((u->of = OP(&gu, open_callbacks, ctx, &cbs, NULL, 0, &err)) == NULL) {
			LOG_WARN("open_callbacks error: %d", err);
			return DECODE_COMPLETE;
		}

		info = OP(&gu, head, u->of, -1);
		u->channels = info->channel_count;

		LOCK_O;
		ctx->output.current_sample_rate = decode_newstream(48000, ctx->output.supported_rates, ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
		UNLOCK_O;

		if (u->channels > 2) {
			LOG_WARN("[%p]: too many channels: %d", ctx, u->channels);
			return DECODE_ERROR;
		}

		LOG_INFO("[%p]: setting track_start", ctx);
	}

	LOCK_O_direct;
	IF_DIRECT(
		frames = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
		write_buf = ctx->outputbuf->writep;
	);
	IF_PROCESS(
		frames = ctx->process.max_in_frames;
		write_buf = ctx->process.inbuf;

	);

	// write the decoded frames into outputbuf (they are 16 bits)
	n = OP(&gu, read, u->of, (opus_int16*) write_buf, frames * u->channels, NULL);

	if (n > 0) {
		frames_t count;
		s16_t *iptr, *optr;

		frames = n;
		count = frames * u->channels;

		// work backward but no unpack needed
		iptr = (s16_t *)write_buf + count;
		optr = (s16_t *)write_buf + frames * 2;

		if (u->channels == 2) {
			while (count--) {
				*--optr = *--iptr;
			}
		} else if (u->channels == 1) {
			while (count--) {
				*--optr = *--iptr;
				*--optr = *iptr;
			}
		}

		IF_DIRECT(
			_buf_inc_writep(ctx->outputbuf, frames * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			ctx->process.in_frames = frames;
		);

		LOG_SDEBUG("[%p]: wrote %u frames", ctx, frames);

	} else if (n == 0) {

		if (ctx->stream.state <= DISCONNECT) {
			LOG_INFO("[%p]: end of decode", ctx);
			UNLOCK_O_direct;
			return DECODE_COMPLETE;
		} else {
			LOG_INFO("[%p]: no frame decoded", ctx);
        }

	} else if (n == OP_HOLE) {

		// recoverable hole in stream, seen when skipping
		LOG_DEBUG("[%p]: hole in stream", ctx);

	} else {

		LOG_INFO("[%p]: op_read error: %d", ctx, n);
		UNLOCK_O_direct;
		return DECODE_COMPLETE;
	}

	UNLOCK_O_direct;
	return DECODE_RUNNING;
}

static void opus_open(u8_t size, u32_t rate, u8_t chan, u8_t endianness, struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;

	if (!u) {
		u = ctx->decode.handle = malloc(sizeof(struct opus));
		u->of = NULL;
	} else if (u->of) {
		OP(&gu, free, u->of);
		u->of = NULL;
	}
}

static void opus_close(struct thread_ctx_s *ctx) {
	struct opus *u = ctx->decode.handle;

	if (u && u->of) {
		OP(&gu, free, u->of);
	}
	free(u);
	ctx->decode.handle = NULL;
}

static bool load_opus(void) {
#if !LINKALL
	char *err;

	gu.handle = dlopen(LIBOPUS, RTLD_NOW);
	if (!gu.handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	gu.op_free = dlsym(gu.handle, "op_free");
	gu.op_read = dlsym(gu.handle, "op_read");
	gu.op_head = dlsym(gu.handle, "op_head");
	gu.op_open_callbacks = dlsym(gu.handle, "op_open_callbacks");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBOPUS, NULL);
#endif

	return true;
}

struct codec *register_opus(void) {
	static struct codec ret = {
		'u',          // id
		"ops",        // types
		4096,         // min read
		20480,        // min space
		opus_open,    // open
		opus_close,   // close
		opus_decompress,  // decode
	};

	if (!load_opus()) {
		return NULL;
	}

	LOG_INFO("using opus to decode ops", NULL);
	return &ret;
}

void deregister_opus(void) {
#if !LINKALL
	if (gu.handle) dlclose(gu.handle);
#endif
}