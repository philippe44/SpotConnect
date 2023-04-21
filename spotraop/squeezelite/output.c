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

// Common output function

#include "squeezelite.h"

extern log_level	output_loglevel;
static log_level 	*loglevel = &output_loglevel;

#define LOCK   mutex_lock(ctx->outputbuf->mutex)
#define UNLOCK mutex_unlock(ctx->outputbuf->mutex)

// functions starting _* are called with mutex locked


/*---------------------------------------------------------------------------*/
frames_t _output_frames(frames_t avail, struct thread_ctx_s *ctx) {

	frames_t frames, size;
	bool silence;

	s32_t cross_gain_in = 0, cross_gain_out = 0;
	s16_t *cross_ptr = NULL;

	s32_t gainL = ctx->output.current_replay_gain ? gain32(ctx->output.gainL, ctx->output.current_replay_gain) : ctx->output.gainL;
	s32_t gainR = ctx->output.current_replay_gain ? gain32(ctx->output.gainR, ctx->output.current_replay_gain) : ctx->output.gainR;

	frames = _buf_used(ctx->outputbuf) / BYTES_PER_FRAME;
	silence = false;

	// start when threshold met
	if (ctx->output.state == OUTPUT_BUFFER && (frames * BYTES_PER_FRAME) > ctx->output.threshold * ctx->output.current_sample_rate / 10 && frames > ctx->output.start_frames) {
		ctx->output.state = OUTPUT_RUNNING;
		LOG_INFO("[%p]: start buffer frames: %u", ctx, frames);
		wake_controller(ctx);
	}

	// skip ahead - consume outputbuf but play nothing
	if (ctx->output.state == OUTPUT_SKIP_FRAMES) {
		if (frames > 0) {
			frames_t skip = min(frames, ctx->output.skip_frames);
			LOG_INFO("[%p]: skip %u of %u frames", ctx, skip, ctx->output.skip_frames);
			frames -= skip;
			ctx->output.frames_played += skip;
			while (skip > 0) {
				frames_t cont_frames = min(skip, _buf_cont_read(ctx->outputbuf) / BYTES_PER_FRAME);
				skip -= cont_frames;
				_buf_inc_readp(ctx->outputbuf, cont_frames * BYTES_PER_FRAME);
			}
		}
		else {
   			LOG_INFO("[%p]: skip but no frames avail", ctx);
        }
		ctx->output.state = OUTPUT_RUNNING;
	}

	// pause frames - play silence for required frames
	if (ctx->output.state == OUTPUT_PAUSE_FRAMES) {
		LOG_INFO("[%p]: pause %u frames", ctx, ctx->output.pause_frames);
		if (ctx->output.pause_frames == 0) {
			ctx->output.state = OUTPUT_RUNNING;
		} else {
			silence = true;
			frames = min(avail, ctx->output.pause_frames);
			frames = min(frames, MAX_SILENCE_FRAMES);
			ctx->output.pause_frames -= frames;
		}
	}

	LOG_SDEBUG("[%p]: avail: %d frames: %d silence: %d", ctx, avail, frames, silence);
	frames = min(frames, avail);
	size = frames;

	while (size > 0) {
		frames_t out_frames;
		frames_t cont_frames = _buf_cont_read(ctx->outputbuf) / BYTES_PER_FRAME;
		int wrote;

		if (ctx->output.track_start && !silence) {
			if (ctx->output.track_start == ctx->outputbuf->readp) {
				LOG_INFO("[%p]: track start sample rate: %u replay_gain: %u", ctx, ctx->output.current_sample_rate, ctx->output.next_replay_gain);
				ctx->output.frames_played = 0;
				ctx->output.track_started = true;
				ctx->output.detect_start_time = true;
				if (ctx->output.fade == FADE_INACTIVE || ctx->output.fade_mode != FADE_CROSSFADE) {
					ctx->output.current_replay_gain = ctx->output.next_replay_gain;
				}
				ctx->output.track_start = NULL;
				break;
			} else if (ctx->output.track_start > ctx->outputbuf->readp) {
				// reduce cont_frames so we find the next track start at beginning of next chunk
				cont_frames = min(cont_frames, (ctx->output.track_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
			}
		}

		if (ctx->output.fade && !silence) {
			if (ctx->output.fade == FADE_DUE) {
				if (ctx->output.fade_start == ctx->outputbuf->readp) {
					LOG_INFO("[%p]: fade start reached", ctx);
					ctx->output.fade = FADE_ACTIVE;
				} else if (ctx->output.fade_start > ctx->outputbuf->readp) {
					cont_frames = min(cont_frames, (ctx->output.fade_start - ctx->outputbuf->readp) / BYTES_PER_FRAME);
				}
			}
			if (ctx->output.fade == FADE_ACTIVE) {
				// find position within fade
				frames_t cur_f = ctx->outputbuf->readp >= ctx->output.fade_start ? (ctx->outputbuf->readp - ctx->output.fade_start) / BYTES_PER_FRAME :
					(ctx->outputbuf->readp + ctx->outputbuf->size - ctx->output.fade_start) / BYTES_PER_FRAME;
				frames_t dur_f = ctx->output.fade_end >= ctx->output.fade_start ? (ctx->output.fade_end - ctx->output.fade_start) / BYTES_PER_FRAME :
					(ctx->output.fade_end + ctx->outputbuf->size - ctx->output.fade_start) / BYTES_PER_FRAME;
				if (cur_f >= dur_f) {
					if (ctx->output.fade_mode == FADE_INOUT && ctx->output.fade_dir == FADE_DOWN) {
						LOG_INFO("[%p]: fade down complete, starting fade up", ctx);
						ctx->output.fade_dir = FADE_UP;
						ctx->output.fade_start = ctx->outputbuf->readp;
						ctx->output.fade_end = ctx->outputbuf->readp + dur_f * BYTES_PER_FRAME;
						if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
							ctx->output.fade_end -= ctx->outputbuf->size;
						}
						cur_f = 0;
					} else if (ctx->output.fade_mode == FADE_CROSSFADE) {
						LOG_INFO("[%p]: crossfade complete", ctx);
						if (_buf_used(ctx->outputbuf) >= dur_f * BYTES_PER_FRAME) {
							_buf_inc_readp(ctx->outputbuf, dur_f * BYTES_PER_FRAME);
							LOG_INFO("[%p]: skipped crossfaded start", ctx);
						} else {
							LOG_WARN("[%p]: unable to skip crossfaded start", ctx);
						}
						ctx->output.fade = FADE_INACTIVE;
						ctx->output.current_replay_gain = ctx->output.next_replay_gain;
					} else {
						LOG_INFO("[%p]: fade complete", ctx);
						ctx->output.fade = FADE_INACTIVE;
					}
				}
				// if fade in progress set fade gain, ensure cont_frames reduced so we get to end of fade at start of chunk
				if (ctx->output.fade) {
					if (ctx->output.fade_end > ctx->outputbuf->readp) {
						cont_frames = min(cont_frames, (ctx->output.fade_end - ctx->outputbuf->readp) / BYTES_PER_FRAME);
					}
					if (ctx->output.fade_dir == FADE_UP || ctx->output.fade_dir == FADE_DOWN) {
						// fade in, in-out, out handled via altering standard gain
						s32_t fade_gain;
						if (ctx->output.fade_dir == FADE_DOWN) {
							cur_f = dur_f - cur_f;
						}
						fade_gain = to_gain((float)cur_f / (float)dur_f);
						gainL = gain32(gainL, fade_gain);
						gainR = gain32(gainR, fade_gain);
					}
					if (ctx->output.fade_dir == FADE_CROSS) {
						// cross fade requires special treatment - performed later based on these values
						// support different replay gain for old and new track by retaining old value until crossfade completes
						if (_buf_used(ctx->outputbuf) / BYTES_PER_FRAME > dur_f + size) {
							cross_gain_in  = to_gain((float)cur_f / (float)dur_f);
							cross_gain_out = FIXED_ONE - cross_gain_in;
							if (ctx->output.current_replay_gain) {
								cross_gain_out = gain32(cross_gain_out, ctx->output.current_replay_gain);
							}
							if (ctx->output.next_replay_gain) {
								cross_gain_in = gain32(cross_gain_in, ctx->output.next_replay_gain);
							}
							gainL = ctx->output.gainL;
							gainR = ctx->output.gainR;
							cross_ptr = (s16_t *)(ctx->output.fade_end + cur_f * BYTES_PER_FRAME);
						} else {
							LOG_INFO("[%p]: unable to continue crossfade - too few samples", ctx);
							ctx->output.fade = FADE_INACTIVE;
						}
					}
				}
			}
		}

		out_frames = !silence ? min(size, cont_frames) : size;

		wrote = ctx->output.write_cb(ctx, out_frames, silence, gainL, gainR, ctx->output.channels, cross_gain_in, cross_gain_out, &cross_ptr);

		if (wrote <= 0) {
			frames -= size;
			break;
		} else {
			out_frames = (frames_t)wrote;
		}

		size -= out_frames;

		if (!silence) {
			_buf_inc_readp(ctx->outputbuf, out_frames * BYTES_PER_FRAME);
			ctx->output.frames_played += out_frames;
		}
	}

	LOG_SDEBUG("[%p]: wrote %u frames", ctx, frames);

	return frames;
}


/*---------------------------------------------------------------------------*/
void _checkfade(bool start, struct thread_ctx_s *ctx) {
	frames_t bytes;

	LOG_INFO("[%p]: fade mode: %u duration: %u %s", ctx, ctx->output.fade_mode, ctx->output.fade_secs, start ? "track-start" : "track-end");

	bytes = ctx->output.current_sample_rate * BYTES_PER_FRAME * ctx->output.fade_secs;
	if (ctx->output.fade_mode == FADE_INOUT) {
		bytes = ((bytes / 2) / BYTES_PER_FRAME) * BYTES_PER_FRAME;
	}

	if (start && (ctx->output.fade_mode == FADE_IN || (ctx->output.fade_mode == FADE_INOUT && _buf_used(ctx->outputbuf) == 0))) {
		bytes = min(bytes, ctx->outputbuf->size - BYTES_PER_FRAME); // shorter than full buffer otherwise start and end align
		LOG_INFO("[%p]: fade IN: %u frames", ctx, bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_UP;
		ctx->output.fade_start = ctx->outputbuf->writep;
		ctx->output.fade_end = ctx->output.fade_start + bytes;
		if (ctx->output.fade_end >= ctx->outputbuf->wrap) {
			ctx->output.fade_end -= ctx->outputbuf->size;
		}
	}

	if (!start && (ctx->output.fade_mode == FADE_OUT || ctx->output.fade_mode == FADE_INOUT)) {
		bytes = min(_buf_used(ctx->outputbuf), bytes);
		LOG_INFO("[%p]: fade %s: %u frames", ctx, ctx->output.fade_mode == FADE_INOUT ? "IN-OUT" : "OUT", bytes / BYTES_PER_FRAME);
		ctx->output.fade = FADE_DUE;
		ctx->output.fade_dir = FADE_DOWN;
		ctx->output.fade_start = ctx->outputbuf->writep - bytes;
		if (ctx->output.fade_start < ctx->outputbuf->buf) {
			ctx->output.fade_start += ctx->outputbuf->size;
		}
		ctx->output.fade_end = ctx->outputbuf->writep;
	}

	if (start && ctx->output.fade_mode == FADE_CROSSFADE) {
		if (_buf_used(ctx->outputbuf) != 0) {
			bytes = min(bytes, _buf_used(ctx->outputbuf));               // max of current remaining samples from previous track
			bytes = min(bytes, (frames_t)(ctx->outputbuf->size * 0.9));  // max of 90% of outputbuf as we consume additional buffer during crossfade
			LOG_INFO("[%p]: CROSSFADE: %u frames", ctx, bytes / BYTES_PER_FRAME);
			ctx->output.fade = FADE_DUE;
			ctx->output.fade_dir = FADE_CROSS;
			ctx->output.fade_start = ctx->outputbuf->writep - bytes;
			if (ctx->output.fade_start < ctx->outputbuf->buf) {
				ctx->output.fade_start += ctx->outputbuf->size;
			}
			ctx->output.fade_end = ctx->outputbuf->writep;
			ctx->output.track_start = ctx->output.fade_start;
		}
#if 0
		//FIXME : don't understand well what's attempted here
		else if (ctx->outputbuf->size == OUTPUTBUF_SIZE && ctx->outputbuf->readp == ctx->outputbuf->buf) {
			// if default setting used and nothing in buffer attempt to resize to provide full crossfade support
			LOG_INFO("[%p]: resize outputbuf for crossfade", ctx);
			_buf_resize(ctx->outputbuf, OUTPUTBUF_SIZE_CROSSFADE);
#if LINUX || FREEBSD || SUNOS
			touch_memory(ctx->outputbuf->buf, ctx->outputbuf->size);
#endif
		}
#endif
	}
}


/*---------------------------------------------------------------------------*/
void output_init_common(void *device, unsigned outputbuf_size, u32_t sample_rate, struct thread_ctx_s *ctx) {
	outputbuf_size = outputbuf_size - (outputbuf_size % BYTES_PER_FRAME);
	LOG_DEBUG("[%p]: outputbuf size: %u", outputbuf_size);

	ctx->outputbuf = &ctx->__o_buf;

	LOG_ERROR("allocating %d", outputbuf_size);
	buf_init(ctx->outputbuf, outputbuf_size);
	if (!ctx->outputbuf->buf) {
		LOG_ERROR("[%p]: unable to malloc output buffer", ctx);
		exit(0);
	}

	ctx->silencebuf = malloc(MAX_SILENCE_FRAMES * BYTES_PER_FRAME);
	if (!ctx->silencebuf) {
		LOG_ERROR("[%p]: unable to malloc silence buffer", ctx);
		exit(0);
	}
	memset(ctx->silencebuf, 0, MAX_SILENCE_FRAMES * BYTES_PER_FRAME);

	ctx->output.state = OUTPUT_STOPPED;
	ctx->output.fade = FADE_INACTIVE;
	ctx->output.device = device;
	ctx->output.error_opening = false;
	ctx->output.detect_start_time = false;

	ctx->output.current_sample_rate = ctx->output.default_sample_rate = sample_rate;
	ctx->output.supported_rates[0] = sample_rate;
	ctx->output.supported_rates[1] = 0;
}


/*---------------------------------------------------------------------------*/
void output_close_common(struct thread_ctx_s *ctx) {
	LOCK;
	ctx->output_running = false;
	UNLOCK;

	pthread_join(ctx->output_thread, NULL);

	buf_destroy(ctx->outputbuf);
	free(ctx->silencebuf);
}


/*---------------------------------------------------------------------------*/
void output_flush(struct thread_ctx_s *ctx) {
	LOG_DEBUG("[%p]: flush output buffer", ctx);
	buf_flush(ctx->outputbuf);
	LOCK;
	ctx->output.fade = FADE_INACTIVE;
	if (ctx->output.state != OUTPUT_OFF) {
		ctx->output.state = OUTPUT_STOPPED;
		if (ctx->output.error_opening) {
			ctx->output.current_sample_rate = ctx->output.default_sample_rate;
		}
		ctx->output.delay_active = false;
	}
	ctx->output.frames_played = ctx->output.frames_played_dmp = 0;
	ctx->output.track_start_time = -1;
	UNLOCK;
}



