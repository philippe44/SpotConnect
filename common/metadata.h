/*
 *  Metadata instance
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 *
 */

#pragma once

#include <stdint.h>

typedef struct metadata_s {
	const char* artist;
	const char* album;
	const char* title;
	const char* remote_title;
	const char* artwork;
	const char *genre;
	uint32_t track;
	uint32_t duration;
	uint32_t sample_rate;
	uint8_t  sample_size;
	uint8_t  channels;
} metadata_t;
