/*
 *  SpotRaop - Spotify to Airplay bridge
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

#if defined(_MSC_VER)
#pragma comment(lib, "Ws2_32.lib")
#endif

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
#include "mdnssd.h"
#include "mdnssvc.h"
#include "http_fetcher.h"
#include "raop_client.h"

#include "spotraop.h"
#include "config_raop.h"
#include "metadata.h"
#include "spotify.h"

#define FRAMES_PER_BLOCK 352
#define DISCOVERY_TIME	 60

enum { VOLUME_IGNORE = 0, VOLUME_SOFT = 1, VOLUME_HARD = 2};
enum { VOLUME_FEEDBACK = 1, VOLUME_UNFILTERED = 2};

/*----------------------------------------------------------------------------*/
/* globals 																	  */
/*----------------------------------------------------------------------------*/
int32_t				glLogLimit = -1;
uint32_t			glNetmask;
uint16_t			glPortBase, glPortRange;
char 				glInterface[128] = "?";
char				glExcludedModels[STR_LEN] = "aircast,airupnp,shairtunes2,airesp32,";
char				glIncludedNames[STR_LEN];
char				glExcludedNames[STR_LEN];
struct sMR			glMRDevices[MAX_RENDERERS];
char				glCredentialsPath[STR_LEN];
bool				glCredentials;

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
							true,			 // Enabled
							"",				 // Credentials
							"",				 // Name
							{0, 0, 0, 0, 0, 0 }, // MAC
							true,			 // sendMetaData
							true,			 // sendCoverArt         
							160,			 // VorbisRate
							120,			 // RemoveTimeout
							false,			 // Encryption
							"",				 // RaopCredentials
							"",				 // Password
							2000,			 // ReadAhead
							2,				 // VolumeMode = HARDWARE
							VOLUME_FEEDBACK, // VolumeFeedback
							true,			 // AlacEncode
					};

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
static char 				glDACPid[] = "1A2B3D4EA1B2C3D5";
static struct mdnsd 		*gl_mDNSResponder;
static int					glActiveRemoteSock;
static pthread_t			glActiveRemoteThread;
static void					*glConfigID = NULL;
static char					glConfigName[STR_LEN] = "./config.xml";
static struct in_addr 		glHost;
static bool					glPairing, glPassword;
static bool					glUpdated;
static char*				glSpotifyUserName;
static char*				glSpotifyPassword;
static char*				glNameFormat = "%s+";

static char usage[] =

		VERSION "\n"
		"See -t for license terms\n"
		"Usage: [options]\n"
		"  -b <ip>             network interface/IP address to bind to\n"
		"  -a <port>[:<count>] set inbound port and range for RTP and HTTP\n"
		"  -J <path>           path to Spotify credentials files\n"
		"  -j  	               store Spotify credentials in XML config file\n"
		"  -U <user>           Spotify username\n"
		"  -P <password>       Spotify password\n"
	    "  -L                  set AirPlay player password\n"
		"  -c <alac[|pcm>      audio format send to player\n"
		"  -r <96|160|320>     set Spotify vorbis codec rate\n"
		"  -N <format>         transform device name using C format (%s=name)\n"
		"  -x <config file>    read config from file (default is ./config.xml)\n"
		"  -i <config file>    discover players, save <config file> and exit\n"
		"  -I                  auto save config at every network scan\n"
		"  -f <logfile>        write debug to logfile\n"
		"  -l                  AppleTV pairing\n"
		"  -p <pid file>       write PID in file\n"
		"  -m <n1,n2...>       exclude devices whose model include tokens\n"
		"  -n <m1,m2,...>      exclude devices whose name includes tokens\n"
		"  -o <m1,m2,...>      include only listed models; overrides -m and -n (use <NULL> if player don't return a model)\n"
		"  -d <log>=<level>    set logging level, logs: all|main|util|upnp, level: error|warn|info|debug|sdebug\n"

#if LINUX || FREEBSD || SUNOS
		   "  -z               daemonize\n"
#endif
		   "  -Z               NOT interactive\n"
		   "  -k               immediate exit on SIGQUIT and SIGTERM\n"
		   "  -t               license terms\n"
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
static bool IsExcluded(char *Model, char* Name);
static void* GetArtworkThread(void* arg);

typedef struct {
	struct sMR* Device;
	char* Url, * ContentType, * Image;
	int Size;
} tArtworkFetch;

