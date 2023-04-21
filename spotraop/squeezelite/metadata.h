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
#include <stdbool.h>

typedef struct metadata_s {
	char* artist;
	char* album;
	char* title;
	char* remote_title;
	char* artwork;
	char *genre;
	uint32_t track, index;
	uint32_t duration;
	uint32_t size;
	uint32_t sample_rate;
	uint8_t  sample_size;
	uint8_t  channels;
	uint32_t bitrate;
	bool remote;
	int repeating;
} metadata_t;

struct metadata_s* metadata_init(struct metadata_s* self);
struct metadata_s* metadata_clone(struct metadata_s* const self);
void   metadata_free(struct metadata_s* const self);
void   metadata_defaults(struct metadata_s* const self);
