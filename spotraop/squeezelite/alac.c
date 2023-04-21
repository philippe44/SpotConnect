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

#include <alac_wrapper.h>

#define BLOCK_SIZE (4096 * BYTES_PER_FRAME)

struct chunk_table {
	u32_t sample, offset;
};

struct alac {
	void *decoder;
	u8_t *writebuf;
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
	u32_t  *block_size, default_block_size, block_index;
	unsigned sample_rate;
	unsigned char channels, sample_size;
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

// read mp4 header to extract config data
static int read_mp4_header(struct thread_ctx_s *ctx) {
	struct alac *l = ctx->decode.handle;
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
			l->trak = 0;
			l->play = 0;
		}
		if (!strcmp(type, "trak")) {
			l->trak++;
		}

		// extract audio config from within alac
		if (!strcmp(type, "alac") && bytes > len) {
			u8_t *ptr = ctx->streambuf->readp + 36;
			unsigned int block_size;
			l->play = l->trak;
			l->decoder = alac_create_decoder(len - 36, ptr, &l->sample_size, &l->sample_rate, &l->channels, &block_size);
			l->writebuf = malloc(block_size + 256);
			LOG_INFO("[%p]: allocated write buffer of %u bytes", ctx, block_size);
			if (!l->writebuf) {
				LOG_ERROR("[%p]: allocation failed", ctx);
				return -1;
			}
		}

		// extract the total number of samples from stts
		if (!strcmp(type, "stsz") && bytes > len) {
			u32_t i;
			u8_t *ptr = ctx->streambuf->readp + 12;
			l->default_block_size = unpackN((u32_t *) ptr); ptr += 4;
			if (!l->default_block_size) {
				u32_t entries = unpackN((u32_t *)ptr); ptr += 4;
				l->block_size = malloc((entries + 1)* 4);
				for (i = 0; i < entries; i++) {
					l->block_size[i] = unpackN((u32_t *)ptr); ptr += 4;
				}
				l->block_size[entries] = 0;
				LOG_DEBUG("[%p]: total blocksize contained in stsz %u", ctx, entries);
			} else {
				LOG_DEBUG("[%p]: fixed blocksize in stsz %u", ctx, l->default_block_size);
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
				l->sttssamples += count * size;
				ptr += 8;
			}
			LOG_DEBUG("[%p]: total number of samples contained in stts: %" PRIu64, ctx, l->sttssamples);
		}

		// stash sample to chunk info, assume it comes before stco
		if (!strcmp(type, "stsc") && bytes > len && !l->chunkinfo) {
			l->stsc = malloc(len - 12);
			if (l->stsc == NULL) {
				LOG_WARN("[%p]: malloc fail", ctx);
				return -1;
			}
			memcpy(l->stsc, ctx->streambuf->readp + 12, len - 12);
		}

		// build offsets table from stco and stored stsc
		if (!strcmp(type, "stco") && bytes > len && l->play == l->trak) {
			u32_t i;
			// extract chunk offsets
			u8_t *ptr = ctx->streambuf->readp + 12;
			u32_t entries = unpackN((u32_t *)ptr);
			ptr += 4;
			l->chunkinfo = malloc(sizeof(struct chunk_table) * (entries + 1));
			if (l->chunkinfo == NULL) {
				LOG_WARN("[%p]: malloc fail", ctx);
				return -1;
			}
			for (i = 0; i < entries; ++i) {
				l->chunkinfo[i].offset = unpackN((u32_t *)ptr);
				l->chunkinfo[i].sample = 0;
				ptr += 4;
			}
			l->chunkinfo[i].sample = 0;
			l->chunkinfo[i].offset = 0;
			// fill in first sample id for each chunk from stored stsc
			if (l->stsc) {
				u32_t stsc_entries = unpackN((u32_t *)l->stsc);
				u32_t sample = 0;
				u32_t last = 0, last_samples = 0;
				u8_t *ptr = (u8_t *)l->stsc + 4;
				while (stsc_entries--) {
					u32_t first = unpackN((u32_t *)ptr);
					u32_t samples = unpackN((u32_t *)(ptr + 4));
					if (last) {
						for (i = last - 1; i < first - 1; ++i) {
							l->chunkinfo[i].sample = sample;
							sample += last_samples;
						}
					}
					if (stsc_entries == 0) {
						for (i = first - 1; i < entries; ++i) {
							l->chunkinfo[i].sample = sample;
							sample += samples;
						}
					}
					last = first;
					last_samples = samples;
					ptr += 12;
				}
				free(l->stsc);
				l->stsc = NULL;
			}
		}

