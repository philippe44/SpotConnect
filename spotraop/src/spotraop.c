/*
 *  Squeeze2raop - Squeezelite to Airplay bridge
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 * See LICENSE
 *
 */

#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <limits.h>
#include <string.h>

#include "platform.h"
#include "squeezedefs.h"

#if USE_SSL
#include <openssl/ssl.h>
#endif

#if WIN
#include <process.h>
#endif

#include "ixml.h"
#include "cross_ssl.h"
#include "cross_util.h"
#include "cross_log.h"
#include "cross_net.h"
#include "cross_thread.h"
#include "squeeze2raop.h"
#include "config_raop.h"
#include "metadata.h"

#include "mdnssd.h"
#include "mdnssvc.h"
#include "http_fetcher.h"

#define DISCOVERY_TIME 20

#define MODEL_NAME_STRING	"RaopBridge"

enum { VOLUME_IGNORE = 0, VOLUME_SOFT = 1, VOLUME_HARD = 2};
enum { VOLUME_FEEDBACK = 1, VOLUME_UNFILTERED = 2};

/*----------------------------------------------------------------------------*/
/* globals 																	  */
/*----------------------------------------------------------------------------*/
int32_t				glLogLimit = -1;
uint32_t			glMask;
char 				glInterface[16] = "?";
char				glExcluded[STR_LEN] = "aircast,airupnp,shairtunes2,airesp32";
int					glMigration = 0;
struct sMR			glMRDevices[MAX_RENDERERS];
char				glPortOpen[STR_LEN];

log_level	slimproto_loglevel = lINFO;
log_level	stream_loglevel = lWARN;
log_level	decode_loglevel = lWARN;
log_level	output_loglevel = lINFO;
log_level	main_loglevel = lINFO;
log_level	slimmain_loglevel = lINFO;
log_level	util_loglevel = lINFO;
log_level	raop_loglevel = lINFO;
bool 		log_cmdline = false;

tMRConfig			glMRConfig = {
							true,
							true,
							true,
							false,
							30, 			// IdleTimeout
							0,				// RemoveTimeout
							false,
							"",				 // credentials
							1000,			 // read_ahead
							2,				 // VolumeMode = HARDWARE
							-1,				 // Volume = nothing at first connection
							VOLUME_FEEDBACK, // volumeFeedback
							"-30:1, -15:50, 0:100",
							true,			 // mute_on_pause
							false,			 // alac_encode
					};

static uint8_t LMSVolumeMap[129] = {
			0, 3, 6, 7, 8, 10, 12, 13, 14, 16, 17, 18, 19, 20,
			21, 22, 24, 25, 26, 27, 28, 28, 29, 30, 31, 32, 33,
			34, 35, 36, 37, 37, 38, 39, 40, 41, 41, 42, 43, 44,
			45, 45, 46, 47, 48, 48, 49, 50, 51, 51, 52, 53, 53,
			54, 55, 55, 56, 57, 57, 58, 59, 60, 60, 61, 61, 62,
			63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 70,
			71, 72, 72, 73, 73, 74, 75, 75, 76, 76, 77, 78, 78,
			79, 79, 80, 80, 81, 82, 82, 83, 83, 84, 84, 85, 86,
			86, 87, 87, 88, 88, 89, 89, 90, 91, 91, 92, 92, 93,
			93, 94, 94, 95, 95, 96, 96, 97, 98, 99, 100
		};

sq_dev_param_t glDeviceParam = {
					STREAMBUF_SIZE,
					OUTPUTBUF_SIZE,
					"aac,ogg,ops,ogf,flc,alc,wav,aif,pcm,mp3", // magic codec order
					"?",
					"",
					{ 0x00,0x00,0x00,0x00,0x00,0x00 },	//mac
					"",		//resolution
					false,	// soft volume
#if defined(RESAMPLE)
					96000,
					true,
					"",
#else
					44100,
#endif
					{ "" },
				} ;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static log_level 			*loglevel = &main_loglevel;
#if LINUX || FREEBSD || SUNOS
static bool			 		glDaemonize = false;
#endif
static bool					glMainRunning = true;
static char					*glPidFile = NULL;
static bool					glAutoSaveConfigFile = false;
static bool					glGracefullShutdown = true;
static struct mdnssd_handle_s	*glmDNSsearchHandle = NULL;
static pthread_t 			glMainThread, glmDNSsearchThread;
static bool					glDiscovery = false;
static pthread_mutex_t		glMainMutex;
static pthread_cond_t		glMainCond;
static bool					glInteractive = true;
static char					*glLogFile;
static char 				glDACPid[] = "1A2B3D4EA1B2C3D4";
static struct mdnsd 		*gl_mDNSResponder;
static int					glActiveRemoteSock;
static pthread_t			glActiveRemoteThread;
static void					*glConfigID = NULL;
static char					glConfigName[STR_LEN] = "./config.xml";
static struct in_addr 		glHost;
static char					glModelName[STR_LEN] = MODEL_NAME_STRING;
static uint16_t				glPortBase, glPortRange;

static char usage[] =

			VERSION "\n"
		   "See -t for license terms\n"
		   "Usage: [options]\n"
		   "  -s <server>[:<port>]\tConnect to specified server, otherwise uses autodiscovery to find server\n"
		   "  -a <port>[:<count>]\tset inbound port base and range\n"
		   "  -b <address>]\tNetwork address to bind to\n"
		   "  -x <config file>\tread config from file (default is ./config.xml)\n"
		   "  -i <config file>\tdiscover players, save <config file> and exit\n"
   		   "  -m <name1,name2...>\texclude from search devices whose model name contains name1 or name 2 ...\n"
		   "  -I \t\t\tauto save config at every network scan\n"
		   "  -f <logfile>\t\tWrite debug to logfile\n"
  		   "  -p <pid file>\t\twrite PID in file\n"
		   "  -d <log>=<level>\tSet logging level, logs: all|slimproto|stream|decode|output|web|main|util|raop, level: error|warn|info|debug|sdebug\n"
		   "  -M <modelname>\tSet the squeezelite player model name sent to the server (default: " MODEL_NAME_STRING ")\n"
#if LINUX || FREEBSD || SUNOS
		   "  -z \t\t\tDaemonize\n"
#endif
		   "  -Z \t\t\tNOT interactive\n"
		   "  -k \t\t\tImmediate exit on SIGQUIT and SIGTERM\n"
		   "  -t \t\t\tLicense terms\n"
		   "\n"
		   "Build options:"
#if LINUX
		   " LINUX"
#endif
#if WIN
		   " WIN"
#endif
#if OSX
		   " OSX"
#endif
#if FREEBSD
		   " FREEBSD"
#endif
#if SUNOS
		   " SUNOS"
#endif
#if EVENTFD
		   " EVENTFD"
#endif
#if SELFPIPE
		   " SELFPIPE"
#endif
#if WINEVENT
		   " WINEVENT"
#endif
#if LOOPBACK
		   " LOOPBACK"
#endif
#if USE_SSL
		   " SSL"
#endif
		   "\n\n";
