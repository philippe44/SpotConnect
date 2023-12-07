/* 
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "pthread.h"

#include "metadata.h"
#include "HTTPmode.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The two sides share a common mutex for accessing player's data. This mutex is always valid for the 
 * duration of this whole application. Note that shadowRequest function expects mutex to be locked but 
 * will lock it if not, for safety. The spotNotify also expects that mutex to be locked but will NOT
 * try to lock as it uses scope_lock and lock_guard for convenience. This also means that spotNotify 
 * can call shadowRequest in return safely, with no risk of deadlock */
 
enum spotEvent{ SPOT_STOP, SPOT_LOAD, SPOT_PLAY, SPOT_PAUSE, SPOT_VOLUME, SPOT_CREDENTIALS };
enum shadowEvent { SHADOW_NONE, SHADOW_TRACK, SHADOW_PLAY, SHADOW_PAUSE, SHADOW_STOP, SHADOW_NEXT, SHADOW_PREV, SHADOW_TIME, SHADOW_VOLUME };

struct spotPlayer;
struct shadowPlayer;

struct HTTPheaderList* shadowHeaders(struct shadowPlayer* shadow, struct HTTPheaderList *headers);
void				   shadowRequest(struct shadowPlayer* shadow, enum spotEvent event, ...);

struct spotPlayer* spotCreatePlayer(char* name, char* id, char *credentials, struct in_addr addr, int audio, char *codec, bool flow, 
								    int64_t contentLength, bool useFileCache, struct shadowPlayer* shadow, pthread_mutex_t *mutex);
void spotDeletePlayer(struct spotPlayer *spotPlayer);
bool spotGetMetaForUrl(struct spotPlayer* spotPlayer, const char* url, metadata_t* metadata);
void spotOpen(uint16_t portBase, uint16_t portRange, char* username, char *password);
void spotClose(void);
void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...);

#ifdef __cplusplus
}
#endif