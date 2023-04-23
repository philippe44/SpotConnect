/* 
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#include "metadata.h"
#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum spotEvent{ SPOT_LOAD, SPOT_PLAY, SPOT_VOLUME, SPOT_METADATA };
enum shadowEvent { SHADOW_PLAY, SHADOW_PAUSE, SHADOW_PLAY_TOGGLE, SHADOW_STOP, SHADOW_NEXT, SHADOW_PREV, SHADOW_VOLUME };

struct spotPlayer;
struct shadowPlayer;
struct raopcl_s;

struct raopcl_s*	shadowRaop(struct shadowPlayer* shadow);
void				shadowRequest(struct shadowPlayer* shadow, enum spotEvent event, ...);

struct spotPlayer* spotCreatePlayer(char* name, char* id, struct in_addr addr, int audio, 
									size_t frameSize, uint32_t delay, struct shadowPlayer* shadow);
void spotDeletePlayer(struct spotPlayer *spotPlayer);
void spotOpen(uint16_t portBase, uint16_t portRange);
void spotClose(void);
void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...);

#ifdef __cplusplus
}
#endif