static char license[] =
		   "This program is free software: you can redistribute it and/or modify\n"
		   "it under the terms of the GNU General Public License as published by\n"
		   "the Free Software Foundation, either version 3 of the License, or\n"
		   "(at your option) any later version.\n\n"
		   "This program is distributed in the hope that it will be useful,\n"
		   "but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
		   "MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
		   "GNU General Public License for more details.\n\n"
		   "You should have received a copy of the GNU General Public License\n"
		   "along with this program.  If not, see <http://www.gnu.org/licenses/>.\n\n";
	#define SET_LOGLEVEL(log) 			  \
	if (!strcmp(resp, #log"dbg")) { \
		char level[20];           \
		i = scanf("%s", level);   \
		log ## _loglevel = debug2level(level); \
	}

/*----------------------------------------------------------------------------*/
/* prototypes */
/*----------------------------------------------------------------------------*/
static bool AddRaopDevice(struct sMR *Device, mdnssd_service_t *s);
static void DelRaopDevice(struct sMR *Device);
static bool IsExcluded(char *Model);

#if BUSY_MODE
static void BusyRaise(struct sMR *Device);
static void BusyDrop(struct sMR *Device);
#endif

/*----------------------------------------------------------------------------*/
bool sq_callback(void *caller, sq_action_t action, ...)
{
	struct sMR *device = caller;
	bool rc = true;

	pthread_mutex_lock(&device->Mutex);

	if (!device->Running)	{
		LOG_WARN("[%]: device has been removed", device);
		pthread_mutex_unlock(&device->Mutex);
		return false;
	}

	va_list args;
	va_start(args, action);

	if (action == SQ_ONOFF) {
		device->on = va_arg(args, int);

		if (device->on && device->Config.AutoPlay) {
			// switching player back ON let us take over 
			device->PlayerStatus = 0;
			sq_notify(device->SqueezeHandle, SQ_PLAY, device->on);
		}

		if (!device->on) {
			// flush everything in queue
			tRaopReq *Req = malloc(sizeof(tRaopReq));
			strcpy(Req->Type, "OFF");
			queue_flush(&device->Queue);
			queue_insert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
		}

		LOG_DEBUG("[%p]: device set on/off %d", caller, device->on);
	}

	if (!device->on && action != SQ_SETNAME && action != SQ_SETSERVER) {
		LOG_DEBUG("[%p]: device off or not controlled by LMS", caller);
		pthread_mutex_unlock(&device->Mutex);
		va_end(args);
		return false;
	}

	LOG_SDEBUG("callback for %s (%d)", device->FriendlyName, action);

	switch (action) {
		case SQ_FINISHED:
			device->LastFlush = gettime_ms();
			device->DiscWait = true;
			device->TrackRunning = false;
			break;
		case SQ_STOP: {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			device->TrackRunning = false;
			device->sqState = SQ_STOP;
			// see note in raop_client.h why this 2-stages stop is needed
			raopcl_stop(device->Raop);

			strcpy(Req->Type, "FLUSH");
			queue_insert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			break;
		}
		case SQ_PAUSE: {
			tRaopReq *Req;

			device->TrackRunning = false;
			device->sqState = SQ_PAUSE;
			// see note in raop_client.h why this 2-stages pause is needed
			raopcl_pause(device->Raop);

			Req = malloc(sizeof(tRaopReq));
			strcpy(Req->Type, "FLUSH");
			queue_insert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			break;
		}
		case SQ_UNPAUSE: {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			device->TrackRunning = true;
			device->sqState = SQ_PLAY;
			unsigned jiffies = va_arg(args, unsigned);
			if (jiffies) 
				raopcl_start_at(device->Raop, TIME_MS2NTP(jiffies) -
								TS2NTP(raopcl_latency(device->Raop), raopcl_sample_rate(device->Raop)));
			strcpy(Req->Type, "CONNECT");
			queue_insert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);
			break;
		}
		case SQ_VOLUME: {
			uint32_t Volume = LMSVolumeMap[va_arg(args, int)];
			uint32_t now = gettime_ms();

			if (device->Config.VolumeMode == VOLUME_HARD &&	now > device->VolumeStampRx + 1000 &&
				(Volume || device->Config.MuteOnPause || sq_get_mode(device->SqueezeHandle) == device->sqState)) {
				
				device->Volume = Volume;
				
				tRaopReq *Req = malloc(sizeof(tRaopReq));
				Req->Data.Volume = device->VolumeMapping[(unsigned)device->Volume];
				strcpy(Req->Type, "VOLUME");
				queue_insert(&device->Queue, Req);
				pthread_cond_signal(&device->Cond);
			} else {
				LOG_INFO("[%p]: volume ignored %u", device, Volume);
			}

			break;
		}
		case SQ_CONNECT: {
			tRaopReq *Req = malloc(sizeof(tRaopReq));

			device->sqState = SQ_PLAY;

			strcpy(Req->Type, "CONNECT");
			queue_insert(&device->Queue, Req);
			pthread_cond_signal(&device->Cond);

			break;
		}
		case SQ_METASEND:
			device->MetadataWait = 5;
			break;
		case SQ_STARTED:
			device->TrackRunning = true;
			device->MetadataWait = 2;
			device->MetadataHash++;
			break;
		case SQ_SETNAME:
			strcpy(device->sq_config.name, va_arg(args, char*));
			break;
		case SQ_SETSERVER:
			strcpy(device->sq_config.dynamic.server, inet_ntoa(*va_arg(args, struct in_addr*)));
			break;
		default:
			break;
	}

	pthread_mutex_unlock(&device->Mutex);
	va_end(args);
	return rc;
}

