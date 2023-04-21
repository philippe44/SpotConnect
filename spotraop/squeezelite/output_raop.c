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

// raop output

#include "squeezelite.h"
#include "raop_client.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK mutex_unlock(ctx->outputbuf->mutex)
#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)


/*---------------------------------------------------------------------------*/
void wake_output(struct thread_ctx_s *ctx) {
	return;
}


/*---------------------------------------------------------------------------*/
static int _raop_write_frames(struct thread_ctx_s *ctx, frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
								s32_t cross_gain_in, s32_t cross_gain_out, s16_t **cross_ptr) {

	s16_t *obuf;

	if (!silence) {

		if (ctx->output.fade == FADE_ACTIVE && ctx->output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(ctx->outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}

		obuf = (s16_t*) ctx->outputbuf->readp;

	} else {

		obuf = (s16_t*) ctx->silencebuf;
	}

	_scale_frames((s16_t*) (ctx->output.buf + ctx->output.buf_frames * BYTES_PER_FRAME), obuf, out_frames, gainL, gainR, flags);

	ctx->output.buf_frames += out_frames;

	return (int) out_frames;
}


/*---------------------------------------------------------------------------*/
void output_close(struct thread_ctx_s *ctx)
{
	output_close_common(ctx);
	free(ctx->output.buf);
}


/*---------------------------------------------------------------------------*/
static void *output_raop_thread(struct thread_ctx_s *ctx) {
	while (ctx->output_running) {
		bool ran = false;

		// proceed only if player can accept audio *and* running
		if (ctx->output.state >= OUTPUT_BUFFER && raopcl_accept_frames(ctx->output.device)) {
			u64_t playtime;

			LOCK;
			// this will internally loop till we have exactly 352 frames
			_output_frames(FRAMES_PER_BLOCK, ctx);
			UNLOCK;

			if (ctx->output.buf_frames) {
				raopcl_send_chunk(ctx->output.device, ctx->output.buf, ctx->output.buf_frames, &playtime);

				// current block is a track start, set the value
				if (ctx->output.detect_start_time) {
					ctx->output.detect_start_time = false;
					ctx->output.track_start_time = NTP2MS(playtime);
					LOG_INFO("[%p]: track actual start time:%u (gap:%d)", ctx, ctx->output.track_start_time,
										(s32_t) (ctx->output.track_start_time - ctx->output.start_at));
				}

				ctx->output.buf_frames = 0;
				ran = true;
			}
		}

		LOCK;
		ctx->output.updated = gettime_ms();
		// TODO: in some cases, we have less frames than latency (beginning & end)
		ctx->output.device_frames = raopcl_latency(ctx->output.device);
		ctx->output.frames_played_dmp = ctx->output.frames_played;
		UNLOCK;

		// nothing to do, sleep
		if (!ran) usleep(10000);
	}

	return 0;
}


/*---------------------------------------------------------------------------*/
bool output_raop_thread_init(struct raopcl_s *raopcl, unsigned outputbuf_size, struct thread_ctx_s *ctx) {
	pthread_attr_t attr;

	LOG_INFO("[%p]: init output raop", ctx);

	memset(&ctx->output, 0, sizeof(ctx->output));

	ctx->output.buf = malloc(FRAMES_PER_BLOCK * BYTES_PER_FRAME);
	if (!ctx->output.buf) {
		LOG_ERROR("[%p]: unable to malloc buf", ctx);
		return false;
	}

	ctx->output_running = true;
	ctx->output.buf_frames = 0;
	ctx->output.start_frames = FRAMES_PER_BLOCK * 2;
	ctx->output.write_cb = &_raop_write_frames;

	output_init_common(raopcl, outputbuf_size, raopcl_sample_rate(raopcl), ctx);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + OUTPUT_THREAD_STACK_SIZE);
	pthread_create(&ctx->output_thread, &attr, (void *(*)(void*)) &output_raop_thread, ctx);
	pthread_attr_destroy(&attr);

	return true;
}