		// found media data, advance to start of first chunk and return
		if (!strcmp(type, "mdat")) {
			_buf_inc_readp(ctx->streambuf, 8);
			l->pos += 8;
			bytes  -= 8;
			if (l->play) {
				LOG_DEBUG("[%p]: type: mdat len: %u pos: %u", ctx, len, l->pos);
				if (l->chunkinfo && l->chunkinfo[0].offset > l->pos) {
					u32_t skip = l->chunkinfo[0].offset - l->pos;
					LOG_DEBUG("[%p]: skipping: %u", ctx, skip);
					if (skip <= bytes) {
						_buf_inc_readp(ctx->streambuf, skip);
						l->pos += skip;
					} else {
						l->consume = skip;
					}
				}
				l->sample = l->nextchunk = 1;
				l->block_index = 0;
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
					LOG_DEBUG("[%p]: iTunSMPB start: %u end: %u samples: %" PRIu64, ctx, b, c, d);
					if (l->sttssamples && l->sttssamples < b + c + d) {
						LOG_DEBUG("[%p]: reducing samples as stts count is less", ctx);
						d = l->sttssamples - (b + c);
					}
					l->skip = b;
					l->samples = d;
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
			l->pos += consume;
			bytes -= consume;
		} else if ( !(!strcmp(type, "esds") || !strcmp(type, "stts") || !strcmp(type, "stsc") ||
					  !strcmp(type, "stsz") || !strcmp(type, "stco") || !strcmp(type, "----")) ) {
			LOG_DEBUG("[%p]: type: %s len: %u consume: %u - partial consume: %u", ctx, type, len, consume, bytes);
			_buf_inc_readp(ctx->streambuf, bytes);
			l->pos += bytes;
			l->consume = consume - bytes;
			break;
		} else if (len >= ctx->streambuf->size) {
			// can't process an atom larger than streambuf!
			LOG_ERROR("[%p]: atom %s too large for buffer %u %u", ctx, type, len, ctx->streambuf->size);
			return -1;
		} else {
			// make sure we have 'len' contiguous space in streambuf (large headers)
			_buf_unwrap(ctx->streambuf, len);
			break;
		}
	}

	return 0;
}