/*----------------------------------------------------------------------------*/
static void* GetArtworkThread(void *arg) {
	union sRaopReqData* Data = &((tRaopReq*) arg)->Data;
	struct sMR* Device = Data->Artwork.Device;

	// try to get the image, might take a while
	Data->Artwork.Size = http_fetch(Data->Artwork.Url, &Data->Artwork.ContentType, &Data->Artwork.Image);
	pthread_mutex_lock(&Device->Mutex);

	// need to make sure we have artwork, that device is active and this si the right query
	if (Data->Artwork.Size > 0 && Device->Running && Device->MetadataHash == Data->Artwork.Hash) {
		queue_insert(&Device->Queue, arg);
		pthread_cond_signal(&Device->Cond);
	} else {
		LOG_WARN("[%p]: Can't get artwork or device not active %s", Device, Data->Artwork.Url);
		NFREE(Data->Artwork.ContentType);
		NFREE(Data->Artwork.Image);
		free(Data->Artwork.Url);
		free(arg);
	}

	pthread_mutex_unlock(&Device->Mutex);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void* GetRequest(cross_queue_t* Queue, pthread_mutex_t* Mutex, pthread_cond_t* Cond, uint32_t timeout) {
	pthread_mutex_lock(Mutex);

	void* data = queue_extract(Queue);
	if (!data) pthread_cond_reltimedwait(Cond, Mutex, timeout);

	pthread_mutex_unlock(Mutex);
	return data;
}

/*----------------------------------------------------------------------------*/
static void *PlayerThread(void *args) {
	struct sMR *Device = (struct sMR*) args;
	uint32_t KeepAlive = 0, Last = 0;

	Device->Running = true;

	/*
	There is probably a few unsafe thread conditions with the callback and the
	the activethread, but nothing serious and locking the mutex during the whole
	time would seriously block the callback, so it's not worth
	*/

	while (Device->Running) {
		// context is valid until this thread ends, no deletion issue
		tRaopReq *req = GetRequest(&Device->Queue, &Device->Mutex, &Device->Cond, 1000);
		uint32_t now = gettime_ms();

		// player is not ready to receive commands
		if (Device->PlayerStatus & DMCP_PREVENT_PLAYBACK) {
			pthread_mutex_lock(&Device->Mutex);

			if (now > Last + 5000) {
				LOG_WARN("[%p]: Player has been in 'PreventPlayback' for too long");
				sq_notify(Device->SqueezeHandle, SQ_OFF);
			} else if (req) {
				queue_insert_first(&Device->Queue, req);
			}

			pthread_mutex_unlock(&Device->Mutex);
			LOG_DEBUG("[%p]: PreventPlayback loop", Device);
			usleep(50 * 1000);

			continue;
		}
			
		Last = now;

		// empty means timeout every sec
		if (!req) {
			LOG_DEBUG("[%p]: tick %u", Device, now);

			if (Device->DiscWait && (Device->LastFlush + (Device->Config.IdleTimeout * 1000) - now > 1000) ) {
				LOG_INFO("[%p]: Disconnecting %u", Device, now);
				raopcl_disconnect(Device->Raop);
				Device->DiscWait = false;
			}

			Device->Sane = raopcl_is_sane(Device->Raop) ? 0 : Device->Sane + 1;
			if (Device->Sane > 3) {
				LOG_WARN("[%p]: broken connection: switching off player", Device);
				pthread_mutex_lock(&Device->Mutex);
				sq_notify(Device->SqueezeHandle, SQ_OFF);
				pthread_mutex_unlock(&Device->Mutex);
			}

			// after that, only check what's needed when running
			if (!Device->TrackRunning) continue;

			// seems that HomePod requires regular RTSP exchange
			if (!(KeepAlive++ & 0x0f)) raopcl_keepalive(Device->Raop);

			pthread_mutex_lock(&Device->Mutex);

			if (Device->MetadataWait && !--Device->MetadataWait && Device->Config.SendMetaData) {
				metadata_t metadata;
				uint32_t hash, Time;

				pthread_mutex_unlock(&Device->Mutex);

				// not a valid metadata, nothing to update
				if (!sq_get_metadata(Device->SqueezeHandle, &metadata, false)) {
					Device->MetadataWait = 5;
					metadata_free(&metadata);
					continue;
				}

				// set progress at every metadata check (for live streams)
				Time = sq_get_time(Device->SqueezeHandle);
				raopcl_set_progress_ms(Device->Raop, Time, metadata.duration);

				hash = hash32(metadata.title) ^ hash32(metadata.artwork);

				if (Device->MetadataHash != hash) {
					raopcl_set_daap(Device->Raop, 5, "minm", 's', metadata.title,
													 "asar", 's', metadata.artist,
													 "asal", 's', metadata.album,
													 "asgn", 's', metadata.genre,
													 "astn", 'i', (int) metadata.track);

					Device->MetadataHash = hash;

					// only get coverart if title has changed
					if (metadata.artwork && Device->Config.SendCoverArt) {
						tRaopReq* Req = calloc(1, sizeof(tRaopReq));
						strcpy(Req->Type, "ARTWORK");
						Req->Data.Artwork.Url = strdup(metadata.artwork);
						Req->Data.Artwork.Device = Device;
						Req->Data.Artwork.Hash = hash;

						pthread_t lambda;
						pthread_create(&lambda, NULL, &GetArtworkThread, Req);
					}

					/*
					Set refresh rate to 5 sec for true live streams and song
					duration + 5s for others that might be either live but with
					real duration from plugin helpers or streaming services
					*/
					if (metadata.remote) {
						Device->MetadataWait = 5;
						if (metadata.duration) {
							Device->MetadataWait += (metadata.duration - Time) / 1000;
						}
					}

					LOG_INFO("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n"
								"\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s",
								 Device, metadata.index, metadata.artist,
								 metadata.album, metadata.title, metadata.genre,
								 div(metadata.duration, 1000).quot,
								 div(metadata.duration,1000).rem, metadata.size,
								 metadata.artwork ? metadata.artwork : "");


				} else Device->MetadataWait = 5;

				metadata_free(&metadata);
				LOG_DEBUG("[%p]: next metadata update %u", Device, Device->MetadataWait);

			} else pthread_mutex_unlock(&Device->Mutex);

			continue;
		}

		if (!strcasecmp(req->Type, "CONNECT")) {
			LOG_INFO("[%p]: raop connecting ...", Device);
			if (raopcl_connect(Device->Raop, Device->PlayerIP, Device->PlayerPort, Device->Config.Volume != -1)) {
				Device->DiscWait = false;
				LOG_INFO("[%p]: raop connected", Device);
			} else {
				LOG_ERROR("[%p]: raop failed to connect", Device);
			}
		}

		if (!strcasecmp(req->Type, "FLUSH")) {
			LOG_INFO("[%p]: flushing ...", Device);
			Device->LastFlush = gettime_ms();
			Device->DiscWait = true;
			raopcl_flush(Device->Raop);
		}

		if (!strcasecmp(req->Type, "OFF")) {
			LOG_INFO("[%p]: processing off", Device);
			raopcl_disconnect(Device->Raop);
			raopcl_sanitize(Device->Raop);
		}

		if (!strcasecmp(req->Type, "VOLUME")) {
			LOG_INFO("[%p]: processing volume device:%d request:%.2f", Device, Device->Volume, req->Data.Volume);
			raopcl_set_volume(Device->Raop, req->Data.Volume);
		}

		if (!strcasecmp(req->Type, "ARTWORK")) {
			union sRaopReqData* Data = &((tRaopReq*)req)->Data;
			LOG_INFO("[%p]: Got artwork for %s", Device, Data->Artwork.Url);

			// need to make sure this is *really* for us
			if (Device->MetadataHash == Data->Artwork.Hash) {
				raopcl_set_artwork(Device->Raop, Data->Artwork.ContentType, Data->Artwork.Size, Data->Artwork.Image);
			} else {
				LOG_WARN("[%p]: Wrong artwork", Device);
			}

			NFREE(Data->Artwork.ContentType);
			NFREE(Data->Artwork.Image);
			free(Data->Artwork.Url);
		}

		free(req);
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
char *GetmDNSAttribute(mdnssd_txt_attr_t *p, int count, char *name) {
	for (int i = 0; i < count; i++)	if (!strcasecmp(p[i].name, name))return strdup(p[i].value);
	return NULL;
}

/*----------------------------------------------------------------------------*/
struct sMR *SearchUDN(char *UDN) {
	for (int i = 0; i < MAX_RENDERERS; i++) if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))	return glMRDevices + i;
	return NULL;
}

/*----------------------------------------------------------------------------*/
bool mDNSsearchCallback(mdnssd_service_t *slist, void *cookie, bool *stop) {
	struct sMR *Device;
	mdnssd_service_t *s;
	uint32_t now = gettime_ms();

	for (s = slist; s && glMainRunning; s = s->next) {
		char *am = GetmDNSAttribute(s->attr, s->attr_count, "am");
		bool excluded = am ? IsExcluded(am) : false;
		NFREE(am);

		// ignore excluded and announces made on behalf
		if (!s->name || excluded || (s->host.s_addr != s->addr.s_addr && ((s->host.s_addr & glMask) == (s->addr.s_addr & glMask)))) continue;

		// is that device already here
		if ((Device = SearchUDN(s->name)) != NULL) {
			Device->Expired = 0;
			// device disconnected
			if (s->expired) {
				if (!raopcl_is_connected(Device->Raop) && !Device->Config.RemoveTimeout) {
					LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
					if (Device->SqueezeHandle) sq_delete_device(Device->SqueezeHandle);
					DelRaopDevice(Device);
				} else {
					LOG_INFO("[%p]: keep missing renderer (%s)", Device, Device->FriendlyName);
					Device->Expired = now ? now : 1;
				}
			// device update - ignore changes in TXT
			} else if (s->port != Device->PlayerPort || s->addr.s_addr != Device->PlayerIP.s_addr) {
				LOG_INFO("[%p]: changed ip:port %s:%d", Device, inet_ntoa(s->addr), s->port);
				Device->PlayerPort = s->port;
				Device->PlayerIP = s->addr;

				// replace ip:port piece of credentials
				if (*Device->Config.Credentials) {
					char *token = strchr(Device->Config.Credentials, '@');
					if (token) *token = '\0';
					sprintf(Device->Config.Credentials + strlen(Device->Config.Credentials), "@%s:%d", inet_ntoa(s->addr), s->port);
				}
			}
			continue;
		}

		// disconnect of an unknown device
		if (!s->port && !s->addr.s_addr) {
			LOG_ERROR("Unknown device disconnected %s", s->name);
			continue;
		}
		// should not happen
		if (s->expired) {
			LOG_DEBUG("Device already expired %s", s->name);
			continue;
		}

		// device creation so search a free spot.
		for (Device = glMRDevices; Device->Running && Device < glMRDevices + MAX_RENDERERS; Device++);

		// no more room !
		if (Device == glMRDevices + MAX_RENDERERS) {
			LOG_ERROR("Too many devices (max:%u)", MAX_RENDERERS);
			break;
		}

		if (AddRaopDevice(Device, s) && !glDiscovery) {
			// create a new slimdevice
			Device->sq_config.soft_volume = (Device->Config.VolumeMode == VOLUME_SOFT);
			Device->SqueezeHandle = sq_reserve_device(Device, &sq_callback);
			if (!*(Device->sq_config.name)) strcpy(Device->sq_config.name, Device->FriendlyName);
			if (!Device->SqueezeHandle || !sq_run_device(Device->SqueezeHandle,
														 Device->Raop, &Device->sq_config)) {
				sq_release_device(Device->SqueezeHandle);
				Device->SqueezeHandle = 0;
				LOG_ERROR("[%p]: cannot create squeezelite instance (%s)", Device, Device->FriendlyName);
				DelRaopDevice(Device);
			}
		}
	}

	// walk through the list for device whose timeout expired
	for (int i = 0; i < MAX_RENDERERS; i++) {
		Device = glMRDevices + i;
		if (!Device->Running || Device->Config.RemoveTimeout <= 0 || !Device->Expired || now < Device->Expired + Device->Config.RemoveTimeout*1000) continue;

		LOG_INFO("[%p]: removing renderer (%s) on timeout", Device, Device->FriendlyName);
		if (Device->SqueezeHandle) sq_delete_device(Device->SqueezeHandle);
		DelRaopDevice(Device);
	}

	if (glAutoSaveConfigFile || glDiscovery) {
		LOG_DEBUG("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, false);
	}

	// we have not released the slist
	return false;
}


/*----------------------------------------------------------------------------*/
static void *mDNSsearchThread(void *args) {
	// launch the query,
	mdnssd_query(glmDNSsearchHandle, "_raop._tcp.local", false,
			   glDiscovery ? DISCOVERY_TIME : 0, &mDNSsearchCallback, NULL);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void *MainThread(void *args) {
	while (glMainRunning) {
		pthread_mutex_lock(&glMainMutex);
		pthread_cond_reltimedwait(&glMainCond, &glMainMutex, 30*1000);
		pthread_mutex_unlock(&glMainMutex);

		if (glLogFile && glLogLimit != - 1) {
			int32_t size = ftell(stderr);
			if (size > glLogLimit*1024*1024) {
				uint32_t Sum, BufSize = 16384;
				uint8_t *buf = malloc(BufSize);
				FILE *rlog = fopen(glLogFile, "rb");
				FILE *wlog = fopen(glLogFile, "r+b");
				LOG_DEBUG("Resizing log", NULL);
				for (Sum = 0, fseek(rlog, size - (glLogLimit*1024*1024) / 2, SEEK_SET);
					 (BufSize = fread(buf, 1, BufSize, rlog)) != 0;
					 Sum += BufSize, fwrite(buf, 1, BufSize, wlog));

				Sum = fresize(wlog, Sum);
				fclose(wlog);
				fclose(rlog);
				NFREE(buf);
				if (!freopen(glLogFile, "a", stderr)) {
					LOG_ERROR("re-open error while truncating log", NULL);
				}
			}
		}
	}

	return NULL;
}

/*----------------------------------------------------------------------------*/
void SetVolumeMapping(struct sMR *Device) {
	char *p;
	int i = 1;
	float a1 = 1, b1 = -30, a2 = 0, b2 = 0;
	Device->VolumeMapping[0] = -144.0;
	p = Device->Config.VolumeMapping;
	do {
		if (!p || !sscanf(p, "%f:%f", &b2, &a2)) {
			LOG_ERROR("[%p]: wrong volume mapping table", Device, p);
			break;
		}
		p = strchr(p, ',');
		if (p) p++;

		while (i <= a2) {
			Device->VolumeMapping[i] = (a1 == a2) ? b1 :
									   i*(b1-b2)/(a1-a2) + b1 - a1*(b1-b2)/(a1-a2);
			i++;
		}
		a1 = a2;
		b1 = b2;
	} while (i <= 100);
	for (; i <= 100; i++) Device->VolumeMapping[i] = Device->VolumeMapping[i-1];
}

/*----------------------------------------------------------------------------*/
static void RaopQueueFree(void* item) {
	tRaopReq* p = item;
	if (strcasestr(p->Type, "ARTWORK")) {
		NFREE(p->Data.Artwork.ContentType);
		NFREE(p->Data.Artwork.Image);
		free(p->Data.Artwork.Url);
	}
	free(p);
}

/*----------------------------------------------------------------------------*/
static bool AddRaopDevice(struct sMR *Device, mdnssd_service_t *s) {
	pthread_attr_t pattr;
	raop_crypto_t Crypto;
	bool Auth = false;
	char *p, *am, *md, *pk;
	char Secret[STR_LEN] = "";
	// read parameters from default then config file
	memcpy(&Device->Config, &glMRConfig, sizeof(tMRConfig));
	memcpy(&Device->sq_config, &glDeviceParam, sizeof(sq_dev_param_t));
	LoadMRConfig(glConfigID, s->name, &Device->Config, &Device->sq_config);

	if (!Device->Config.Enabled) return false;

	if (strcasestr(s->name, "AirSonos")) {
		LOG_DEBUG("[%p]: skipping AirSonos player (please use uPnPBridge)", Device);
		return false;
	}

	am = GetmDNSAttribute(s->attr, s->attr_count, "am");
	pk = GetmDNSAttribute(s->attr, s->attr_count, "pk");
	md = GetmDNSAttribute(s->attr, s->attr_count, "md");

	// if airport express, force auth
	if (am && strcasestr(am, "airport")) {
		LOG_INFO("[%p]: AirPort Express", Device);
		Auth = true;
	}

	if (am && strcasestr(am, "appletv") && pk && *pk) {
		char *token = strchr(Device->Config.Credentials, '@');
		LOG_INFO("[%p]: AppleTV with authentication (pairing must be done separately)", Device);
		if (Device->Config.Credentials[0]) sscanf(Device->Config.Credentials, "%[a-fA-F0-9]", Secret);
		if (token) *token = '\0';
		sprintf(Device->Config.Credentials + strlen(Device->Config.Credentials), "@%s:%d", inet_ntoa(s->addr), s->port);
	}

	Device->Magic 			= MAGIC;
	Device->on 				= false;
	Device->SqueezeHandle 	= 0;
	Device->Running 		= true;
	// make sure that 1st volume is not missed
	Device->VolumeStampRx 	= gettime_ms() - 2000;
	Device->PlayerIP 		= s->addr;
	Device->PlayerPort 		= s->port;
	Device->PlayerStatus	= 0;
	Device->DiscWait 		= false;
	Device->TrackRunning 	= false;
	Device->Volume 			= Device->Config.Volume;
	Device->SkipStart 		= 0;
	Device->SkipDir 		= false;
	Device->ContentType[0] 	= '\0';
	Device->sqState 		= SQ_STOP;
	Device->Raop 			= NULL;
	Device->LastFlush 		= 0;
	Device->Expired			= 0;
	Device->Sane 			= true;
	Device->MetadataWait 	= Device->MetadataHash = 0;
	Device->Busy			= 0;
	Device->Delete			= 0;

	memset(Device->ActiveRemote, 0, 16);
	SetVolumeMapping(Device);

	strcpy(Device->UDN, s->name);
	sprintf(Device->ActiveRemote, "%u", hash32(Device->UDN));

	strcpy(Device->FriendlyName, s->hostname);
	p = strcasestr(Device->FriendlyName, ".local");
	if (p) *p = '\0';

	if (!memcmp(Device->sq_config.mac, "\0\0\0\0\0\0", 6)) {
		uint32_t mac_size = 6;
		if (SendARP(s->addr.s_addr, INADDR_ANY, Device->sq_config.mac, &mac_size)) {
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: creating MAC", Device);
		}
		memset(Device->sq_config.mac, 0xaa, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(glMRDevices[i].sq_config.mac, Device->sq_config.mac, 6)) {
			memset(Device->sq_config.mac, 0xaa, 2);
			*(uint32_t*)(Device->sq_config.mac + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	LOG_INFO("[%p]: adding renderer (%s@%s) with mac %hX-%X", Device, Device->FriendlyName, inet_ntoa(Device->PlayerIP),  
	         *(uint16_t*)Device->sq_config.mac, *(uint32_t*)(Device->sq_config.mac + 2));

	// gather RAOP device capabilities, to be matched mater
	Device->SampleSize = GetmDNSAttribute(s->attr, s->attr_count, "ss");
	Device->SampleRate = GetmDNSAttribute(s->attr, s->attr_count, "sr");
	Device->Channels = GetmDNSAttribute(s->attr, s->attr_count, "ch");
	Device->Codecs = GetmDNSAttribute(s->attr, s->attr_count, "cn");
	Device->Crypto = GetmDNSAttribute(s->attr, s->attr_count, "et");

	if (!Device->Codecs || !strchr(Device->Codecs, '1')) {
		LOG_WARN("[%p]: ALAC not in codecs, player might not work %s", Device, Device->Codecs);
	}

	if ((Device->Config.Encryption || Auth) && strchr(Device->Crypto, '1'))	Crypto = RAOP_RSA;
	else Crypto = RAOP_CLEAR;

	Device->Raop = raopcl_create(glHost, glPortBase, glPortRange,
								 glDACPid, Device->ActiveRemote,
								 Device->Config.AlacEncode ? RAOP_ALAC : RAOP_ALAC_RAW , FRAMES_PER_BLOCK,
								 (uint32_t) MS2TS(Device->Config.ReadAhead, Device->SampleRate ? atoi(Device->SampleRate) : 44100),
								 Crypto, Auth, Secret, Device->Crypto, md,
								 Device->SampleRate ? atoi(Device->SampleRate) : 44100,
								 Device->SampleSize ? atoi(Device->SampleSize) : 16,
								 Device->Channels ? atoi(Device->Channels) : 2,
								 Device->Volume > 0 ? Device->VolumeMapping[(unsigned) Device->Volume] : -144.0);

	NFREE(am);
	NFREE(md);
	NFREE(pk);

	if (!Device->Raop) {
		LOG_ERROR("[%p]: cannot create raop device", Device);
		NFREE(Device->SampleSize);
		NFREE(Device->SampleRate);
		NFREE(Device->Channels);
		NFREE(Device->Codecs);
		NFREE(Device->Crypto);
		return false;
	}

	pthread_attr_init(&pattr);
	pthread_attr_setstacksize(&pattr, PTHREAD_STACK_MIN + 64*1024);
	pthread_create(&Device->Thread, NULL, &PlayerThread, Device);
	pthread_attr_destroy(&pattr);

	return true;
}

/*----------------------------------------------------------------------------*/
void FlushRaopDevices(void) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) DelRaopDevice(p);
	}
}

/*----------------------------------------------------------------------------*/
void DelRaopDevice(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	pthread_cond_signal(&Device->Cond);
	pthread_mutex_unlock(&Device->Mutex);
	pthread_join(Device->Thread, NULL);
	raopcl_destroy(Device->Raop);
	queue_flush(&Device->Queue);

	LOG_INFO("[%p]: Raop device stopped", Device);

	NFREE(Device->SampleSize);
	NFREE(Device->SampleRate);
	NFREE(Device->Channels);
	NFREE(Device->Codecs);
	NFREE(Device->Crypto);
}

/*----------------------------------------------------------------------------*/
static void *ActiveRemoteThread(void *args) {
	char buf[1024], command[128], ActiveRemote[16];
	char response[] = "HTTP/1.0 204 No Content\r\nDate: %s,%02d %s %4d %02d:%02d:%02d "
					  "GMT\r\nDAAP-Server: iTunes/7.6.2 (Windows; N;)\r\nContent-Type: "
					  "application/x-dmap-tagged\r\nContent-Length: 0\r\n"
					  "Connection: close\r\n\r\n";
	char *day[] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
	char *month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sept", "Oct", "Nov", "Dec" };

	if (listen(glActiveRemoteSock, 1) < 0) {
		LOG_ERROR("Cannot listen %d", glActiveRemoteSock);
		return NULL;
	}

	while (glMainRunning) {
		int sd, i;
		struct sockaddr cli_addr;
		socklen_t clilen = sizeof(cli_addr);
		struct sMR *Device = NULL;
		char *p;
		time_t now = time(NULL);
		struct tm gmt;
		sd = accept(glActiveRemoteSock, (struct sockaddr *) &cli_addr, &clilen);

		if (!glMainRunning) break;
		
		if (sd < 0) {
			LOG_WARN("Accept error", NULL);
			continue;
		}

		// receive data, all should be in a single receive, hopefully
		ssize_t n = recv(sd, (void*) buf, sizeof(buf) - 1, 0);

		if (n < 0) {
			LOG_WARN("error receving remote control data");
			closesocket(sd);
			continue;
		}

		buf[n] = '\0';
		strlwr(buf);
		LOG_INFO("raw active remote: %s", buf);

		// a pretty basic reading of command
		p = strstr(buf, "active-remote:");
		if (p) sscanf(p, "active-remote:%15s", ActiveRemote);
		ActiveRemote[sizeof(ActiveRemote) - 1] = '\0';
		p = strstr(buf, "/ctrl-int/1/");
		if (p) sscanf(p, "/ctrl-int/1/%127s", command);
		command[sizeof(command) - 1] = '\0';

		// find where this is coming from
		for (i = 0; i < MAX_RENDERERS; i++) {
			if (glMRDevices[i].Running && !strcmp(glMRDevices[i].ActiveRemote, ActiveRemote)) {
				Device = &glMRDevices[i];
				break;
			}
		}

		if (!Device) {
			LOG_WARN("DACP from unknown player %s", buf);
			closesocket(sd);
			continue;
		}

		// this is async, so need to check context validity
		pthread_mutex_lock(&Device->Mutex);

		if (!Device->Running) {
			LOG_WARN("[%p]: device has been removed", Device);
			pthread_mutex_unlock(&Device->Mutex);
			closesocket(sd);
			continue;
		}

		LOG_INFO("[%p]: remote command %s", Device, command);

		if (!strcasecmp(command, "pause")) sq_notify(Device->SqueezeHandle, SQ_PAUSE);
		if (!strcasecmp(command, "play")) sq_notify(Device->SqueezeHandle, SQ_PLAY);
		if (!strcasecmp(command, "playpause")) sq_notify(Device->SqueezeHandle, SQ_PLAY_PAUSE);
		if (!strcasecmp(command, "stop")) sq_notify(Device->SqueezeHandle, SQ_STOP);
		if (!strcasecmp(command, "mutetoggle")) sq_notify(Device->SqueezeHandle, SQ_MUTE_TOGGLE);
		if (!strcasecmp(command, "nextitem")) sq_notify(Device->SqueezeHandle, SQ_NEXT);
		if (!strcasecmp(command, "previtem")) sq_notify(Device->SqueezeHandle, SQ_PREVIOUS);
		if (!strcasecmp(command, "volumeup")) sq_notify(Device->SqueezeHandle, SQ_VOLUME, "up");
		if (!strcasecmp(command, "volumedown")) sq_notify(Device->SqueezeHandle, SQ_VOLUME, "down");
		if (!strcasecmp(command, "shuffle_songs")) sq_notify(Device->SqueezeHandle, SQ_SHUFFLE);
		if (!strcasecmp(command, "beginff") || !strcasecmp(command, "beginrew")) {
			Device->SkipStart = gettime_ms();
			Device->SkipDir = !strcasecmp(command, "beginff") ? true : false;
		}
		if (!strcasecmp(command, "playresume")) {
			int32_t gap = gettime_ms() - Device->SkipStart;
			gap = (gap + 3) * (gap + 3) * (Device->SkipDir ? 1 : -1);
			sq_notify(Device->SqueezeHandle, SQ_FF_REW, gap);
		}

		// handle DMCP commands
		if (strcasestr(command, "setproperty?dmcp")) {

			// player can't handle requests or switched to another input
			if (strcasestr(command, "device-prevent-playback=1")) {
				Device->PlayerStatus |= DMCP_PREVENT_PLAYBACK;
			} else if (strcasestr(command, "device-prevent-playback=0")) {
				Device->PlayerStatus &= ~DMCP_PREVENT_PLAYBACK;
			} else if (strcasestr(command, "device-busy=1")) {
				Device->PlayerStatus |= DMCP_BUSY;
				if (Device->PlayerStatus & DMCP_PREVENT_PLAYBACK) {
					sq_notify(Device->SqueezeHandle, SQ_OFF);
				}
			} else if (strcasestr(command, "device-busy=0")) {
				Device->PlayerStatus &= ~DMCP_BUSY;
			}

			/* Volume remote command in 2 formats
			 *	- setproperty?dmcp.volume=0..100
			 *	- setproperty?dmcp.device-volume=-30..0 (or -144) 
			 */
			if ((strcasestr(command, "device-volume=") || strcasestr(command, ".volume=")) &&
				Device->Config.VolumeMode != VOLUME_SOFT && Device->Config.VolumeFeedback) {
				float volume;
				int i;
				uint32_t now = gettime_ms();

				sscanf(command, "%*[^=]=%f", &volume);
				if (strcasestr(command, ".volume=")) i = (int) volume;
				else for (i = 0; i < 100 && volume > Device->VolumeMapping[i]; i++);
				volume = Device->VolumeMapping[i];

				LOG_INFO("[%p]: volume feedback %u (%.2f)", Device, i, volume);

				if (i != Device->Volume) {
					// in case of change, notify LMS (but we should ignore command)
					char vol[10];
					sprintf(vol, "%d", i);
					Device->VolumeStampRx = now;
					sq_notify(Device->SqueezeHandle, SQ_VOLUME, vol);

					// some players expect controller to update volume, it's a request not a notification	
					tRaopReq* Req = malloc(sizeof(tRaopReq));
					Req->Data.Volume = volume;
					strcpy(Req->Type, "VOLUME");
					queue_insert(&Device->Queue, Req);
					pthread_cond_signal(&Device->Cond);
				} 
			}
		}

		// send pre-made response
		gmt = *gmtime(&now);
		sprintf(buf, response, day[gmt.tm_wday], gmt.tm_mday, month[gmt.tm_mon],
								gmt.tm_year + 1900, gmt.tm_hour, gmt.tm_min, gmt.tm_sec);
		send(sd, buf, strlen(buf), 0);
		closesocket(sd);

		// don't free the mutex before answer has been given, in case a pthread_cond is emitted
		pthread_mutex_unlock(&Device->Mutex);
	}

	return NULL;
}
/*----------------------------------------------------------------------------*/
void StartActiveRemote(struct in_addr host) {
	struct sockaddr_in addr;
	socklen_t nlen = sizeof(struct sockaddr);
	const char *txt[] = {
		"txtvers=1",
		"Ver=131075",
		"DbId=63B5E5C0C201542E",
		"OSsi=0x1F5",
		NULL
	};
	struct {
		uint16_t count, range;
		uint16_t offset;
	} aport = { 0 };
	aport.range = glPortBase ? glPortRange : 1;
	aport.offset = rand() % aport.range;
	do {
		if ((glActiveRemoteSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			LOG_ERROR("Cannot create ActiveRemote socket", NULL);
			return;
		}
		memset(&addr, 0, sizeof(addr));
		addr.sin_addr.s_addr = host.s_addr;
		addr.sin_family = AF_INET;
		addr.sin_port = htons(glPortBase + ((aport.offset + aport.count++) % aport.range));
		if (bind(glActiveRemoteSock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
			closesocket(glActiveRemoteSock);
			glActiveRemoteSock = -1;
		}
	} while (glActiveRemoteSock < 0 && aport.count < aport.range);
	if (glActiveRemoteSock < 0) {
		LOG_ERROR("Cannot bind ActiveRemote: %s", strerror(errno));
		return;
	}
	getsockname(glActiveRemoteSock, (struct sockaddr *) &addr, &nlen);
	uint16_t port = ntohs(addr.sin_port);
	LOG_INFO("DACP port: %d", port);

	// start mDNS responder
	if ((gl_mDNSResponder = mdnsd_start(host, false)) == NULL) {
		LOG_ERROR("mdnsd responder start error", NULL);
		return;
	}

	char buf[STR_LEN];
	
	// set hostname
	gethostname(buf, sizeof(buf));
	strcat(buf, ".local");
	mdnsd_set_hostname(gl_mDNSResponder, buf, host);

	// register service
	snprintf(buf, sizeof(buf), "iTunes_Ctrl_%s", glDACPid);
	struct mdns_service* svc = mdnsd_register_svc(gl_mDNSResponder, buf, "_dacp._tcp.local", port, NULL, txt);
	mdns_service_destroy(svc);

	// start ActiveRemote answering thread
	pthread_create(&glActiveRemoteThread, NULL, ActiveRemoteThread, NULL);
}

/*/
void getNetMask(int sock, char* iface) {
	struct ifreq ifr;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, iface, IF_NAMESIZE);

	if ((ioctl(rsock, SIOCGIFNETMASK, &ifr)) == -1) {
		perror("ioctl():");
		return -1;
	}
}
*/

/*----------------------------------------------------------------------------*/
void StopActiveRemote(void) {
	if (glActiveRemoteSock != -1) {
#if WIN
		shutdown(glActiveRemoteSock, SD_BOTH);
#else
#if FREEBSD
		// wake-up thread by connecting socket, needed for freeBSD
		struct sockaddr addr;
		socklen_t nlen = sizeof(struct sockaddr);
		int sock = socket(AF_INET, SOCK_STREAM, 0);
		getsockname(glActiveRemoteSock, (struct sockaddr *) &addr, &nlen);
		connect(sock, (struct sockaddr*) &addr, sizeof(addr));
		closesocket(sock);
#endif
		shutdown(glActiveRemoteSock, SHUT_RDWR);
#endif
		closesocket(glActiveRemoteSock);
	}
	pthread_join(glActiveRemoteThread, NULL);
	mdnsd_stop(gl_mDNSResponder);
}

/*----------------------------------------------------------------------------*/
static bool IsExcluded(char *Model) {
	char item[STR_LEN];
	char *p = glExcluded;
	do {
		sscanf(p, "%[^,]", item);
		if (strcasestr(Model, item)) return true;
		p += strlen(item);
	} while (*p++);
	return false;
}

#if BUSY_MODE
/*----------------------------------------------------------------------------*/
void BusyRaise(struct sMR *Device) {
	LOG_DEBUG("[%p]: busy raise %u", Device, Device->Busy);
	Device->Busy++;
	pthread_mutex_unlock(&Device->Mutex);
}

/*----------------------------------------------------------------------------*/
void BusyDrop(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Busy--;
	if (!Device->Busy && Device->Delete) pthread_cond_signal(&Device->Cond);
	LOG_DEBUG("[%p]: busy drop %u", Device, Device->Busy);
	pthread_mutex_unlock(&Device->Mutex);
}

#endif

/*----------------------------------------------------------------------------*/
static bool Start(void) {
	int i;

	if (!cross_ssl_load()) {
		LOG_ERROR("Cannot load SSL libraries", NULL);
		return false;
	}

#if USE_SSL
	SSL_library_init();
#endif

	// must bind to an address
	glHost = get_interface(!strchr(glInterface, '?') ? glInterface : NULL, NULL, &glMask);
	if (glHost.s_addr == INADDR_NONE) return false;

	memset(&glMRDevices, 0, sizeof(glMRDevices));

	pthread_mutex_init(&glMainMutex, 0);
	pthread_cond_init(&glMainCond, 0);

    for (i = 0; i < MAX_RENDERERS;  i++) {
		pthread_mutex_init(&glMRDevices[i].Mutex, 0);
		pthread_cond_init(&glMRDevices[i].Cond, 0);
		queue_init(&glMRDevices[i].Queue, false, RaopQueueFree);
	}

	sq_init(glHost, glModelName);

	LOG_INFO("Binding to %s", inet_ntoa(glHost));

	/* start the mDNS devices discovery thread */
	if ((glmDNSsearchHandle = mdnssd_init(false, glHost, true)) == NULL) {;
		LOG_ERROR("Cannot start mDNS searcher", NULL);
		return false;
	}

	pthread_create(&glmDNSsearchThread, NULL, &mDNSsearchThread, NULL);

	// Start the ActiveRemote server
	StartActiveRemote(glHost);

	/* start the main thread */
	pthread_create(&glMainThread, NULL, &MainThread, NULL);

	return true;
}

/*---------------------------------------------------------------------------*/
static bool Stop(void) {
	glMainRunning = false;

	LOG_DEBUG("terminate search thread ...", NULL);

	// this forces an ongoing search to end
	mdnssd_close(glmDNSsearchHandle);
	pthread_join(glmDNSsearchThread, NULL);

	LOG_DEBUG("flush renderers ...", NULL);
	FlushRaopDevices();

	// Stop ActiveRemote server
	LOG_DEBUG("terminate mDNS responder", NULL);
	StopActiveRemote();

	LOG_DEBUG("terminate main thread ...", NULL);
	pthread_cond_signal(&glMainCond);
	pthread_join(glMainThread, NULL);
	pthread_mutex_destroy(&glMainMutex);
	pthread_cond_destroy(&glMainCond);

	for (int i = 0; i < MAX_RENDERERS;  i++) {
		pthread_mutex_destroy(&glMRDevices[i].Mutex);
		pthread_cond_destroy(&glMRDevices[i].Cond);
	}

	if (glConfigID) ixmlDocument_free(glConfigID);

	netsock_close();
	cross_ssl_free();

	return true;
}

/*---------------------------------------------------------------------------*/
static void sighandler(int signum) {
	static bool quit = false;
	// give it some time to finish ...
	if (quit) {
		LOG_INFO("Please wait for clean exit!", NULL);
		return;
	}
	quit = true;

	if (!glGracefullShutdown) {
		LOG_INFO("forced exit", NULL);
		exit(EXIT_SUCCESS);
	}
	sq_end();
	Stop();
	exit(EXIT_SUCCESS);
}

/*---------------------------------------------------------------------------*/
bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int i, optind = 1;
	char cmdline[256] = "";
	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}
	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("astxdfpibmM", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIk"
#if defined(RESAMPLE)
						  "uR"
#endif
		  , opt)) {
			optarg = NULL;
			optind += 1;
		}
		else {
			printf("%s", usage);
			return false;
		}
		switch (opt[0]) {
		case 's':
			strcpy(glDeviceParam.server, optarg);
			break;
		case 'a':
			strcpy(glPortOpen, optarg);
			break;
		case 'M':
			strcpy(glModelName, optarg);
			break;
		case 'b':
			strcpy(glInterface, optarg);
			break;
#if defined(RESAMPLE)
		case 'u':
		case 'R':
			if (optind < argc && argv[optind] && argv[optind][0] != '-') {
				strcpy(glDeviceParam.resample_options, argv[optind++]);
				glDeviceParam.resample = true;
			} else {
				strcpy(glDeviceParam.resample_options, "");
				glDeviceParam.resample = false;
			}
			break;
#endif
		case 'f':
			glLogFile = optarg;
			break;
		case 'i':
			strcpy(glConfigName, optarg);
			glDiscovery = true;
			break;
		case 'I':
			glAutoSaveConfigFile = true;
			break;
		case 'p':
			glPidFile = optarg;
			break;
		case 'Z':
			glInteractive = false;
			break;
		case 'k':
			glGracefullShutdown = false;
			break;
		case 'm':
			strcpy(glExcluded, optarg);
			break;
#if LINUX || FREEBSD || SUNOS
		case 'z':
			glDaemonize = true;
			break;
#endif
		case 'd':
			{
				char *l = strtok(optarg, "=");
				char *v = strtok(NULL, "=");
				log_level new = lWARN;
				if (l && v) {
					log_cmdline = true;
					if (!strcmp(v, "error"))  new = lERROR;
					if (!strcmp(v, "warn"))   new = lWARN;
					if (!strcmp(v, "info"))   new = lINFO;
					if (!strcmp(v, "debug"))  new = lDEBUG;
					if (!strcmp(v, "sdebug")) new = lSDEBUG;
					if (!strcmp(l, "all") || !strcmp(l, "slimproto"))	slimproto_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "stream"))    	stream_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "decode"))    	decode_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "output"))    	output_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "main"))     	main_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "util"))    	util_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "raop"))    	raop_loglevel = new;
					if (!strcmp(l, "all") || !strcmp(l, "slimmain"))    slimmain_loglevel = new;				}
				else {
					printf("%s", usage);
					return false;
				}
			}
			break;
		case 't':
			printf("%s", license);
			return false;
		default:
			break;
		}
	}
	return true;
}

