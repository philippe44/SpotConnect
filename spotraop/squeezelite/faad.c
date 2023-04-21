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

#include "squeezelite.h"

#include <neaacdec.h>

#define WRAPBUF_LEN 2048

struct chunk_table {
	u32_t sample, offset;
};

#if !LINKALL
struct {
	void *handle;
	NeAACDecConfigurationPtr (* NeAACDecGetCurrentConfiguration)(NeAACDecHandle);
	unsigned char (* NeAACDecSetConfiguration)(NeAACDecHandle, NeAACDecConfigurationPtr);
	NeAACDecHandle (* NeAACDecOpen)(void);
	void (* NeAACDecClose)(NeAACDecHandle);
	long (* NeAACDecInit)(NeAACDecHandle, unsigned char *, unsigned long, unsigned long *, unsigned char *);
	char (* NeAACDecInit2)(NeAACDecHandle, unsigned char *pBuffer, unsigned long, unsigned long *, unsigned char *);
	void *(* NeAACDecDecode)(NeAACDecHandle, NeAACDecFrameInfo *, unsigned char *, unsigned long);
	char *(* NeAACDecGetErrorMessage)(unsigned char);
} ga;
#endif

struct faad {
	NeAACDecHandle hAac;
	u8_t type;
	// following used for mp4 only
	u32_t consume;
	u32_t pos;
	u32_t sample;
	u32_t nextchunk;
	void *stsc;
	u32_t skip;
	u64_t samples;
	u64_t sttssamples;
	bool  empty;
	struct chunk_table *chunkinfo;
	unsigned long samplerate;
	unsigned char channels;
	unsigned trak, play;
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
#define NEAAC(h, fn, ...) (NeAACDec ## fn)(__VA_ARGS__)
#else
#define NEAAC(h, fn, ...) (h)->NeAACDec##fn(__VA_ARGS__)
#endif

// minimal code for mp4 file parsing to extract audio config and find media data

// adapted from faad2/common/mp4ff
static u32_t mp4_desc_length(u8_t **buf) {
	u8_t b;
	u8_t num_bytes = 0;
	u32_t length = 0;

	do {
		b = **buf;
		*buf += 1;
		num_bytes++;
		length = (length << 7) | (b & 0x7f);
	} while ((b & 0x80) && num_bytes < 4);

	return length;
}

// read mp4 header to extract config data
static int read_mp4_header(unsigned long *samplerate_p, unsigned char *channels_p, struct thread_ctx_s *ctx) {
	struct faad *a = ctx->decode.handle;
	size_t bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
	char type[5];
	u32_t len;

	while (bytes >= 8) {
		// count trak to find the first playable one
		u32_t consume;

		len = unpackN((u32_t *)ctx->streambuf->readp);
		memcpy(type, ctx->streambuf->readp + 4, 4);
		type[4] = '\0';

		if (!strcmp(type, "moov")) {
			a->trak = 0;
			a->play = 0;
		}
		if (!strcmp(type, "trak")) {
			a->trak++;
		}

		// extract audio config from within esds and pass to DecInit2
		if (!strcmp(type, "esds") && bytes > len) {
			unsigned config_len;
			u8_t *ptr = ctx->streambuf->readp + 12;
			if (*ptr++ == 0x03) {
				mp4_desc_length(&ptr);
				ptr += 4;
			} else {
				ptr += 3;
			}
			mp4_desc_length(&ptr);
			ptr += 13;
			if (*ptr++ != 0x05) {
				LOG_WARN("[%p]: error parsing esds", ctx);
				return -1;
			}
			config_len = mp4_desc_length(&ptr);
			if (NEAAC(&ga, Init2, a->hAac, ptr, config_len, samplerate_p, channels_p) == 0) {
				LOG_DEBUG("[%p]: playable aac track: %u", ctx, a->trak);
				a->play = a->trak;
			}
		}

		// extract the total number of samples from stts
		if (!strcmp(type, "stts") && bytes > len) {
			u32_t i;
			u8_t *ptr = ctx->streambuf->readp + 12;
			u32_t entries = unpackN((u32_t *)ptr);
			ptr += 4;
			for (i = 0; i < entries; ++i) {
				u32_t count = unpackN((u32_t *)ptr);
				u32_t size = unpackN((u32_t *)(ptr + 4));
				a->sttssamples += count * size;
				ptr += 8;
			}
			LOG_DEBUG("[%p]: total number of samples contained in stts: %" PRIu64, ctx, a->sttssamples);
		}

		// stash sample to chunk info, assume it comes before stco
		if (!strcmp(type, "stsc") && bytes > len && !a->chunkinfo) {
			a->stsc = malloc(len - 12);
			if (a->stsc == NULL) {
				LOG_WARN("[%p]: malloc fail", ctx);
				return -1;
			}
			memcpy(a->stsc, ctx->streambuf->readp + 12, len - 12);
		}

		// build offsets table from stco and stored stsc
		if (!strcmp(type, "stco") && bytes > len && a->play == a->trak) {
			u32_t i;
			// extract chunk offsets
			u8_t *ptr = ctx->streambuf->readp + 12;
			u32_t entries = unpackN((u32_t *)ptr);
			ptr += 4;
			a->chunkinfo = malloc(sizeof(struct chunk_table) * (entries + 1));
			if (a->chunkinfo == NULL) {
				LOG_WARN("[%p]: malloc fail", ctx);
				return -1;
			}
			for (i = 0; i < entries; ++i) {
				a->chunkinfo[i].offset = unpackN((u32_t *)ptr);
				a->chunkinfo[i].sample = 0;
				ptr += 4;
			}
			a->chunkinfo[i].sample = 0;
			a->chunkinfo[i].offset = 0;
			// fill in first sample id for each chunk from stored stsc
			if (a->stsc) {
				u32_t stsc_entries = unpackN((u32_t *)a->stsc);
				u32_t sample = 0;
				u32_t last = 0, last_samples = 0;
				u8_t *ptr = (u8_t *)a->stsc + 4;
				while (stsc_entries--) {
					u32_t first = unpackN((u32_t *)ptr);
					u32_t samples = unpackN((u32_t *)(ptr + 4));
					if (last) {
						for (i = last - 1; i < first - 1; ++i) {
							a->chunkinfo[i].sample = sample;
							sample += last_samples;
						}
					}
					if (stsc_entries == 0) {
						for (i = first - 1; i < entries; ++i) {
							a->chunkinfo[i].sample = sample;
							sample += samples;
						}
					}
					last = first;
					last_samples = samples;
					ptr += 12;
				}
				free(a->stsc);
				a->stsc = NULL;
			}
		}

		// found media data, advance to start of first chunk and return
		if (!strcmp(type, "mdat")) {
			_buf_inc_readp(ctx->streambuf, 8);
			a->pos += 8;
			bytes  -= 8;
			if (a->play) {
				LOG_DEBUG("[%p]: type: mdat len: %u pos: %u", ctx, len, a->pos);
				if (a->chunkinfo && a->chunkinfo[0].offset > a->pos) {
					u32_t skip = a->chunkinfo[0].offset - a->pos; 	
					LOG_DEBUG("[%p]: skipping: %u", ctx, skip);
					if (skip <= bytes) {
						_buf_inc_readp(ctx->streambuf, skip);
						a->pos += skip;
					} else {
						a->consume = skip;
					}
				}
				a->sample = a->nextchunk = 1;
				return 1;
			} else {
				LOG_DEBUG("[%p]: type: mdat len: %u, no playable track found", ctx, len);
				return -1;
			}
		}

		// parse key-value atoms within ilst ---- entries to get encoder padding within iTunSMPB entry for gapless
		if (!strcmp(type, "----") && bytes > len) {
			u8_t *ptr = ctx->streambuf->readp + 8;
			u32_t remain = len - 8, size;
			if (!memcmp(ptr + 4, "mean", 4) && (size = unpackN((u32_t *)ptr)) < remain) {
				ptr += size; remain -= size;
			}
			if (!memcmp(ptr + 4, "name", 4) && (size = unpackN((u32_t *)ptr)) < remain && !memcmp(ptr + 12, "iTunSMPB", 8)) {
				ptr += size; remain -= size;
			}
			if (!memcmp(ptr + 4, "data", 4) && remain > 16 + 48) {
				// data is stored as hex strings: 0 start end samples
				u32_t b, c; u64_t d;
				if (sscanf((const char *)(ptr + 16), "%x %x %x %" PRIx64, &b, &b, &c, &d) == 4) {
					LOG_DEBUG("[%p]: iTunSMPB start: %u end: %u samples: " PRIu64, ctx, b, c, d);
					if (a->sttssamples && a->sttssamples < b + c + d) {
						LOG_DEBUG("[%p]: reducing samples as stts count is less", ctx);
						d = a->sttssamples - (b + c);
					}
					a->skip = b;
					a->samples = d;
				}
			}
		}

		// default to consuming entire box
		consume = len;

		// read into these boxes so reduce consume
		if (!strcmp(type, "moov") || !strcmp(type, "trak") || !strcmp(type, "mdia") || !strcmp(type, "minf") || !strcmp(type, "stbl") ||
			!strcmp(type, "udta") || !strcmp(type, "ilst")) {
			consume = 8;
		}
		// special cases which mix mix data in the enclosing box which we want to read into
		if (!strcmp(type, "stsd")) consume = 16;
		if (!strcmp(type, "mp4a")) consume = 36;
		if (!strcmp(type, "meta")) consume = 12;

		// consume rest of box if it has been parsed (all in the buffer) or is not one we want to parse
		if (bytes >= consume) {
			LOG_DEBUG("[%p]: type: %s len: %u consume: %u", ctx, type, len, consume);
			_buf_inc_readp(ctx->streambuf, consume);
			a->pos += consume;
			bytes -= consume;
		} else if ( !(!strcmp(type, "esds") || !strcmp(type, "stts") || !strcmp(type, "stsc") ||
					 !strcmp(type, "stco") || !strcmp(type, "----")) ) {
			LOG_DEBUG("[%p]: type: %s len: %u consume: %u - partial consume: %u", ctx, type, len, consume, bytes);
			_buf_inc_readp(ctx->streambuf, bytes);
			a->pos += bytes;
			a->consume = consume - bytes;
			break;
		} else if (len > bytes && len <= _buf_used(ctx->streambuf)) {
			// atom body wrapping around buffer, need to linearize it
			u8_t *buf = malloc(bytes);
			memcpy(buf, ctx->streambuf->readp, bytes);
			memmove(ctx->streambuf->buf + bytes, ctx->streambuf->buf, ctx->streambuf->writep - ctx->streambuf->buf);
			memcpy(ctx->streambuf->buf, buf, bytes);
			_buf_inc_writep(ctx->streambuf, bytes);
			LOG_WARN("[%p]: buffer wrap in mp4 header parsing type:%s len:%u bytes:%s", ctx, type, len, bytes);
			break;
		 } else if (len >= ctx->streambuf->size) {
			// can't process an atom larger than streambuf!
			LOG_ERROR("[%p]: atom %s too large for buffer %u %u", ctx, type, len, ctx->streambuf->size);
			return -1;
		 } else {
			break;
		 }
	}

	return 0;
}

static decode_state faad_decode(struct thread_ctx_s *ctx) {
	size_t bytes_total;
	size_t bytes_wrap;
	NeAACDecFrameInfo info;
	s16_t *iptr;
	bool endstream;
	frames_t frames;
	struct faad *a = ctx->decode.handle;

	LOCK_S;
	bytes_total = _buf_used(ctx->streambuf);
	bytes_wrap  = min(bytes_total, _buf_cont_read(ctx->streambuf));

	if (ctx->stream.state <= DISCONNECT && !bytes_total) {
		UNLOCK_S;
		return DECODE_COMPLETE;
	}

	if (a->consume) {
		u32_t consume = min(a->consume, bytes_wrap);
		LOG_DEBUG("[%p]: consume: %u of %u", ctx, consume, a->consume);
		_buf_inc_readp(ctx->streambuf, consume);
		a->pos += consume;
		a->consume -= consume;
		UNLOCK_S;
		return DECODE_RUNNING;
	}

	if (ctx->decode.new_stream) {
		int found = 0;

		if (a->type == '2') {

			// adts stream - seek for header
			while (bytes_wrap >= 2 && (*(ctx->streambuf->readp) != 0xFF || (*(ctx->streambuf->readp + 1) & 0xF6) != 0xF0)) {
				_buf_inc_readp(ctx->streambuf, 1);
				bytes_total--;
				bytes_wrap--;
			}

			if (bytes_wrap >= 2) {
				long n = NEAAC(&ga, Init, a->hAac, ctx->streambuf->readp, bytes_wrap, &a->samplerate, &a->channels);
				if (n < 0) {
					found = -1;
				} else {
					_buf_inc_readp(ctx->streambuf, n);
					found = 1;
				}
			}

		} else {
			// mp4 - read header
			found = read_mp4_header(&a->samplerate, &a->channels, ctx);
		}

		if (found == 1) {
			LOG_INFO("[%p]: samplerate: %u channels: %u", ctx, a->samplerate, a->channels);
			bytes_total = _buf_used(ctx->streambuf);
			bytes_wrap  = min(bytes_total, _buf_cont_read(ctx->streambuf));

			LOCK_O;
			LOG_INFO("[%p]: setting track_start", ctx);
			// don't use next_sample_rate
			ctx->output.current_sample_rate = decode_newstream(a->samplerate, ctx->output.supported_rates, ctx);
			ctx->output.track_start = ctx->outputbuf->writep;
			if (ctx->output.fade_mode) _checkfade(true, ctx);
			ctx->decode.new_stream = false;
			UNLOCK_O;

		} else if (found == -1) {

			LOG_WARN("[%p]: error reading stream header", ctx);
			UNLOCK_S;
			return DECODE_ERROR;

		} else {

			// not finished header parsing come back next time
			UNLOCK_S;
			return DECODE_RUNNING;
		}
	}

	if (bytes_wrap < WRAPBUF_LEN && bytes_wrap != bytes_total) {
		// make a local copy of frames which may have wrapped round the end of streambuf
		u8_t buf[WRAPBUF_LEN];
		memcpy(buf, ctx->streambuf->readp, bytes_wrap);
		memcpy(buf + bytes_wrap, ctx->streambuf->buf, WRAPBUF_LEN - bytes_wrap);
		iptr = NEAAC(&ga, Decode, a->hAac, &info, buf, WRAPBUF_LEN);

	} else {
		iptr = NEAAC(&ga, Decode, a->hAac, &info, ctx->streambuf->readp, bytes_wrap);
	}

	if (info.error) {
		LOG_WARN("[%p]: error: %u %s", ctx, info.error, NEAAC(&ga, GetErrorMessage, info.error));
	}

	endstream = false;

	// mp4 end of chunk - skip to next offset
	if (a->chunkinfo && a->chunkinfo[a->nextchunk].offset && a->sample++ == a->chunkinfo[a->nextchunk].sample) {

		if (a->chunkinfo[a->nextchunk].offset > a->pos) {
			u32_t skip = a->chunkinfo[a->nextchunk].offset - a->pos;
			if (skip != info.bytesconsumed) {
				LOG_DEBUG("[%p]: skipping to next chunk pos: %u consumed: %u != skip: %u", ctx, a->pos, info.bytesconsumed, skip);
			}
			if (bytes_total >= skip) {
				_buf_inc_readp(ctx->streambuf, skip);
				a->pos += skip;
			} else {
				a->consume = skip;
			}
			a->nextchunk++;
		} else {
			LOG_ERROR("[%p]: error: need to skip backwards!", ctx);
			endstream = true;
		}

	// adts and mp4 when not at end of chunk
	} else if (info.bytesconsumed != 0) {

		_buf_inc_readp(ctx->streambuf, info.bytesconsumed);
		a->pos += info.bytesconsumed;

	// error which doesn't advance streambuf - end
	} else {
		endstream = true;
	}

	UNLOCK_S;

	if (endstream) {
		LOG_WARN("[%p]: unable to decode further", ctx);
		return DECODE_ERROR;
	}

	if (!info.samples) {
		a->empty = true;
		return DECODE_RUNNING;
	}

	frames = info.samples / info.channels;

	if (a->skip) {
		u32_t skip;
		if (a->empty) {
			a->empty = false;
			a->skip -= frames;
			LOG_DEBUG("[%p]: gapless: first frame empty, skipped %u frames at start", ctx, frames);
		}
		skip = min(frames, a->skip);
		LOG_DEBUG("[%p]: gapless: skipping %u frames at start", ctx, skip);
		frames -= skip;
		a->skip -= skip;
		iptr += skip * info.channels;
	}

	if (a->samples) {
		if (a->samples < frames) {
			LOG_DEBUG("[%p]: gapless: trimming %u frames from end", ctx, frames - a->samples);
			frames = (frames_t)a->samples;
		}
		a->samples -= frames;
	}

	LOG_SDEBUG("[%p]: write %u frames", ctx, frames);

	LOCK_O_direct;

	while (frames > 0) {
		frames_t f;
		frames_t count;
		s16_t *optr = NULL;

		IF_DIRECT(
			f = _buf_cont_write(ctx->outputbuf) / BYTES_PER_FRAME;
			optr = (s16_t *)ctx->outputbuf->writep;
		);
		IF_PROCESS(
			f = ctx->process.max_in_frames;
			optr = (s16_t *)ctx->process.inbuf;
		);

		f = min(f, frames);
		count = f;

		if (info.channels == 2) {
			while (count--) {
				*optr++ = *iptr++;
				*optr++ = *iptr++;
			}
		} else if (info.channels == 1) {
			while (count--) {
				*optr++ = *iptr;
				*optr++ = *iptr++;
			}
		} else {
			LOG_WARN("[%^p]: unsupported number of channels", ctx);
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

	return DECODE_RUNNING;
}

static void faad_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	NeAACDecConfigurationPtr conf;
	struct faad *a = ctx->decode.handle;

	if (!a) {
		a = ctx->decode.handle = malloc(sizeof(struct faad));
		if (!a) return;
		a->hAac = a->chunkinfo = a->stsc = NULL;
	}

	// a bit of a hack here b/c sample size is not really a sample_size in that case
	LOG_INFO("[%p]: opening %s stream", ctx, sample_size == '2' ? "adts" : "mp4");

	a->type = sample_size;
	a->pos = a->consume = a->sample = a->nextchunk = 0;

	if (a->chunkinfo) free(a->chunkinfo);
	if (a->stsc) free(a->stsc);
	a->chunkinfo = NULL;
	a->stsc = NULL;
	a->skip = 0;
	a->samples = 0;
	a->sttssamples = 0;
	a->empty = false;

	if (a->hAac) {
		NEAAC(&ga, Close, a->hAac);
	}
	a->hAac = NEAAC(&ga, Open);

	conf = NEAAC(&ga, GetCurrentConfiguration, a->hAac);

	//FIXME: set 16 bits and maybe sample rate
	conf->outputFormat = FAAD_FMT_16BIT;
    conf->defSampleRate = 44100;
	conf->downMatrix = 1;

	if (!NEAAC(&ga, SetConfiguration, a->hAac, conf)) {
		LOG_WARN("[%p]: error setting config", ctx);
	};
}

static void faad_close(struct thread_ctx_s *ctx) {
	struct faad *a = ctx->decode.handle;

	NEAAC(&ga, Close, a->hAac);
	a->hAac = NULL;
	if (a->chunkinfo) {
		free(a->chunkinfo);
		a->chunkinfo = NULL;
	}
	if (a->stsc) {
		free(a->stsc);
		a->stsc = NULL;
	}
	free(a);
	ctx->decode.handle = NULL;
}

static bool load_faad(void) {
#if !LINKALL
	char *err;

	ga.handle = dlopen(LIBFAAD, RTLD_NOW);

	if (!ga.handle) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	ga.NeAACDecGetCurrentConfiguration = dlsym(ga.handle, "NeAACDecGetCurrentConfiguration");
	ga.NeAACDecSetConfiguration = dlsym(ga.handle, "NeAACDecSetConfiguration");
	ga.NeAACDecOpen = dlsym(ga.handle, "NeAACDecOpen");
	ga.NeAACDecClose = dlsym(ga.handle, "NeAACDecClose");
	ga.NeAACDecInit = dlsym(ga.handle, "NeAACDecInit");
	ga.NeAACDecInit2 = dlsym(ga.handle, "NeAACDecInit2");
	ga.NeAACDecDecode = dlsym(ga.handle, "NeAACDecDecode");
	ga.NeAACDecGetErrorMessage = dlsym(ga.handle, "NeAACDecGetErrorMessage");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);
		return false;
	}

	LOG_INFO("loaded "LIBFAAD"", NULL);
#endif

	return true;
}

struct codec *register_faad(void) {
	static struct codec ret = { 
		'a',          // id
		"aac",        // types
		WRAPBUF_LEN,  // min read
		20480,        // min space
		faad_open,    // open
		faad_close,   // close
		faad_decode,  // decode
	};

	if (!load_faad()) {
		return NULL;
	}

	LOG_INFO("using faad to decode aac", NULL);
	return &ret;
}


void deregister_faad(void) {
#if !LINKALL
	if (ga.handle) dlclose(ga.handle);
#endif
}

