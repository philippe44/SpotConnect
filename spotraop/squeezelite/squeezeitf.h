/* 
 *  Squeeze2raop - Squeezelite to AirPlay bridge
 *
 *  (c) Philippe, philippe_44@outlook.com for raop/multi-instance modifications
 *  
 *  See LICENSE
 *
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "squeezedefs.h"
#include "metadata.h"

typedef enum { SQ_NONE, SQ_PLAY, SQ_PAUSE, SQ_UNPAUSE, SQ_STOP, SQ_SEEK,
			  SQ_VOLUME, SQ_ONOFF, SQ_NEXT, SQ_CONNECT, SQ_STARTED,
			  SQ_METASEND, SQ_SETNAME, SQ_SETSERVER, SQ_FINISHED, SQ_PLAY_PAUSE,
			  SQ_MUTE_TOGGLE, SQ_PREVIOUS, SQ_SHUFFLE,
			  SQ_FF_REW, SQ_OFF } sq_action_t;

typedef	sq_action_t sq_event_t;

#define FRAMES_PER_BLOCK			352
#define MAX_SUPPORTED_SAMPLERATES 	2

#define STREAMBUF_SIZE 				(2 * 1024 * 1024)
#define OUTPUTBUF_SIZE 				(44100 * 4 * 10)

typedef enum { L24_PACKED, L24_PACKED_LPCM, L24_TRUNC_16, L24_TRUNC_16_PCM, L24_UNPACKED_HIGH, L24_UNPACKED_LOW } sq_L24_pack_t;
typedef enum { FLAC_NO_HEADER = 0, FLAC_NORMAL_HEADER = 1, FLAC_FULL_HEADER = 2 } sq_flac_header_t;
typedef	int	sq_dev_handle_t;
typedef unsigned sq_rate_t;

typedef	struct sq_dev_param_s {
	unsigned 	streambuf_size;
	unsigned 	outputbuf_size;
	char		codecs[STR_LEN];
	char 		server[STR_LEN];
	char 		name[STR_LEN];
	uint8_t		mac[6];
	char 		resolution[STR_LEN];
	bool		soft_volume;
#if defined(RESAMPLE)
	uint32_t	sample_rate;
	bool		resample;
	char		resample_options[STR_LEN];
#endif
	struct {
		char 	server[STR_LEN];
	} dynamic;
} sq_dev_param_t;

struct raopcl_s;

typedef bool (*sq_callback_t)(void *caller, sq_action_t action, ...);

void				sq_init(struct in_addr host, char *model_name);
void				sq_end(void);

bool			 	sq_run_device(sq_dev_handle_t handle, struct raopcl_s *raopcl, sq_dev_param_t *param);
void				sq_delete_device(sq_dev_handle_t);
sq_dev_handle_t		sq_reserve_device(void *caller_id, sq_callback_t callback);
void				sq_release_device(sq_dev_handle_t);

void				sq_notify(sq_dev_handle_t handle, sq_event_t event, ...);
bool				sq_get_metadata(sq_dev_handle_t handle, metadata_t *metadata, bool next);
uint32_t			sq_get_time(sq_dev_handle_t handle);
bool 				sq_set_time(sq_dev_handle_t handle, char *pos);
sq_action_t 		sq_get_mode(sq_dev_handle_t handle);
void*				sq_get_ptr(sq_dev_handle_t handle);