/*----------------------------------------------------------------------------*/
/*																			  */
/*----------------------------------------------------------------------------*/
int main(int argc, char *argv[])
{
	int i;
	char resp[20] = "";
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
#if defined(SIGPIPE)
	signal(SIGPIPE, SIG_IGN);
#endif
#if defined(SIGQUIT)
	signal(SIGQUIT, sighandler);
#endif
#if defined(SIGHUP)
	signal(SIGHUP, sighandler);
#endif

	netsock_init();

	// first try to find a config file on the command line
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-x")) {
			strcpy(glConfigName, argv[i+1]);
		}
	}
	// load config from xml file
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig, &glDeviceParam);
	// do some parameters migration
	if (!glMigration || glMigration == 1 || glMigration == 2) {
		glMigration = 3;
		if (!strcasestr(glDeviceParam.codecs, "ogg")) strcat(glDeviceParam.codecs, ",ogg");
		SaveConfig(glConfigName, glConfigID, CONFIG_MIGRATE);
	}

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	// make sure port range is correct
	sscanf(glPortOpen, "%hu:%hu", &glPortBase, &glPortRange);
	if (glPortBase && !glPortRange) glPortRange = MAX_RENDERERS*4;

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting squeeze2raop version: %s\n", VERSION);

	if (strtod("0.30", NULL) != 0.30) {
		LOG_WARN("weird GLIBC, try -static build in case of failure");
	}

	if (!glConfigID) {
		LOG_ERROR("\n\n!!!!!!!!!!!!!!!!!! ERROR LOADING CONFIG FILE !!!!!!!!!!!!!!!!!!!!!\n", NULL);
	}

	// just do device discovery and exit
	if (glDiscovery) {
		Start();
		sleep(DISCOVERY_TIME + 1);
		Stop();
		return(0);
	}

