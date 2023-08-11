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
#include "raop_client.h"
#include "cross_util.h"
#include "spotify.h"

#define VERSION "v0.1.7" " (" __DATE__ " @ " __TIME__ ")"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define STR_LEN 256

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

typedef struct sMRConfig
{
	bool		Enabled;
	char		Name[STR_LEN];
	uint8_t		MAC[6];
	bool		SendMetaData;
	bool		SendCoverArt;
	int			VorbisRate;
	int			RemoveTimeout;
	bool		Encryption;
	char		Credentials[STR_LEN];
	int 		ReadAhead;
	int			VolumeMode;
	int			VolumeFeedback;
	bool		AlacEncode;
} tMRConfig;


struct sMR {
	uint32_t Magic;
	bool  Running;
	uint32_t Expired;
	tMRConfig Config;
	char UDN			[RESOURCE_LENGTH];
	char FriendlyName	[RESOURCE_LENGTH];
	struct spotPlayer* SpotPlayer;
	enum spotEvent	SpotState;
	double			Volume;
	bool			Muted;
	uint32_t		VolumeStampRx;
	uint8_t			mac[6];
	struct raopcl_s	*Raop;
	struct in_addr 	PlayerIP;
	uint16_t		PlayerPort;
	uint8_t			PlayerStatus;
	pthread_mutex_t Mutex;
	char ActiveRemote[16];
	uint32_t SkipStart;
	bool SkipDir;
};

extern char 				glInterface[];
extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern struct sMR			glMRDevices[MAX_RENDERERS];
extern char					glExcluded[STR_LEN];
extern uint16_t				glPortBase, glPortRange;

#ifdef __cplusplus
extern "C" {
#endif

bool AppleTVPairing(void);

#ifdef __cplusplus
}
#endif
