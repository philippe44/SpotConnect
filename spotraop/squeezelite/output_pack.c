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

// Scale and pack functions

#include "squeezelite.h"

#define MAX_VAL16 0x7fffffffLL
#define MAX_SCALESAMPLE 0x7fffffffffffLL
#define MIN_SCALESAMPLE -MAX_SCALESAMPLE


/*---------------------------------------------------------------------------*/
inline s16_t gain(s32_t gain, s16_t sample) {
	s64_t res = (s64_t)gain * sample;
	if (res > MAX_VAL16) res = MAX_VAL16;
	else if (res < -MAX_VAL16) res = -MAX_VAL16;
	return (s16_t) (res >> 16);
}


/*---------------------------------------------------------------------------*/
s32_t gain32(s32_t gain, s32_t sample) {
	s64_t res = (s64_t)gain * (s64_t)sample;
	if (res > MAX_SCALESAMPLE) res = MAX_SCALESAMPLE;
	if (res < MIN_SCALESAMPLE) res = MIN_SCALESAMPLE;
	return (s32_t) (res >> 16);
}


/*---------------------------------------------------------------------------*/
s32_t to_gain(float f) {
	return (s32_t)(f * 65536.0F);
}


/*---------------------------------------------------------------------------*/
void _scale_frames(s16_t *outputptr, s16_t *inputptr, frames_t count, s32_t gainL, s32_t gainR, u8_t flags)
{
	if (gainL == FIXED_ONE && gainR == FIXED_ONE && !(flags & (MONO_LEFT | MONO_RIGHT))) {
		memcpy(outputptr, inputptr, count * BYTES_PER_FRAME);
	}  else if ((flags & MONO_LEFT) && (flags & MONO_RIGHT)) {
		while (count--) {
			s32_t sample = gain(gainL, *inputptr++);
			sample = (sample + gain(gainR, *inputptr++)) / 2;
			*outputptr++ = sample;
			*outputptr++ = sample;
		}
	} else if (flags & MONO_LEFT) {
		while (count--) {
			inputptr++;
			*(outputptr + 1) = *outputptr = gain(gainR, *inputptr++);
			outputptr += 2;
		}
	} else if (flags & MONO_RIGHT) {
		while (count--) {
			*(outputptr + 1) = *outputptr = gain(gainL, *inputptr++);
			inputptr++;
			outputptr += 2;
		}
   } else {
		while (count--) {
			*outputptr++ = gain(gainL, *inputptr++);
			*outputptr++ = gain(gainR, *inputptr++);
		}
	}
}

/*---------------------------------------------------------------------------*/
void _apply_cross(struct buffer *outputbuf, frames_t out_frames, s32_t cross_gain_in, s32_t cross_gain_out, s16_t **cross_ptr) {
	s16_t *ptr = (s16_t *)(void *)outputbuf->readp;
	frames_t count = out_frames * 2;

	while (count--) {
		if (*cross_ptr > (s16_t *) outputbuf->wrap) {
			*cross_ptr -= outputbuf->size / BYTES_PER_FRAME * 2;
		}
		*ptr = gain(cross_gain_out, *ptr) + gain(cross_gain_in, **cross_ptr);
		ptr++; (*cross_ptr)++;
	}
}