#if LINUX || FREEBSD || SUNOS
	if (glDaemonize) {
		if (daemon(1, glLogFile ? 1 : 0)) {
			fprintf(stderr, "error daemonizing: %s\n", strerror(errno));
		}
	}
#endif
	if (glPidFile) {
		FILE *pid_file;
		pid_file = fopen(glPidFile, "wb");
		if (pid_file) {
			fprintf(pid_file, "%d", getpid());
			fclose(pid_file);
		}
		else {
			LOG_ERROR("Cannot open PID file %s", glPidFile);
		}
	}

	if (!Start()) {
		LOG_ERROR("Cannot start, exiting", NULL);
		exit(0);
	}

	while (strcmp(resp, "exit")) {

#if LINUX || FREEBSD || SUNOS
		if (!glDaemonize && glInteractive)
			i = scanf("%s", resp);
		else
			pause();
#else
		if (glInteractive)
			i = scanf("%s", resp);
		else
#if OSX
			pause();
#else
			Sleep(INFINITE);
#endif
#endif

		SET_LOGLEVEL(stream);
		SET_LOGLEVEL(output);
		SET_LOGLEVEL(decode);
		SET_LOGLEVEL(slimproto);
		SET_LOGLEVEL(slimmain);
		SET_LOGLEVEL(main);
		SET_LOGLEVEL(util);
		SET_LOGLEVEL(raop);

		if (!strcmp(resp, "save"))	{
			char name[128];
			i = scanf("%s", name);
			SaveConfig(name, glConfigID, true);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			uint32_t now = gettime_ms();
			bool all = !strcmp(resp, "dumpall");

			for (i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [sq:%u] [%s:%u] [mw:%u] [f:%u] [%p::%p]\n",
						p->FriendlyName, p->Running, Locked, p->sqState,
						inet_ntoa(p->PlayerIP), p->PlayerPort, p->MetadataWait,
						(now - p->LastFlush)/1000,
						p, sq_get_ptr(p->SqueezeHandle));
			}
		}
	}

	LOG_INFO("stopping squeezelite devices ...", NULL);
	sq_end();
	LOG_INFO("stopping Raop devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);
	return true;
}



