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

// upsampling using libsoxr - only included if RESAMPLE set

#include "squeezelite.h"

#if RESAMPLE

#include <math.h>
#include <soxr.h>

extern log_level 	decode_loglevel;
static log_level 	*loglevel = &decode_loglevel;

#if !WIN
#undef LINKALL
#define LINKALL 1
#endif

#if !LINKALL
struct  {
	void *handle;
	// soxr symbols to be dynamically loaded
	soxr_io_spec_t (* soxr_io_spec)(soxr_datatype_t itype, soxr_datatype_t otype);
	soxr_quality_spec_t (* soxr_quality_spec)(unsigned long recipe, unsigned long flags);
	soxr_t (* soxr_create)(double, double, unsigned, soxr_error_t *,
						   soxr_io_spec_t const *, soxr_quality_spec_t const *, soxr_runtime_spec_t const *);
	void (* soxr_delete)(soxr_t);
	soxr_error_t (* soxr_process)(soxr_t, soxr_in_t, size_t, size_t *, soxr_out_t, size_t olen, size_t *);
	size_t *(* soxr_num_clips)(soxr_t);
#if RESAMPLE_MP
	soxr_runtime_spec_t (* soxr_runtime_spec)(unsigned num_threads);
#endif
	// soxr_strerror is a macro so not included here
} gr;
#endif

struct soxr {
	soxr_t resampler;
	size_t old_clips;
	unsigned long q_recipe;
	unsigned long q_flags;
	double q_precision;         /* Conversion precision (in bits).           20    */
	double q_phase_response;    /* 0=minimum, ... 50=linear, ... 100=maximum 50    */
	double q_passband_end;      /* 0dB pt. bandwidth to preserve; nyquist=1  0.913 */
	double q_stopband_begin;    /* Aliasing/imaging control; > passband_end   1    */
	double scale;
	bool max_rate;
	bool exception;
};


#if LINKALL
#define SOXR(h, fn, ...) (soxr_ ## fn)(__VA_ARGS__)
#else
#define SOXR(h, fn, ...) (h)->soxr_##fn(__VA_ARGS__)
#endif


void resample_samples(struct thread_ctx_s *ctx) {
	struct soxr *r = ctx->decode.process_handle;
	size_t idone, odone;
	size_t clip_cnt;

	soxr_error_t error =
		SOXR(&gr, process, r->resampler, ctx->process.inbuf, ctx->process.in_frames, &idone, ctx->process.outbuf, ctx->process.max_out_frames, &odone);
	if (error) {
		LOG_INFO("[%p]: soxr_process error: %s", ctx, soxr_strerror(error));
		return;
	}

	if (idone != ctx->process.in_frames) {
		// should not get here if buffers are big enough...
		LOG_ERROR("[%p]: should not get here - partial sox process: %u of %u processed %u of %u out",
				  ctx, (unsigned)idone, ctx->process.in_frames, (unsigned)odone, ctx->process.max_out_frames);
	}

	ctx->process.out_frames = odone;
	ctx->process.total_in  += idone;
	ctx->process.total_out += odone;

	clip_cnt = *(SOXR(&gr, num_clips, r->resampler));
	if (clip_cnt - r->old_clips) {
		LOG_SDEBUG("[%p]: resampling clips: %u", ctx, (unsigned)(clip_cnt - r->old_clips));
		r->old_clips = clip_cnt;
	}
}

bool resample_drain(struct thread_ctx_s *ctx) {
	struct soxr *r = ctx->decode.process_handle;
	size_t odone;
	size_t clip_cnt;

	soxr_error_t error = SOXR(&gr, process, r->resampler, NULL, 0, NULL, ctx->process.outbuf, ctx->process.max_out_frames, &odone);
	if (error) {
		LOG_INFO("[%p]: soxr_process error: %s", ctx, soxr_strerror(error));
		return true;
	}

	ctx->process.out_frames = odone;
	ctx->process.total_out += odone;

	clip_cnt = *(SOXR(&gr, num_clips, r->resampler));
	if (clip_cnt - r->old_clips) {
		LOG_DEBUG("[%p]: resampling clips: %u", ctx, (unsigned)(clip_cnt - r->old_clips));
		r->old_clips = clip_cnt;
	}

	if (odone == 0) {

		LOG_INFO("[%p]: resample track complete - total track clips: %u", ctx, r->old_clips);

		SOXR(&gr, delete, r->resampler);
		r->resampler = NULL;

		return true;

	} else {

		return false;
	}
}