static decode_state alac_decode(struct thread_ctx_s *ctx) {
	struct alac *l = ctx->decode.handle;
	size_t bytes;
	bool endstream;
	u8_t *iptr;
	u32_t frames, block_size;

	LOCK_S;

	// data not reached yet
	if (l->consume) {
		u32_t consume = min(l->consume, _buf_used(ctx->streambuf));
		LOG_DEBUG("[%p]: consume: %u of %u", ctx, consume, l->consume);
		_buf_inc_readp(ctx->streambuf, consume);
		l->pos += consume;
		l->consume -= consume;
		UNLOCK_S;
		return DECODE_RUNNING;
	}

	if (ctx->decode.new_stream) {
		int found = 0;

		// mp4 - read header
		found = read_mp4_header(ctx);

		if (found == 1) {
			LOG_INFO("[%p]: sample_rate: %u channels: %u", ctx, l->sample_rate, l->channels);
			bytes = min(_buf_used(ctx->streambuf), _buf_cont_read(ctx->streambuf));
			LOG_INFO("[%p]: setting track_start", ctx);
			LOCK_O;
			// don't use next_sample_rate
			ctx->output.current_sample_rate = decode_newstream(l->sample_rate, ctx->output.supported_rates, ctx);
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

	bytes = _buf_used(ctx->streambuf);
	block_size = l->default_block_size ? l->default_block_size : l->block_size[l->block_index];

	// stream terminated
	if (ctx->stream.state <= DISCONNECT && (bytes == 0 || block_size == 0)) {
		UNLOCK_S;
		LOG_DEBUG("[%p]: end of stream", ctx);
		return DECODE_COMPLETE;
	}

	// enough data for coding
	if (bytes < block_size) {
		UNLOCK_S;
		return DECODE_RUNNING;
	} else if (block_size != l->default_block_size) l->block_index++;

	bytes = min(bytes, _buf_cont_read(ctx->streambuf));

	// need to create a buffer with contiguous data
	if (bytes < block_size) {
		iptr = malloc(block_size);
		memcpy(iptr, ctx->streambuf->readp, bytes);
		memcpy(iptr + bytes, ctx->streambuf->buf, block_size - bytes);
	} else iptr = ctx->streambuf->readp;

	if (!alac_to_pcm(l->decoder, iptr, l->writebuf, 2, &frames)) {
		LOG_ERROR("[%p]: decode error", ctx);
		UNLOCK_S;
		return DECODE_ERROR;
	}

	// and free it
	if (bytes < block_size) free(iptr);

	LOG_SDEBUG("[%p]: block of %u bytes (%u frames)", ctx, block_size, frames);

	endstream = false;
	// mp4 end of chunk - skip to next offset
	if (l->chunkinfo && l->chunkinfo[l->nextchunk].offset && l->sample++ == l->chunkinfo[l->nextchunk].sample) {
		 if (l->chunkinfo[l->nextchunk].offset > l->pos) {
			u32_t skip = l->chunkinfo[l->nextchunk].offset - l->pos;
			if (_buf_used(ctx->streambuf) >= skip) {
				_buf_inc_readp(ctx->streambuf, skip);
				l->pos += skip;
			} else {
				l->consume = skip;
			}
			l->nextchunk++;
		 } else {
			LOG_ERROR("[%p]: error: need to skip backwards!", ctx);
			endstream = true;
		 }
	// mp4 when not at end of chunk
	} else if (frames) {
		_buf_inc_readp(ctx->streambuf, block_size);
		l->pos += block_size;
	} else {
		endstream = true;
	}

	UNLOCK_S;

	if (endstream) {
		LOG_WARN("[%p]: unable to decode further", ctx);
		return DECODE_ERROR;
	}

	// now point at the beginning of decoded samples
	iptr = l->writebuf;

	if (l->skip) {
		u32_t skip;
		if (l->empty) {
			l->empty = false;
			l->skip -= frames;
			LOG_DEBUG("[%p]: gapless: first frame empty, skipped %u frames at start", ctx, frames);
		}
		skip = min(frames, l->skip);
		LOG_DEBUG("[%p]: gapless: skipping %u frames at start", ctx, skip);
		frames -= skip;
		l->skip -= skip;
		iptr += skip * l->channels * l->sample_size;
	}

	if (l->samples) {
		if (l->samples < frames) {
			LOG_DEBUG("[%p]: gapless: trimming %u frames from end", ctx, frames - l->samples);
			frames = l->samples;
		}
		l->samples -= frames;
	}

	LOCK_O_direct;

	while (frames > 0) {
		size_t f, count;
		s16_t *optr = NULL;

		IF_DIRECT(
			f = min(frames, _buf_cont_write(ctx->outputbuf) / BYTES_PER_FRAME);
			optr = (s16_t *)ctx->outputbuf->writep;
		);
		IF_PROCESS(
			f = min(frames, ctx->process.max_in_frames - ctx->process.in_frames);
			optr = (s16_t *)((u8_t *) ctx->process.inbuf + ctx->process.in_frames * BYTES_PER_FRAME);
		);

		f = min(f, frames);
		count = f;

		if (l->sample_size == 8) {
			while (count--) {
				*optr++ = (*(u16_t*) iptr) << 8;
				*optr++ = (*(u16_t*) (iptr + 1)) << 8;
				iptr += 2;
			}
		} else if (l->sample_size == 16) {
			while (count--) {
				*optr++ = (*(u16_t*) iptr);
				*optr++ = (*(u16_t*) (iptr + 2));
				iptr += 4;
			}
		} else if (l->sample_size == 24) {
			while (count--) {
				*optr++ = (*(u32_t*) iptr) >> 8;
				*optr++ = (*(u32_t*) (iptr + 3)) >> 8;
				iptr += 6;
			}
		} else if (l->sample_size == 32) {
			while (count--) {
				*optr++ = (*(u32_t*) iptr) >> 16;
				*optr++ = (*(u32_t*) (iptr + 4)) >> 16;
				iptr += 8;
			}
		} else {
			LOG_ERROR("[%p]: unsupported bits per sample: %u", ctx, l->sample_size);
		}

		frames -= f;

		IF_DIRECT(
			_buf_inc_writep(ctx->outputbuf, f * BYTES_PER_FRAME);
		);
		IF_PROCESS(
			ctx->process.in_frames = f;
			// called only if there is enough space in process buffer
			if (frames) LOG_ERROR("[%p]: unhandled case", ctx);
		);
	 }

	UNLOCK_O_direct;

	return DECODE_RUNNING;
}

static void alac_cleanup (struct alac *l) {
	if (l->decoder) alac_delete_decoder(l->decoder);
	if (l->writebuf) free(l->writebuf);
	if (l->chunkinfo) free(l->chunkinfo);
	if (l->block_size) free(l->block_size);
	if (l->stsc) free(l->stsc);
	memset(l, 0, sizeof(struct alac));
}

static void alac_open(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx) {
	struct alac *l = ctx->decode.handle;

	if (!l) {
		if ((l = calloc(1, sizeof(struct alac))) == NULL) return;
		ctx->decode.handle = l;
	} else alac_cleanup(l);
}

static void alac_close(struct thread_ctx_s *ctx) {
	struct alac *l = ctx->decode.handle;

	ctx->decode.handle = NULL;
	alac_cleanup(l);
	free(l);
}

struct codec *register_alac(void) {
	static struct codec ret = {
		'l',            // id
		"alc",          // types
		BLOCK_SIZE,	    // min read
		BLOCK_SIZE * 4, // min space assuming a ratio of 4
		alac_open,      // open
		alac_close,     // close
		alac_decode,    // decode
	};

	LOG_INFO("using alac to decode alc", NULL);
	return &ret;
}


void deregister_alac(void) {
}

