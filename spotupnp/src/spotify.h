/* 
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#include "metadata.h"
#include "HTTPmode.h"

#ifdef __cplusplus
extern "C" {
#endif

enum spotEvent{ SPOT_STOP, SPOT_LOAD, SPOT_PLAY, SPOT_PAUSE, SPOT_VOLUME };
enum shadowEvent { SHADOW_NONE, SHADOW_TRACK, SHADOW_PLAY, SHADOW_PAUSE, SHADOW_STOP, SHADOW_NEXT, SHADOW_PREV, SHADOW_TIME, SHADOW_VOLUME };

struct spotPlayer;
struct shadowPlayer;

void			       shadowRequest(struct shadowPlayer* shadow, enum spotEvent event, ...);
struct HTTPheaderList* shadowHeaders(struct shadowPlayer* shadow, struct HTTPheaderList *headers);

struct spotPlayer* spotCreatePlayer(char* name, char* id, struct in_addr addr, int audio, char *codec, bool flow, 
								    int64_t contentLength, struct shadowPlayer* shadow);
void spotDeletePlayer(struct spotPlayer *spotPlayer);
bool spotGetMetaData(struct spotPlayer* spotPlayer, const char* streamUrl, metadata_t* metadata);
void spotOpen(uint16_t portBase, uint16_t portRange);
void spotClose(void);
void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...);

#ifdef __cplusplus
}
#endif