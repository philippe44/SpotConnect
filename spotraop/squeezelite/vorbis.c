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

// automatically select between floating point (preferred) and fixed point libraries:
// NOTE: works with Tremor version here: http://svn.xiph.org/trunk/Tremor, not vorbisidec.1.0.2 currently in ubuntu

// we take common definations from <vorbis/vorbisfile.h> even though we can use tremor at run time
// tremor's OggVorbis_File struct is normally smaller so this is ok, but padding added to malloc in case it is bigger
#define OV_EXCLUDE_STATIC_CALLBACKS

#include <vorbis/vorbisfile.h>

#if !LINKALL
static struct {
	void *handle;
	// vorbis symbols to be dynamically loaded - from either vorbisfile or vorbisidec (tremor) version of library
	vorbis_info *(* ov_info)(OggVorbis_File *vf, int link);
	int (* ov_clear)(OggVorbis_File *vf);
	long (* ov_read)(OggVorbis_File *vf, char *buffer, int length, int bigendianp, int word, int sgned, int *bitstream);
	long (* ov_read_tremor)(OggVorbis_File *vf, char *buffer, int length, int *bitstream);
	int (* ov_open_callbacks)(void *datasource, OggVorbis_File *vf, const char *initial, long ibytes, ov_callbacks callbacks);
} gv;
#endif

