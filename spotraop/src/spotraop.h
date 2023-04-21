/*
 *  Squeeze2raop - Squeezelite to AirPlay bridge
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma once

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include "pthread.h"

#include "platform.h"
#include "squeezedefs.h"
#include "squeezeitf.h"
#include "raop_client.h"
#include "cross_util.h"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define MAX_PROTO		128
#define MAX_RENDERERS	32
#define MAGIC			0xAABBCCDD
#define RESOURCE_LENGTH	250
#define	SCAN_TIMEOUT 	15
#define SCAN_INTERVAL	30

#define PLAYER_LATENCY	1500

#define DMCP_PREVENT_PLAYBACK	0x01
#define DMCP_BUSY				0x02

enum { CONFIG_CREATE, CONFIG_UPDATE, CONFIG_MIGRATE };

typedef struct sRaopReq {
	char Type[20];
	union sRaopReqData {
		float Volume;
		uint64_t FlushTS;
		struct {
			struct sMR* Device;
			uint32_t Hash;
			char* Url, *ContentType, *Image;
			int Size;
		} Artwork;
	} Data;
} tRaopReq;

typedef struct sMRConfig
{
	bool		Enabled;
	bool		SendMetaData;
	bool		SendCoverArt;
	bool		AutoPlay;
	int			IdleTimeout;
	int			RemoveTimeout;
	bool		Encryption;
	char		Credentials[STR_LEN];
	int 		ReadAhead;
	int			VolumeMode;
	int			Volume;
	int			VolumeFeedback;
	char		VolumeMapping[STR_LEN];
	bool		MuteOnPause;
	bool		AlacEncode;
} tMRConfig;


struct sMR {
	uint32_t Magic;
	bool  Running;
	uint32_t Expired;
	tMRConfig Config;
	sq_dev_param_t	sq_config;
	bool on;
	char UDN			[RESOURCE_LENGTH];
	char FriendlyName	[RESOURCE_LENGTH];
	char			ContentType[STR_LEN];		// a bit patchy ... to buffer next URI
	int	 			SqueezeHandle;
	sq_action_t		sqState;
	int8_t			Volume;
	uint32_t		VolumeStampRx;
	float 			VolumeMapping[101];
	struct raopcl_s	*Raop;
	struct in_addr 	PlayerIP;
	uint16_t		PlayerPort;
	pthread_t		Thread;
	uint8_t			PlayerStatus;
	pthread_mutex_t Mutex;
	pthread_cond_t	Cond;
	bool			Delete;
	uint32_t		Busy;
	cross_queue_t	Queue;
	uint32_t		LastFlush;
	bool			DiscWait;
	int				Sane;
	bool			TrackRunning;
	uint8_t			MetadataWait;
	uint32_t			MetadataHash;
	char *SampleSize;
	char *SampleRate;
	char *Channels;
	char *Codecs;
	char *Crypto;
	char ActiveRemote[16];
	uint32_t SkipStart;
	bool SkipDir;
};

extern char 				glInterface[];
extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern sq_dev_param_t		glDeviceParam;
extern struct sMR			glMRDevices[MAX_RENDERERS];
extern char					glExcluded[STR_LEN];
extern int					glMigration;
extern char					glPortOpen[STR_LEN];