bool resample_newstream(unsigned raw_sample_rate, int supported_rates[], struct thread_ctx_s *ctx) {
	struct soxr *r = ctx->decode.process_handle;
	unsigned outrate = 0;
	int i = 0;

	if (r->exception) {
		// find direct match - avoid resampling
		for (i = 0; supported_rates[i]; i++) {
			if (raw_sample_rate == supported_rates[i]) {
				outrate = raw_sample_rate;
				break;
			}
		}
		// else find next highest sync sample rate
		while (!outrate && i >= 0) {
			if (supported_rates[i] > raw_sample_rate && supported_rates[i] % raw_sample_rate == 0) {
				outrate = supported_rates[i];
				break;
			}
			i--;
		}
	}

	if (!outrate) {
		if (r->max_rate) {
			// resample to max rate for device
			outrate = supported_rates[0];
		} else {
			// resample to max sync sample rate
			for (i = 0; supported_rates[i]; i++) {
				if (supported_rates[i] % raw_sample_rate == 0 || raw_sample_rate % supported_rates[i] == 0) {
					outrate = supported_rates[i];
					break;
				}
			}
		}
		if (!outrate) {
			outrate = supported_rates[0];
		}
	}

	ctx->process.in_sample_rate = raw_sample_rate;
	ctx->process.out_sample_rate = outrate;

	if (r->resampler) {
		SOXR(&gr, delete, r->resampler);
		r->resampler = NULL;
	}

	if (raw_sample_rate != outrate) {

		soxr_io_spec_t io_spec;
		soxr_quality_spec_t q_spec;
		soxr_error_t error;
#if RESAMPLE_MP
		soxr_runtime_spec_t r_spec;
#endif

		LOG_INFO("[%p]: resampling from %u -> %u", ctx, raw_sample_rate, outrate);

		io_spec = SOXR(&gr, io_spec, SOXR_INT16_I, SOXR_INT16_I);
		io_spec.scale = r->scale;

		q_spec = SOXR(&gr, quality_spec, r->q_recipe, r->q_flags);
		if (r->q_precision > 0) {
			q_spec.precision = r->q_precision;
		}
		if (r->q_passband_end > 0) {
			q_spec.passband_end = r->q_passband_end;
		}
		if (r->q_stopband_begin > 0) {
			q_spec.stopband_begin = r->q_stopband_begin;
		}
		if (r->q_phase_response > -1) {
			q_spec.phase_response = r->q_phase_response;
		}

#if RESAMPLE_MP
		r_spec = SOXR(&gr, runtime_spec, 0); // make use of libsoxr OpenMP support allowing parallel execution if multiple cores
#endif

		LOG_DEBUG("[%p]: resampling with soxr_quality_spec_t[precision: %03.1f, passband_end: %03.6f, stopband_begin: %03.6f, "
				  "phase_response: %03.1f, flags: 0x%02x], soxr_io_spec_t[scale: %03.2f]", ctx, q_spec.precision,
				  q_spec.passband_end, q_spec.stopband_begin, q_spec.phase_response, q_spec.flags, io_spec.scale);

#if RESAMPLE_MP
		r->resampler = SOXR(&gr, create, raw_sample_rate, outrate, 2, &error, &io_spec, &q_spec, &r_spec);
#else
		r->resampler = SOXR(&gr, create, raw_sample_rate, outrate, 2, &error, &io_spec, &q_spec, NULL);
#endif

		if (error) {
			LOG_INFO("[%p]: soxr_create error: %s", ctx, soxr_strerror(error));
			return false;
		}

		r->old_clips = 0;
		return true;

	} else {

		LOG_INFO("[%p]: disable resampling - rates match %u", ctx, outrate);
		return false;
	}
}

void resample_flush(struct thread_ctx_s *ctx) {
	struct soxr *r = ctx->decode.process_handle;

	if (r->resampler) {
		SOXR(&gr, delete, r->resampler);
		r->resampler = NULL;
	}
}