struct vorbis {
	OggVorbis_File *vf;
	bool opened;
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
#define OV(h, fn, ...) (ov_ ## fn)(__VA_ARGS__)
#define TREMOR(h)      0
#if !WIN
extern int ov_read_tremor(); // needed to enable compilation, not linked
#endif
#else
#define OV(h, fn, ...) (h)->ov_##fn(__VA_ARGS__)
#define TREMOR(h)      (h)->ov_read_tremor
#endif

// called with mutex locked within vorbis_decode to avoid locking O before S
static size_t _read_cb(void *ptr, size_t size, size_t nmemb, void *datasource) {
	size_t bytes;
	struct thread_ctx_s *ctx = datasource;

	while (1) {
		LOCK_S;
		bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
		bytes = min(bytes, size * nmemb);
		if (bytes || ctx->stream.state <= DISCONNECT) break;

		UNLOCK_S;
		usleep(50 * 1000);
	}

	memcpy(ptr, ctx->streambuf->readp, bytes);
	_buf_inc_readp(ctx->streambuf, bytes);
	UNLOCK_S;

	return bytes / size;
}

// these are needed for older versions of tremor, later versions and libvorbis allow NULL to be used
static int _seek_cb(void *datasource, ogg_int64_t offset, int whence) { return -1; }
static int _close_cb(void *datasource) { return 0; }
static long _tell_cb(void *datasource) { return 0; }

static decode_state vorbis_decode( struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;
	frames_t frames;
	int bytes, s, n;
	u8_t *write_buf = NULL;

	if (ctx->decode.new_stream) {
		ov_callbacks cbs;
		int err;
		struct vorbis_info *info;

		cbs.read_func = _read_cb;

		if (TREMOR(&gv)) {
			cbs.seek_func = _seek_cb; cbs.close_func = _close_cb; cbs.tell_func = _tell_cb;
		} else {
			cbs.seek_func = NULL; cbs.close_func = NULL; cbs.tell_func = NULL;
		}

		if ((err = OV(&gv, open_callbacks, ctx, v->vf, NULL, 0, cbs)) < 0) {
			LOG_WARN("[%p]: open_callbacks error: %d", ctx, err);
			return DECODE_COMPLETE;
		}

		info = OV(&gv, info, v->vf, -1);

		v->opened = true;
		v->channels = info->channels;

		LOCK_O;
		// don't use next_sample_rate
		ctx->output.current_sample_rate = decode_newstream(info->rate, ctx->output.supported_rates, ctx);
		ctx->output.track_start = ctx->outputbuf->writep;
		if (ctx->output.fade_mode) _checkfade(true, ctx);
		ctx->decode.new_stream = false;
		UNLOCK_O;

		if (v->channels > 2) {
			LOG_WARN("[%p]: too many channels: %d", ctx, v->channels);
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

	bytes = frames * 2 * v->channels; // samples returned are 16 bits

	// write the 16 bits decoded frames into outputbuf even when they are mono
	if (!TREMOR(&gv)) {
#if SL_LITTLE_ENDIAN
		n = OV(&gv, read, v->vf, (char *)write_buf, bytes, 0, 2, 1, &s);
#else
		n = OV(&gv, read, v->vf, (char *)write_buf, bytes, 1, 2, 1, &s);
#endif
#if !WIN
	} else {
		n = OV(&gv, read_tremor, v->vf, (char *)write_buf, bytes, &s);
#endif
	}

	if (n > 0) {

		frames_t count;
		s16_t *iptr, *optr;

		frames = n / 2 / v->channels;
		count = frames * v->channels;

		// work backward although no unpack is needed
		iptr = (s16_t *)write_buf + count;
		optr = (s16_t *)write_buf + frames * 2;

		if (v->channels == 2) {
			while (count--) {
				*--optr = *--iptr;
			}
		} else if (v->channels == 1) {
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
			LOG_INFO("[%p]: partial decode", ctx);
			UNLOCK_O_direct;
			return DECODE_COMPLETE;
		} else {
			LOG_INFO("[%p]: no frame decoded", ctx);
	    }

	} else if (n == OV_HOLE) {

		// recoverable hole in stream, seen when skipping
		LOG_DEBUG("[%p]: hole in stream", ctx);

	} else {

		LOG_INFO("[%p]: ov_read error: %d", ctx, n);
		UNLOCK_O_direct;
		return DECODE_COMPLETE;
	}

	UNLOCK_O_direct;
	return DECODE_RUNNING;
}

static void vorbis_open(u8_t size, u32_t rate, u8_t chan, u8_t endianness, struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;

	if (!v) {
		v = ctx->decode.handle = malloc(sizeof(struct vorbis));
		if (!v) return;
		v->opened = false;
		v->vf = malloc(sizeof(OggVorbis_File) + 128); // add some padding as struct size may be larger
		if (!v->vf ) {
			free(v);
			return;
		}
		memset(v->vf, 0, sizeof(OggVorbis_File) + 128);
	}

	if (v->opened) {
		OV(&gv, clear, v->vf);
		v->opened = false;
	}
}

static void vorbis_close(struct thread_ctx_s *ctx) {
	struct vorbis *v = ctx->decode.handle;

	if (v->opened) {
		OV(&gv, clear, v->vf);
	}
	free(v->vf);
	free(v);
	ctx->decode.handle = NULL;
}

static bool load_vorbis(void) {
#if !LINKALL
	char *err;
	bool tremor = false;

	gv.handle = dlopen(LIBVORBIS, RTLD_NOW);
	if (!gv.handle) {
		gv.handle = dlopen(LIBTREMOR, RTLD_NOW);
		if (gv.handle) {
			tremor = true;
		} else {
			LOG_INFO("dlerror: %s", dlerror());
			return false;
		}
	}

	gv.ov_read = tremor ? NULL : dlsym(gv.handle, "ov_read");
	gv.ov_read_tremor = tremor ? dlsym(gv.handle, "ov_read") : NULL;
	gv.ov_info = dlsym(gv.handle, "ov_info");
	gv.ov_clear = dlsym(gv.handle, "ov_clear");
	gv.ov_open_callbacks = dlsym(gv.handle, "ov_open_callbacks");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded %s", tremor ? LIBTREMOR : LIBVORBIS);
#endif

	return true;
}

struct codec *register_vorbis(void) {
	static struct codec ret = {
		'o',          // id
		"ogg",        // types
		2048,         // min read
		20480,        // min space
		vorbis_open,  // open
		vorbis_close, // close
		vorbis_decode,// decode
	};

	if (!load_vorbis()) {
		return NULL;
	}

	LOG_INFO("using vorbis to decode ogg", NULL);
	return &ret;
}

void deregister_vorbis(void) {
#if !LINKALL
	if (gv.handle) dlclose(gv.handle);
#endif
}