/*----------------------------------------------------------------------------*/
struct raopcl_s* shadowRaop(struct shadowPlayer* shadow) {
	struct sMR* Device = (struct sMR*)shadow;
	return Device->Raop;
}

/*----------------------------------------------------------------------------*/
void shadowRequest(struct shadowPlayer* shadow, enum spotEvent event, ...) {
	struct sMR* Device = (struct sMR*)shadow;
	va_list args;
	va_start(args, event);

	pthread_mutex_lock(&Device->Mutex);

	switch (event) {

	case SPOT_CREDENTIALS: {
		char* Credentials = va_arg(args, char*);

		// store credentials in dedicated file
		if (*glCredentialsPath) {
			char* name;
			asprintf(&name, "%s/spotraop-%08x.json", glCredentialsPath, hash32(Device->UDN));
			FILE* file = fopen(name, "w");
			free(name);
			if (file) {
				fputs(Credentials, file);
				fclose(file);
			}
		}

		// store credentials in XML config file if required (small chance of race condition)
		if (glCredentials && glAutoSaveConfigFile) {
			glUpdated = true;
			strncpy(Device->Config.Credentials, Credentials, sizeof(Device->Config.Credentials) - 1);
		}
		break;
	}
	case SPOT_LOAD:
		LOG_INFO("[%p]: spotify LOAD request", Device);
		raopcl_connect(Device->Raop, Device->PlayerIP, Device->PlayerPort, true);
		break;
	case SPOT_PLAY:
		LOG_INFO("[%p]: spotify PLAY request", Device);
		raopcl_connect(Device->Raop, Device->PlayerIP, Device->PlayerPort, true);
		Device->SpotState = SPOT_PLAY;
		break;
	case SPOT_VOLUME: {
		// discard echo commands
		uint32_t now = gettime_ms();
		if (now < Device->VolumeStampRx + 1000) break;

		// Volume is normalized 0..1 and 0 is -144
		Device->Muted = false;
		Device->Volume = -30.0 * (1.0 - (double) va_arg(args, int) / UINT16_MAX);
		raopcl_set_volume(Device->Raop, Device->Volume > -30.0 ? Device->Volume : -144);

		LOG_INFO("[%p]: spotify VOLUME request %lf", Device, Device->Volume);
		break;
	}
	case SPOT_METADATA: {
		if (!Device->Config.SendMetaData) break;
	
		metadata_t* MetaData = va_arg(args, metadata_t*);
		
		// send basic metadata now (works as long as we are flushed)
		raopcl_set_daap(Device->Raop, 3, 
			"minm", 's', MetaData->title,
			"asar", 's', MetaData->artist,
			"asal", 's', MetaData->album
		);

		// go fetch the artwork if any
		if (MetaData->artwork && Device->Config.SendCoverArt) {
			tArtworkFetch* Artwork = calloc(1, sizeof(tArtworkFetch));
			Artwork->Url = strdup(MetaData->artwork);
			Artwork->Device = Device;

			pthread_t lambda;
			pthread_create(&lambda, NULL, &GetArtworkThread, Artwork);
		}

		break;
	}
	default:
		break;
	}

	va_end(args);
	pthread_mutex_unlock(&Device->Mutex);
}