bool resample_init(char *opt, struct thread_ctx_s *ctx) {
	struct soxr *r;
	char *recipe = NULL, *flags = NULL;
	char *atten = NULL;
	char *precision = NULL, *passband_end = NULL, *stopband_begin = NULL, *phase_response = NULL;

#if !LINKALL
	if (!gr.handle) return false;
#endif

	r = ctx->decode.process_handle = malloc(sizeof(struct soxr));
	if (!r) {
		LOG_WARN("[%p]: resampling disabled", ctx);
		return false;
	}

	r->resampler = NULL;
	r->old_clips = 0;
	// do not try to go max_rate
	r->max_rate = false;
	// do not rsample if matching !
	r->exception = true;

	if (opt) {
		recipe = next_param(opt, ':');
		flags = next_param(NULL, ':');
		atten = next_param(NULL, ':');
		precision = next_param(NULL, ':');
		passband_end = next_param(NULL, ':');
		stopband_begin = next_param(NULL, ':');
		phase_response = next_param(NULL, ':');
	}

	// default to QQ (16 bit) if not user specified
	r->q_recipe = SOXR_QQ;
	r->q_flags = 0;
	// default to 1db of attenuation if not user specified
	r->scale = pow(10, -1.0 / 20);
	// override recipe derived values with user specified values
	r->q_precision = 0;
	r->q_passband_end = 0;
	r->q_stopband_begin = 0;
	r->q_phase_response = -1;

	if (recipe && recipe[0] != '\0') {
		if (strchr(recipe, 'm')) r->q_recipe = SOXR_MQ;
		if (strchr(recipe, 'l')) r->q_recipe = SOXR_LQ;
		if (strchr(recipe, 'q')) r->q_recipe = SOXR_QQ;
		if (strchr(recipe, 'L')) r->q_recipe |= SOXR_LINEAR_PHASE;
		if (strchr(recipe, 'I')) r->q_recipe |= SOXR_INTERMEDIATE_PHASE;
		if (strchr(recipe, 'M')) r->q_recipe |= SOXR_MINIMUM_PHASE;
		if (strchr(recipe, 's')) r->q_recipe |= SOXR_STEEP_FILTER;
	}

	if (flags) {
		r->q_flags = strtoul(flags, 0, 16);
	}

	if (atten) {
		double scale = pow(10, -atof(atten) / 20);
		if (scale > 0 && scale <= 1.0) {
			r->scale = scale;
		}
	}

	if (precision) {
		r->q_precision = atof(precision);
	}

	if (passband_end) {
		r->q_passband_end = atof(passband_end) / 100;
	}

	if (stopband_begin) {
		r->q_stopband_begin = atof(stopband_begin) / 100;
	}

	if (phase_response) {
		r->q_phase_response = atof(phase_response);
	}

	LOG_INFO("[%p]: resampling %s recipe: 0x%02x, flags: 0x%02x, scale: %03.2f, precision: %03.1f, passband_end: %03.5f, stopband_begin: %03.5f, phase_response: %03.1f",
			ctx, r->max_rate ? "async" : "sync",
			r->q_recipe, r->q_flags, r->scale, r->q_precision, r->q_passband_end, r->q_stopband_begin, r->q_phase_response);

	return true;
}


void resample_end(struct thread_ctx_s *ctx) {
	if (ctx->decode.process_handle) free(ctx->decode.process_handle);
}


static bool load_soxr(void) {
#if !LINKALL
	char *err;

	gr.handle = dlopen(LIBSOXR, RTLD_NOW);
	if (!gr.handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	gr.soxr_io_spec = dlsym(gr.handle, "soxr_io_spec");
	gr.soxr_quality_spec = dlsym(gr.handle, "soxr_quality_spec");
	gr.soxr_create = dlsym(gr.handle, "soxr_create");
	gr.soxr_delete = dlsym(gr.handle, "soxr_delete");
	gr.soxr_process = dlsym(gr.handle, "soxr_process");
	gr.soxr_num_clips = dlsym(gr.handle, "soxr_num_clips");
#if RESAMPLE_MP
	gr.soxr_runtime_spec = dlsym(gr.handle, "soxr_runtime_spec");
#endif

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBSOXR, NULL);
#endif

	return true;
}


bool register_soxr(void) {
	if (!load_soxr()) {
		LOG_WARN("resampling disabled", NULL);
		return false;
	}

	LOG_INFO("using soxr for resampling", NULL);
	return true;
}

void deregister_soxr(void) {
#if !LINKALL
	dlclose(gr.handle);
#endif
}


#endif // #if RESAMPLE
