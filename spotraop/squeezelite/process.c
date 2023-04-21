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

// sample processing - only included when building with PROCESS set

#include "squeezelite.h"

#if PROCESS

extern log_level 	decode_loglevel;
static log_level 	*loglevel = &decode_loglevel;

#define LOCK_D   mutex_lock(ctx->decode.mutex);
#define UNLOCK_D mutex_unlock(ctx->decode.mutex);
#define LOCK_O   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK_O mutex_unlock(ctx->outputbuf->mutex)

// macros to map to processing functions - currently only resample.c
// this can be made more generic when multiple processing mechanisms get added
#if RESAMPLE
#define SAMPLES_FUNC resample_samples
#define DRAIN_FUNC   resample_drain
#define NEWSTREAM_FUNC resample_newstream
#define FLUSH_FUNC   resample_flush
#define INIT_FUNC    resample_init
#define END_FUNC    resample_end
#endif


// transfer all processed frames to the output buf
static void _write_samples(struct thread_ctx_s *ctx) {
	size_t frames = ctx->process.out_frames;
	u16_t *iptr   = (u16_t *) ctx->process.outbuf;
	unsigned cnt  = 10;

	LOCK_O;

	while (frames > 0) {

		frames_t f = min(_buf_space(ctx->outputbuf), _buf_cont_write(ctx->outputbuf)) / BYTES_PER_FRAME;
		u16_t *optr = (u16_t *)ctx->outputbuf->writep;

		if (f > 0) {

			f = min(f, frames);

			memcpy(optr, iptr, f * BYTES_PER_FRAME);

			frames -= f;

			_buf_inc_writep(ctx->outputbuf, f * BYTES_PER_FRAME);
			iptr += f * BYTES_PER_FRAME / sizeof(*iptr);

		} else if (cnt--) {

			// there should normally be space in the output buffer, but may need to wait during drain phase
			UNLOCK_O;
			usleep(10000);
			LOCK_O;

		} else {

			// bail out if no space found after 100ms to avoid locking
			LOG_ERROR("[%p]: unable to get space in output buffer", ctx);
			UNLOCK_O;
			return;
		}
	}

	UNLOCK_O;
}

// process samples - called with decode mutex set
void process_samples(struct thread_ctx_s *ctx) {

	SAMPLES_FUNC(ctx);

	_write_samples(ctx);

	ctx->process.in_frames = 0;
}

// drain at end of track - called with decode mutex set
void process_drain(struct thread_ctx_s *ctx) {
	bool done;

	do {

		done = DRAIN_FUNC(ctx);

		_write_samples(ctx);

	} while (!done);

	LOG_DEBUG("[%p]: processing track complete - frames in: %lu out: %lu", ctx, ctx->process.total_in, ctx->process.total_out);
}

// new stream - called with decode mutex set
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, int supported_rates[], struct thread_ctx_s *ctx) {

	bool active = NEWSTREAM_FUNC(raw_sample_rate, supported_rates, ctx);

	LOG_INFO("[%p]: processing: %s", ctx, active ? "active" : "inactive");

	*direct = !active;

	if (active) {

		unsigned max_in_frames, max_out_frames;

		ctx->process.in_frames = ctx->process.out_frames = 0;
		ctx->process.total_in = ctx->process.total_out = 0;

		max_in_frames = ctx->codec->min_space / BYTES_PER_FRAME ;

		// increase size of output buffer by 10% as output rate is not an exact multiple of input rate
		if (ctx->process.out_sample_rate % ctx->process.in_sample_rate == 0) {
			max_out_frames = max_in_frames * (ctx->process.out_sample_rate / ctx->process.in_sample_rate);
		} else {
			max_out_frames = (int)(1.1 * (float)max_in_frames * (float)ctx->process.out_sample_rate / (float)ctx->process.in_sample_rate);
		}

		if (ctx->process.max_in_frames != max_in_frames) {
			LOG_DEBUG("[%p]: creating process buf in frames: %u", ctx, max_in_frames);
			if (ctx->process.inbuf) free(ctx->process.inbuf);
			ctx->process.inbuf = malloc(max_in_frames * BYTES_PER_FRAME);
			ctx->process.max_in_frames = max_in_frames;
		}

		if (ctx->process.max_out_frames != max_out_frames) {
			LOG_DEBUG("[%p]: creating process buf out frames: %u", ctx, max_out_frames);
			if (ctx->process.outbuf) free(ctx->process.outbuf);
			ctx->process.outbuf = malloc(max_out_frames * BYTES_PER_FRAME);
			ctx->process.max_out_frames = max_out_frames;
		}

		if (!ctx->process.inbuf || !ctx->process.outbuf) {
			LOG_ERROR("[%p]: malloc fail creating process buffers", ctx);
			*direct = true;
			return raw_sample_rate;
		}

		return ctx->process.out_sample_rate;
	}

	return raw_sample_rate;
}

// process flush - called with decode mutex set
void process_flush(struct thread_ctx_s *ctx) {

	LOG_INFO("[%p]: process flush", ctx);

	FLUSH_FUNC(ctx);

	ctx->process.in_frames = 0;
}

// init - called with no mutex
void process_init(char *opt, struct thread_ctx_s *ctx) {

	bool enabled = INIT_FUNC(opt, ctx);

	memset(&ctx->process, 0, sizeof(ctx->process));

	if (enabled) {
		LOCK_D;
		ctx->decode.process = true;
		UNLOCK_D;
	}
}

void process_end(struct thread_ctx_s *ctx) {

	END_FUNC(ctx);

	LOCK_D;
	ctx->decode.process = false;
	if (ctx->process.inbuf) free(ctx->process.inbuf);
	if (ctx->process.outbuf) free(ctx->process.outbuf);
	UNLOCK_D;
}

#endif // #if PROCESS
