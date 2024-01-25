/*
 *  SpotUPnP - Spotify to uPNP gateway
 *
 *	(c) Philippe, philippe_44@outlook.com
 *
 * See LICENSE
 *
 */

#pragma once

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <locale.h>

#include "pthread.h"
#include "upnp.h"

#include "platform.h"
#include "cross_util.h"
#include "metadata.h"
#include "spotify.h"

#define VERSION "v0.9.1"" ("__DATE__" @ "__TIME__")"

/*----------------------------------------------------------------------------*/
/* typedefs */
/*----------------------------------------------------------------------------*/

#define STR_LEN	256
#define CREDENTIALS_LEN	1024

#define MAX_RENDERERS	32
#define RESOURCE_LENGTH	250

enum 	eMRstate { UNKNOWN, STOPPED, PLAYING, PAUSED, TRANSITIONING };
enum 	{ AVT_SRV_IDX = 0, REND_SRV_IDX, CNX_MGR_IDX, TOPOLOGY_IDX, GRP_REND_SRV_IDX, NB_SRV };

struct sService {
	char Id			[RESOURCE_LENGTH];
	char Type		[RESOURCE_LENGTH];
	char EventURL	[RESOURCE_LENGTH];
	char ControlURL	[RESOURCE_LENGTH];
	Upnp_SID		SID;
	int32_t			TimeOut;
	uint32_t		Failed;
};

typedef struct sMRConfig
{
	bool		Enabled;
	char		Credentials[CREDENTIALS_LEN];
	char		Name[STR_LEN];
	uint8_t		mac[6];
	int			UPnPMax;
	int			MaxVolume;
	char		Codec[STR_LEN];
	int			VorbisRate;
	bool		Flow;
	int			CacheMode;
	bool		Gapless;
	int64_t		HTTPContentLength;
	bool		SendMetaData;
	bool		SendCoverArt;
	char		ArtWork[4*STR_LEN];
} tMRConfig;

struct sMR {
	bool  Running;
	tMRConfig Config;
	char Credentials[CREDENTIALS_LEN];
	char UDN			[RESOURCE_LENGTH];
	char DescDocURL		[RESOURCE_LENGTH];
	char friendlyName	[STR_LEN];
	enum eMRstate 	State;
	bool			ExpectStop;
	struct spotPlayer *SpotPlayer;
	metadata_t		MetaData;
	enum spotEvent	SpotState;
	uint32_t		Elapsed, ElapsedAccrued;
	uint32_t		LastSeen;
	uint8_t			*seqN;
	void			*WaitCookie, *StartCookie, *LastCookie;
	cross_queue_t	ActionQueue;
	unsigned		TrackPoll, StatePoll;
	struct sService Service[NB_SRV];
	struct sAction	*Actions;
	struct sMR		*Master;
	pthread_mutex_t Mutex;
	pthread_t 		Thread;
	double			Volume;		// to avoid int volume being stuck at 0
	uint32_t		VolumeStampRx, VolumeStampTx;
	int				ErrorCount;
	bool			TimeOut;
	char 			ProtocolInfo[STR_LEN];
	bool			Gapless;
	char			TrackURI[STR_LEN];
	char*			NextStreamUrl;
};

extern UpnpClient_Handle   	glControlPointHandle;
extern int32_t				glLogLimit;
extern tMRConfig			glMRConfig;
extern struct sMR			*glMRDevices;
extern int					glMaxDevices;
extern char					glInterface[128];
extern unsigned short		glPortBase, glPortRange;
extern char					glCredentialsPath[STR_LEN];
extern bool					glCredentials;

int MasterHandler(Upnp_EventType EventType, const void *Event, void *Cookie);
int ActionHandler(Upnp_EventType EventType, const void *Event, void *Cookie);