/*----------------------------------------------------------------------------*/
static void* GetArtworkThread(void *arg) {
	tArtworkFetch* Artwork = (tArtworkFetch*)arg;
	struct sMR* Device = Artwork->Device;

	// try to get the image, might take a while
	Artwork->Size = http_fetch(Artwork->Url, &Artwork->ContentType, &Artwork->Image);
	pthread_mutex_lock(&Device->Mutex);

	// only send if we are running (if flushed then nothing happens)
	if (Artwork->Size > 0 && Device->Running) {
		LOG_INFO("got artwork for %s", Artwork->Url);
		raopcl_set_artwork(Device->Raop, Artwork->ContentType, Artwork->Size, Artwork->Image);
	}

	NFREE(Artwork->ContentType);
	NFREE(Artwork->Image);
	free(Artwork->Url);
	free(Artwork);

	pthread_mutex_unlock(&Device->Mutex);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static char *GetmDNSAttribute(mdnssd_txt_attr_t *p, int count, char *name) {
	for (int i = 0; i < count; i++)	if (!strcasecmp(p[i].name, name))return strdup(p[i].value);
	return NULL;
}

/*----------------------------------------------------------------------------*/
static struct sMR *SearchUDN(char *UDN) {
	for (int i = 0; i < MAX_RENDERERS; i++) if (glMRDevices[i].Running && !strcmp(glMRDevices[i].UDN, UDN))	return glMRDevices + i;
	return NULL;
}

/*----------------------------------------------------------------------------*/
static void UpdateDevices() {
	uint32_t now = gettime_ms();

	pthread_mutex_lock(&glMainMutex);

	// walk through the list for device whose timeout expired
	for (int i = 0; i < MAX_RENDERERS; i++) {
		struct sMR* Device = Device = glMRDevices + i;
		if (!Device->Running || Device->Config.RemoveTimeout <= 0 || !Device->Expired || now < Device->Expired + Device->Config.RemoveTimeout * 1000) continue;

		LOG_INFO("[%p]: removing renderer (%s) on timeout", Device, Device->FriendlyName);
		spotDeletePlayer(Device->SpotPlayer);
		DelRaopDevice(Device);
	}

	pthread_mutex_unlock(&glMainMutex);
}

/*----------------------------------------------------------------------------*/
static bool mDNSsearchCallback(mdnssd_service_t *slist, void *cookie, bool *stop) {
	struct sMR *Device;
	mdnssd_service_t *s;
	uint32_t now = gettime_ms();

	for (s = slist; s && glMainRunning; s = s->next) {
		char *am = GetmDNSAttribute(s->attr, s->attr_count, "am");
		bool excluded = am ? IsExcluded(am, s->name) : false;
		NFREE(am);

		// ignore excluded and announces made on behalf
		if (!s->name || excluded || (s->host.s_addr != s->addr.s_addr && ((s->host.s_addr & glNetmask) == (s->addr.s_addr & glNetmask)))) continue;

		// is that device already here
		if ((Device = SearchUDN(s->name)) != NULL) {
			Device->Expired = 0;
			// device disconnected
			if (s->expired) {
				if (!raopcl_is_connected(Device->Raop) && !Device->Config.RemoveTimeout) {
					LOG_INFO("[%p]: removing renderer (%s)", Device, Device->FriendlyName);
					spotDeletePlayer(Device->SpotPlayer);
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
			// create a new spotify device
			char id[6 * 2 + 1] = { 0 };
			for (int i = 0; i < 6; i++) sprintf(id + i * 2, "%02x", Device->Config.MAC[i]);
			if (!*(Device->Config.Name)) sprintf(Device->Config.Name, glNameFormat, Device->FriendlyName);
			Device->SpotPlayer = spotCreatePlayer(Device->Config.Name, id, Device->Credentials, glHost, Device->Config.VorbisRate, 
												  FRAMES_PER_BLOCK, Device->Config.ReadAhead, (struct shadowPlayer*)Device);
			glUpdated = true;
		}
	}

	UpdateDevices();

	// save config file if needed (only when creating/changing config items devices)
	if ((glUpdated && glAutoSaveConfigFile) || glDiscovery) {
		glUpdated = false;
		if (!glDiscovery) LOG_INFO("Updating configuration %s", glConfigName);
		SaveConfig(glConfigName, glConfigID, glDiscovery);
	}

	// we have intentionally not released the slist
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
		if (!glMainRunning) break;

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

		UpdateDevices();
	}

	return NULL;
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
	LoadMRConfig(glConfigID, s->name, &Device->Config);

	if (!Device->Config.Enabled) return false;

	if (strcasestr(s->name, "AirSonos")) {
		LOG_DEBUG("[%p]: skipping AirSonos player", Device);
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
		if (*Device->Config.RaopCredentials) {
			LOG_INFO("[%p]: AppleTV with valid authentication key %s", Device, Device->Config.RaopCredentials);
		} else {
			LOG_INFO("[%p]: AppleTV with no authentication key, create one using '-l' option", Device);
		}
	}

	Device->Running 		= true;
	// make sure that 1st volume is not missed
	Device->VolumeStampRx 	= gettime_ms() - 2000;
	Device->PlayerIP 		= s->addr;
	Device->PlayerPort 		= s->port;
	Device->PlayerStatus	= 0;
	Device->Volume			= -30.0;
	Device->SkipStart 		= 0;
	Device->SkipDir 		= false;
	Device->SpotPlayer		= NULL;
	Device->Raop 			= NULL;
	Device->Expired			= 0;
	
	memset(Device->ActiveRemote, 0, 16);

	strcpy(Device->UDN, s->name);
	sprintf(Device->ActiveRemote, "%u", hash32(Device->UDN));

	// get credentials from config file if allowed
	if (glCredentials) strcpy(Device->Credentials, Device->Config.Credentials);

	// or from separated credential file (has precedence)
	if (*glCredentialsPath) {
		char* name;
		asprintf(&name, "%s/spotraop-%08x.json", glCredentialsPath, hash32(Device->UDN));
		FILE* file = fopen(name, "r");
		free(name);
		if (file) {
			fgets(Device->Credentials, sizeof(Device->Credentials), file);
			fclose(file);
		}
	}

	strcpy(Device->FriendlyName, s->hostname);
	p = strcasestr(Device->FriendlyName, ".local");
	if (p) *p = '\0';

	if (!memcmp(Device->Config.MAC, "\0\0\0\0\0\0", 6)) {
		uint32_t mac_size = 6;
		if (SendARP(s->addr.s_addr, INADDR_ANY, Device->Config.MAC, &mac_size)) {
			*(uint32_t*)(Device->Config.MAC + 2) = hash32(Device->UDN);
	}
		memset(Device->Config.MAC, 0xaa, 2);
	}

	// virtual players duplicate mac address
	for (int i = 0; i < MAX_RENDERERS; i++) {
		if (glMRDevices[i].Running && Device != glMRDevices + i && !memcmp(glMRDevices[i].Config.MAC, Device->Config.MAC, 6)) {
			memset(Device->Config.MAC, 0xaa, 2);
			*(uint32_t*)(Device->Config.MAC + 2) = hash32(Device->UDN);
			LOG_INFO("[%p]: duplicated mac ... updating", Device);
		}
	}

	LOG_INFO("[%p]: adding renderer (%s@%s) with mac %hX-%X", Device, Device->FriendlyName, inet_ntoa(Device->PlayerIP),  
	         *(uint16_t*)Device->Config.MAC, *(uint32_t*)(Device->Config.MAC + 2));

	// gather RAOP device capabilities, to be matched later
	char *SampleSize = GetmDNSAttribute(s->attr, s->attr_count, "ss");
	char *SampleRate = GetmDNSAttribute(s->attr, s->attr_count, "sr");
	char *Channels = GetmDNSAttribute(s->attr, s->attr_count, "ch");
	char *Codecs = GetmDNSAttribute(s->attr, s->attr_count, "cn");
	char *Cipher = GetmDNSAttribute(s->attr, s->attr_count, "et");

	if (!Codecs || !strchr(Codecs, '1')) {
		LOG_WARN("[%p]: ALAC not in codecs, player might not work %s", Device, Codecs);
	}

	if ((Device->Config.Encryption || Auth) && strchr(Cipher, '1'))	Crypto = RAOP_RSA;
	else Crypto = RAOP_CLEAR;

	char* password = NULL;

	if (*Device->Config.Password) {
		char* encrypted;
		asprintf(&encrypted, "%s==", Device->Config.Password);
		password = calloc(strlen(encrypted), 1);
		base64_decode(encrypted, password);
		for (int i = 0; password[i]; i++) password[i] ^= Device->UDN[i];
		free(encrypted);
	}

	Device->Raop = raopcl_create(glHost, glPortBase, glPortRange,
								 glDACPid, Device->ActiveRemote,
								 Device->Config.AlacEncode ? RAOP_ALAC : RAOP_ALAC_RAW , FRAMES_PER_BLOCK,
								 (uint32_t) MS2TS(Device->Config.ReadAhead, SampleRate ? atoi(SampleRate) : 44100),
								 Crypto, Auth, Device->Config.RaopCredentials, password, Cipher, md,
								 SampleRate ? atoi(SampleRate) : 44100,
								 SampleSize ? atoi(SampleSize) : 16,
								 Channels ? atoi(Channels) : 2,
								 Device->Volume);

	NFREE(password);
	NFREE(am);
	NFREE(md);
	NFREE(pk);
	NFREE(SampleSize);
	NFREE(SampleRate);
	NFREE(Channels);
	NFREE(Codecs);
	NFREE(Cipher);

	if (!Device->Raop) {
		LOG_ERROR("[%p]: cannot create raop device", Device);
		return false;
	}

	return true;
}

/*----------------------------------------------------------------------------*/
static void FlushRaopDevices(void) {
	for (int i = 0; i < MAX_RENDERERS; i++) {
		struct sMR *p = &glMRDevices[i];
		if (p->Running) DelRaopDevice(p);
	}
}

/*----------------------------------------------------------------------------*/
static void DelRaopDevice(struct sMR *Device) {
	pthread_mutex_lock(&Device->Mutex);
	Device->Running = false;
	pthread_mutex_unlock(&Device->Mutex);
	// there is no thread to join here...
	raopcl_destroy(Device->Raop);

	LOG_INFO("[%p]: Raop device stopped (%s)", Device, Device->FriendlyName);
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

		if (!strcasecmp(command, "pause")) spotNotify(Device->SpotPlayer, SHADOW_PAUSE);
		else if (!strcasecmp(command, "play")) spotNotify(Device->SpotPlayer, SHADOW_PLAY);
		else if (!strcasecmp(command, "playpause")) spotNotify(Device->SpotPlayer, SHADOW_PLAY_TOGGLE);
		else if (!strcasecmp(command, "stop")) spotNotify(Device->SpotPlayer, SHADOW_STOP);
		else if (!strcasecmp(command, "nextitem")) spotNotify(Device->SpotPlayer, SHADOW_NEXT);
		else if (!strcasecmp(command, "previtem")) spotNotify(Device->SpotPlayer, SHADOW_PREV);
		else if (!strcasecmp(command, "mutetoggle")) {
			Device->Muted = !Device->Muted;
			if (Device->Muted) {
				raopcl_set_volume(Device->Raop, -144.0);
				spotNotify(Device->SpotPlayer, SHADOW_VOLUME, 0);
			} else {
				raopcl_set_volume(Device->Raop, Device->Volume > -30 ? Device->Volume : -144);
				spotNotify(Device->SpotPlayer, SHADOW_VOLUME, (int)((30.0 + Device->Volume) / 30.0 * UINT16_MAX));
			}
		} else if (!strcasecmp(command, "volumeup")) {
			Device->Volume += 30.0 / 20.0;
			if (Device->Volume > 0) Device->Volume = 0;
			raopcl_set_volume(Device->Raop, Device->Volume);
			spotNotify(Device->SpotPlayer, SHADOW_VOLUME, (int)((30.0 + Device->Volume) / 30.0 * UINT16_MAX));
		} else if (!strcasecmp(command, "volumedown")) {
			Device->Volume -= 30.0 / 20.0;
			if (Device->Volume < -30.0) Device->Volume = -30.0;
			raopcl_set_volume(Device->Raop, Device->Volume > -30 ? Device->Volume : -144);
			spotNotify(Device->SpotPlayer, SHADOW_VOLUME, (int)((30.0 + Device->Volume) / 30.0 * UINT16_MAX));
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
					//sq_notify(Device->SqueezeHandle, SQ_OFF);
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
				double volume;
				uint32_t now = gettime_ms();

				sscanf(command, "%*[^=]=%lf", &volume);
				if (strcasestr(command, ".volume=")) volume = -30.0 * (1.0 - volume / 100);

				LOG_INFO("[%p]: volume feedback %u (%.2lf)", Device, i, volume);

				if (volume != Device->Volume || Device->Muted) {
					Device->VolumeStampRx = now;
					Device->Volume = volume;
					Device->Muted = false;
					spotNotify(Device->SpotPlayer, SHADOW_VOLUME, (int) ((30.0 + volume) / 30.0 * UINT16_MAX));
					raopcl_set_volume(Device->Raop, volume > -30 ? volume : -144);
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
static void StartActiveRemote(struct in_addr host) {
	struct sockaddr_in addr;
	socklen_t nlen = sizeof(struct sockaddr);
	const char *txt[] = {
		"txtvers=1",
		"Ver=131075",
		"DbId=63B5E5C0C201542E",
		"OSsi=0x1F5",
		NULL
	};

	if ((glActiveRemoteSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		LOG_ERROR("Cannot create ActiveRemote socket", NULL);
		return;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_addr.s_addr = host.s_addr;
	addr.sin_family = AF_INET;

	for (int range = glPortRange ? glPortRange : 1, count = 0, offset = rand();; count++, offset++) {
		addr.sin_port = htons(glPortBase + (offset % range));
		if (!bind(glActiveRemoteSock, (struct sockaddr*)&addr, sizeof(addr))) break;
		if (!glPortBase || count == range) {
			LOG_ERROR("Cannot bind ActiveRemote: %s", strerror(errno));
			closesocket(glActiveRemoteSock);
			return;
		}
	};

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

/*----------------------------------------------------------------------------*/
static void StopActiveRemote(void) {
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
static bool IsExcluded(char* Model, char* Name) {
	char item[STR_LEN];
	char* p = glExcludedModels;
	char* q = glExcludedNames;
	char* o = glIncludedNames;

	if (*glIncludedNames) {
		if (!Name) {
			if (strcasestr(glIncludedNames, "<NULL>")) return false;
			else return true;
		}
		do {
			sscanf(o, "%[^,]", item);
			if (!strcmp(Name, item)) return false;
			o += strlen(item);
		} while (*o++);
		return true;
	}

	if (*glExcludedModels && Model) {
		do {
			sscanf(p, "%[^,]", item);
			if (strcasestr(Model, item)) return true;
			p += strlen(item);
		} while (*p++);
	}

	if (*glExcludedNames && Name) {
		do {
			sscanf(q, "%[^,]", item);
			if (strcasestr(Name, item)) return true;
			q += strlen(item);
		} while (*q++);
	}

	return false;
}

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
	glHost = get_interface(!strchr(glInterface, '?') ? glInterface : NULL, NULL, &glNetmask);
	if (glHost.s_addr == INADDR_NONE) return false;

	memset(&glMRDevices, 0, sizeof(glMRDevices));

	pthread_mutex_init(&glMainMutex, 0);
	pthread_cond_init(&glMainCond, 0);

    for (i = 0; i < MAX_RENDERERS;  i++) {
		pthread_mutexattr_t mutexAttr;
		pthread_mutexattr_init(&mutexAttr);
		pthread_mutexattr_settype(&mutexAttr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&glMRDevices[i].Mutex, &mutexAttr);
	}

	// start cspot
	spotOpen(glPortBase, glPortRange, glSpotifyUserName, glSpotifyPassword);

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

	LOG_INFO("terminate search thread ...", NULL);

	// this forces an ongoing search to end
	mdnssd_close(glmDNSsearchHandle);
	pthread_join(glmDNSsearchThread, NULL);

	// can now finish all cspot instances
	spotClose();

	LOG_INFO("flush renderers ...", NULL);
	FlushRaopDevices();

	// Stop ActiveRemote server
	LOG_INFO("terminate mDNS responder", NULL);
	StopActiveRemote();

	LOG_INFO("terminate main thread ...", NULL);
	pthread_cond_signal(&glMainCond);
	pthread_join(glMainThread, NULL);
	pthread_mutex_destroy(&glMainMutex);
	pthread_cond_destroy(&glMainCond);

	for (int i = 0; i < MAX_RENDERERS;  i++) {
		pthread_mutex_destroy(&glMRDevices[i].Mutex);
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

	spotClose();
	Stop();
	exit(EXIT_SUCCESS);
}

/*---------------------------------------------------------------------------*/
static bool ParseArgs(int argc, char **argv) {
	char *optarg = NULL;
	int i, optind = 1;
	char cmdline[256] = "";
	for (i = 0; i < argc && (strlen(argv[i]) + strlen(cmdline) + 2 < sizeof(cmdline)); i++) {
		strcat(cmdline, argv[i]);
		strcat(cmdline, " ");
	}
	while (optind < argc && strlen(argv[optind]) >= 2 && argv[optind][0] == '-') {
		char *opt = argv[optind] + 1;
		if (strstr("abcrxifpmnodJUPN", opt) && optind < argc - 1) {
			optarg = argv[optind + 1];
			optind += 2;
		} else if (strstr("tzZIkljL"
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
		case 'a':
			sscanf(optarg, "%hu:%hu", &glPortBase, &glPortRange);
			break;
		case 'b':
			strcpy(glInterface, optarg);
			break;
		case 'c':
			if (!strcasecmp(optarg, "alac")) glMRConfig.AlacEncode = false;
			break;
		case 'r':
			glMRConfig.VorbisRate = atoi(optarg);
			break;
		case 'N':
			glNameFormat = optarg;
			break;
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
			strcat(glExcludedModels, optarg);
			break;
		case 'n':
			strcpy(glExcludedNames, optarg);
			break;
		case 'o':
			strcpy(glIncludedNames, optarg);
			break;
		case 'l':
			glPairing = true;
			break;
		case 'L':
			glPassword = true;
			break;
		case 'J':
			strncpy(glCredentialsPath, optarg, sizeof(glCredentialsPath) - 1);
			break;
		case 'j':
			glCredentials = true;
			break;
		case 'U':
			glSpotifyUserName = optarg;
			break;
		case 'P':
			glSpotifyPassword = optarg;
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
	glConfigID = (void*) LoadConfig(glConfigName, &glMRConfig);

	// potentially overwrite with some cmdline parameters
	if (!ParseArgs(argc, argv)) exit(1);

	// make sure port range is correct
	if (glPortBase && !glPortRange) glPortRange = MAX_RENDERERS*4;

	if (glLogFile) {
		if (!freopen(glLogFile, "a", stderr)) {
			fprintf(stderr, "error opening logfile %s: %s\n", glLogFile, strerror(errno));
		}
	}

	LOG_ERROR("Starting spotraop version: %s\n", VERSION);

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

	// just do pairing
	if (glPairing) {
		glDiscovery = true;
		Start();

		printf("\n*************** Wait 5 seconds for player discovery **************\n");
		sleep(5);
		printf("\n***************************** done *******************************\n");

		char* UDN = NULL, * secret = NULL;
		while (AppleTVpairing(NULL, &UDN, &secret)) {
			if (!UDN || !secret) continue;
			for (int i = 0; i < MAX_RENDERERS; i++) {
				if (glMRDevices[i].Running && !strcasecmp(glMRDevices[i].UDN, UDN)) {
					strcpy(glMRDevices[i].Config.RaopCredentials, secret);
					SaveConfig(glConfigName, glConfigID, false);
					break;
				}
			}
			NFREE(UDN); NFREE(secret);
		}

		Stop();
		return(0);
	}

	// just set password(s)
	if (glPassword) {
		glDiscovery = true;
		Start();

		printf("\n*************** Wait 5 seconds for player discovery **************\n");
		sleep(5);
		printf("\n***************************** done *******************************\n");

		char* UDN = NULL, * passwd = NULL;
		while (AirPlayPassword(NULL, IsExcluded, &UDN, &passwd)) {
			if (!UDN) continue;
			for (int i = 0; i < MAX_RENDERERS; i++) {
				if (glMRDevices[i].Running && !strcasecmp(glMRDevices[i].UDN, UDN)) {
					*glMRDevices[i].Config.Password = '\0';
					if (passwd && *passwd) {
						for (int j = 0; passwd[j]; j++) passwd[j] ^= glMRDevices[i].UDN[j];
						char* encrypted;
						size_t len = strlen(passwd);
						base64_encode(passwd, len, &encrypted);
						encrypted[strlen(encrypted) - 2] = '\0';
						strcpy(glMRDevices[i].Config.Password, encrypted);
						free(encrypted);
					}
					SaveConfig(glConfigName, glConfigID, false);
					break;
				}
			}
			NFREE(UDN); NFREE(passwd);
		}

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
			SaveConfig(name, glConfigID, false);
		}

		if (!strcmp(resp, "dump") || !strcmp(resp, "dumpall"))	{
			uint32_t now = gettime_ms();
			bool all = !strcmp(resp, "dumpall");

			for (i = 0; i < MAX_RENDERERS; i++) {
				struct sMR *p = &glMRDevices[i];
				bool Locked = pthread_mutex_trylock(&p->Mutex);

				if (!Locked) pthread_mutex_unlock(&p->Mutex);
				if (!p->Running && !all) continue;
				printf("%20.20s [r:%u] [l:%u] [sq:%u] [%s:%u]\n",
						p->FriendlyName, p->Running, Locked, /*p->sqState*/ 0,
						inet_ntoa(p->PlayerIP), p->PlayerPort);
			}
		}
	}

	LOG_INFO("stopping cspot devices ...", NULL);
	spotClose();
	LOG_INFO("stopping raop devices ...", NULL);
	Stop();
	LOG_INFO("all done", NULL);
	return true;
}
