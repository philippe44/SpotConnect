/*
 *  Squeezelite - lightweight headless squeezeplay emulator for linux
 *
 *  (c) Adrian Smith 2012, triode1@btinternet.com
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

#include "squeezelite.h"

#include <FLAC/stream_decoder.h>

#if !LINKALL
static struct {
	void *handle;
	// FLAC symbols to be dynamically loaded
	const char **FLAC__StreamDecoderErrorStatusString;
	const char **FLAC__StreamDecoderStateString;
	FLAC__StreamDecoder * (* FLAC__stream_decoder_new)(void);
	FLAC__bool (* FLAC__stream_decoder_reset)(FLAC__StreamDecoder *decoder);
	void (* FLAC__stream_decoder_delete)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderInitStatus (* FLAC__stream_decoder_init_stream)(
		FLAC__StreamDecoder *decoder,
		FLAC__StreamDecoderReadCallback read_callback,
		FLAC__StreamDecoderSeekCallback seek_callback,
		FLAC__StreamDecoderTellCallback tell_callback,
		FLAC__StreamDecoderLengthCallback length_callback,
		FLAC__StreamDecoderEofCallback eof_callback,
		FLAC__StreamDecoderWriteCallback write_callback,
		FLAC__StreamDecoderMetadataCallback metadata_callback,
		FLAC__StreamDecoderErrorCallback error_callback,
		void *client_data
	);
		FLAC__StreamDecoderInitStatus (* FLAC__stream_decoder_init_ogg_stream)(
		FLAC__StreamDecoder *decoder,
		FLAC__StreamDecoderReadCallback read_callback,
		FLAC__StreamDecoderSeekCallback seek_callback,
		FLAC__StreamDecoderTellCallback tell_callback,
		FLAC__StreamDecoderLengthCallback length_callback,
		FLAC__StreamDecoderEofCallback eof_callback,
		FLAC__StreamDecoderWriteCallback write_callback,
		FLAC__StreamDecoderMetadataCallback metadata_callback,
		FLAC__StreamDecoderErrorCallback error_callback,
		void *client_data
	);
	FLAC__bool (* FLAC__stream_decoder_process_single)(FLAC__StreamDecoder *decoder);
	FLAC__StreamDecoderState (* FLAC__stream_decoder_get_state)(const FLAC__StreamDecoder *decoder);
} gf;
#endif

struct flac {
	FLAC__StreamDecoder *decoder;
	u8_t container;
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
#define FLAC(h, fn, ...) (FLAC__ ## fn)(__VA_ARGS__)
#define FLAC_A(h, a)     (FLAC__ ## a)
#else
#define FLAC(h, fn, ...) (h)->FLAC__##fn(__VA_ARGS__)
#define FLAC_A(h, a)     (h)->FLAC__ ## a
#endif

static FLAC__StreamDecoderReadStatus read_cb(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[], size_t *want, void *client_data) {
	size_t bytes;
	bool end;
	struct thread_ctx_s *ctx = (struct thread_ctx_s*) client_data;

	LOCK_S;
	bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
	bytes = min(bytes, *want);
	end = (ctx->stream.state <= DISCONNECT && bytes == 0);

	memcpy(buffer, ctx->streambuf->readp, bytes);
	_buf_inc_readp(ctx->streambuf, bytes);
	UNLOCK_S;

	*want = bytes;

	return end ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus write_cb(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
											   const FLAC__int32 *const buffer[], void *client_data) {

	struct thread_ctx_s *ctx = (struct thread_ctx_s*) client_data;
	size_t frames = frame->header.blocksize;
	unsigned bits_per_sample = frame->header.bits_per_sample;
	unsigned channels = frame->header.channels;

	FLAC__int32 *lptr = (FLAC__int32 *)buffer[0];
	FLAC__int32 *rptr = (FLAC__int32 *)buffer[channels > 1 ? 1 : 0];

	if (ctx->decode.new_stream) {
		LOCK_O;
		LOG_INFO("[%p]: setting track_start", ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		ctx->decode.new_stream = false;
		// don't use next_sample_rate
		ctx->output.current_sample_rate = decode_newstream(frame->header.sample_rate, ctx->output.supported_rates, ctx);
		if (ctx->output.fade_mode) _checkfade(true, ctx);

		UNLOCK_O;
	}

	LOCK_O_direct;

	while (frames > 0) {
		frames_t f;
		frames_t count;
		s16_t *optr = NULL;

		IF_DIRECT(
			optr = (s16_t *)ctx->outputbuf->writep;
			f = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
		);
		IF_PROCESS(
			optr = (s16_t *)ctx->process.inbuf;
			f = ctx->process.max_in_frames;
		);

		f = min(f, frames);

		count = f;

		if (bits_per_sample == 8) {
			while (count--) {
				*optr++ = *lptr++ << 8;
				*optr++ = *rptr++ << 8;
			}
		} else if (bits_per_sample == 16) {
			while (count--) {
				*optr++ = *lptr++;
				*optr++ = *rptr++;
			}
		} else if (bits_per_sample == 24) {
			while (count--) {
				*optr++ = *lptr++ >> 8;
				*optr++ = *rptr++ >> 8;
			}
		} else if (bits_per_sample == 32) {
			while (count--) {
				*optr++ = *lptr++ >> 16;
				*optr++ = *rptr++ >> 16;
			}
		} else {
			LOG_ERROR("[%p]: unsupported bits per sample: %u", ctx, bits_per_sample);
		}

		frames -= f;

		IF_DIRECT(
			_buf_inc_writep(ctx->outputbuf, f * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			ctx->process.in_frames = f;
			if (frames) LOG_ERROR("[%p]: unhandled case", ctx);
		);
	}

	UNLOCK_O_direct;

	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void error_cb(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status,
					 void *client_data) {
	struct thread_ctx_s *ctx = (struct thread_ctx_s*) client_data;
	LOG_INFO("[%p]: flac error: %s", ctx, FLAC_A(&gf, StreamDecoderErrorStatusString)[status]);
}

static void flac_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct flac *f = ctx->decode.handle;

	if (!f) {
		f = ctx->decode.handle = malloc(sizeof(struct flac));
		f->decoder = NULL;
		f->container = '?';
	}

	if (!f) return;

	if (f->decoder) {
		if (f->container != sample_size ) {
			FLAC(&gf, stream_decoder_delete, f->decoder);
			f->decoder = FLAC(&gf, stream_decoder_new);
		} else {
			FLAC(&gf, stream_decoder_reset, f->decoder);
		}
	} else {
		f->decoder = FLAC(&gf, stream_decoder_new);
	}

	f->container = sample_size;

	if ( f->container == 'o' ) {
		FLAC(&gf, stream_decoder_init_ogg_stream, f->decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, NULL, &error_cb, ctx);
	} else {
		FLAC(&gf, stream_decoder_init_stream, f->decoder, &read_cb, NULL, NULL, NULL, NULL, &write_cb, NULL, &error_cb, ctx);
	}
}

static void flac_close(struct thread_ctx_s *ctx) {
	struct flac *f = ctx->decode.handle;

	FLAC(&gf, stream_decoder_delete, f->decoder);
	free(ctx->decode.handle);
	ctx->decode.handle = NULL;
}

static decode_state flac_decode(struct thread_ctx_s *ctx) {
	struct flac *f = ctx->decode.handle;
	bool ok = FLAC(&gf, stream_decoder_process_single, f->decoder);
	FLAC__StreamDecoderState state = FLAC(&gf, stream_decoder_get_state, f->decoder);

	if (!ok && state != FLAC__STREAM_DECODER_END_OF_STREAM) {
		LOG_INFO("flac error: %s", FLAC_A(&gf, StreamDecoderStateString)[state]);
	};

	if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_COMPLETE;
	} else if (state > FLAC__STREAM_DECODER_END_OF_STREAM) {
		return DECODE_ERROR;
	} else {
		return DECODE_RUNNING;
	}
}

static bool load_flac(void) {
#if !LINKALL
	char *err;

	gf.handle = dlopen(LIBFLAC, RTLD_NOW);

	if (!gf.handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	gf.FLAC__StreamDecoderErrorStatusString = dlsym(gf.handle, "FLAC__StreamDecoderErrorStatusString");
	gf.FLAC__StreamDecoderStateString = dlsym(gf.handle, "FLAC__StreamDecoderStateString");
	gf.FLAC__stream_decoder_new = dlsym(gf.handle, "FLAC__stream_decoder_new");
	gf.FLAC__stream_decoder_reset = dlsym(gf.handle, "FLAC__stream_decoder_reset");
	gf.FLAC__stream_decoder_delete = dlsym(gf.handle, "FLAC__stream_decoder_delete");
	gf.FLAC__stream_decoder_init_stream = dlsym(gf.handle, "FLAC__stream_decoder_init_stream");
	gf.FLAC__stream_decoder_init_ogg_stream = dlsym(gf.handle, "FLAC__stream_decoder_init_ogg_stream");
	gf.FLAC__stream_decoder_process_single = dlsym(gf.handle, "FLAC__stream_decoder_process_single");
	gf.FLAC__stream_decoder_get_state = dlsym(gf.handle, "FLAC__stream_decoder_get_state");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBFLAC, NULL);
#endif

	return true;
}

struct codec *register_flac(void) {
	static struct codec ret = {
		'f',          // id
		"ogf,flc",        // types
		16384,        // min read
		204800,       // min space
		flac_open,    // open
		flac_close,   // close
		flac_decode,  // decode
	};

	if (!load_flac()) {
		return NULL;
	}

	LOG_INFO("using flac to decode flc", NULL);
	return &ret;
}


void deregister_flac(void) {
#if !LINKALL
	if (gf.handle) dlclose(gf.handle);
#endif
}

