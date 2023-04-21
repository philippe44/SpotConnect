/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  (c) Philippe, philippe_44@outlook.com for raop/multi-instance modifications
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#include <math.h>
#include <signal.h>
#include <ctype.h>

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)
#define LOCK_D   mutex_lock(ctx->decode.mutex)
#define UNLOCK_D mutex_unlock(ctx->decode.mutex)
#define LOCK_P   mutex_lock(ctx->mutex)
#define UNLOCK_P mutex_unlock(ctx->mutex)

struct thread_ctx_s thread_ctx[MAX_PLAYER];
char				sq_model_name[STR_LEN];
struct in_addr		sq_local_host;
bool				soxr_loaded = false;

/*----------------------------------------------------------------------------*/
/* locals */
/*----------------------------------------------------------------------------*/
static void sq_wipe_device(struct thread_ctx_s *ctx);

extern log_level	slimmain_loglevel;
static log_level	*loglevel = &slimmain_loglevel;

/*---------------------------------------------------------------------------*/
void sq_end() {
	int i;

	for (i = 0; i < MAX_PLAYER; i++) {
		if (thread_ctx[i].in_use) {
			sq_wipe_device(&thread_ctx[i]);
		}
	}

	decode_end();
#if RESAMPLE
	deregister_soxr();
#endif
}

static bool lambda(void* caller, sq_action_t action, ...) {
	return true;
}

/*--------------------------------------------------------------------------*/
void sq_wipe_device(struct thread_ctx_s *ctx) {
	ctx->callback = lambda;
	ctx->in_use = false;

	slimproto_close(ctx);
	output_close(ctx);
#if RESAMPLE
	process_end(ctx);
#endif
	decode_close(ctx);
	stream_close(ctx);
}

/*--------------------------------------------------------------------------*/
void sq_delete_device(sq_dev_handle_t handle) {
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	sq_wipe_device(ctx);
}

/*---------------------------------------------------------------------------*/
static char from_hex(char ch) {

  return isdigit(ch) ? ch - '0' : tolower(ch) - 'a' + 10;
}

/*---------------------------------------------------------------------------*/
static char to_hex(char code) {
  static char hex[] = "0123456789abcdef";
  return hex[code & 15];
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_encode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) * 3 + 1), *pbuf = buf;
  while (*pstr) {
	if ( isalnum(*pstr) || *pstr == '-' || *pstr == '_' || *pstr == '.' ||
						  *pstr == '~' || *pstr == ' ' || *pstr == ')' ||
						  *pstr == '(' )
	  *pbuf++ = *pstr;
	else if (*pstr == '%') {
	  *pbuf++ = '%',*pbuf++ = '2', *pbuf++ = '5';
	}
	else {
	  *pbuf++ = '%', *pbuf++ = to_hex(*pstr >> 4), *pbuf++ = to_hex(*pstr & 15);
   }
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}

/*---------------------------------------------------------------------------*/
/* IMPORTANT: be sure to free() the returned string after use */
static char *cli_decode(char *str) {
  char *pstr = str, *buf = malloc(strlen(str) + 1), *pbuf = buf;
  while (*pstr) {
	if (*pstr == '%') {
	  if (pstr[1] && pstr[2]) {
		*pbuf++ = (from_hex(pstr[1]) << 4) | from_hex(pstr[2]);
		pstr += 2;
	  }
	} else {
	  *pbuf++ = *pstr;
	}
	pstr++;
  }
  *pbuf = '\0';
  return buf;
}


/*---------------------------------------------------------------------------*/

/* IMPORTANT: be sure to free() the returned string after use */

static char *cli_find_tag(char *str, char *tag)

{
	char *p, *res = NULL;
	char *buf = malloc(max(strlen(str), strlen(tag)) + 4);

	strcpy(buf, tag);
	strcat(buf, "%3a");
	if ((p = strcasestr(str, buf)) != NULL) {
		int i = 0;
		p += strlen(buf);
		while (*(p+i) != ' ' && *(p+i) != '\n' && *(p+i)) i++;
		if (i) {
			strncpy(buf, p, i);
			buf[i] = '\0';
			res = url_decode(buf);
		}
	}
	free(buf);
	return res;
}


/*---------------------------------------------------------------------------*/
bool cli_open_socket(struct thread_ctx_s *ctx) {
	struct sockaddr_in addr;

	if (ctx->cli_sock > 0) return true;

	ctx->cli_sock = socket(AF_INET, SOCK_STREAM, 0);
	set_nonblock(ctx->cli_sock);
	set_nosigpipe(ctx->cli_sock);

	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ctx->slimproto_ip;
	addr.sin_port = htons(ctx->cli_port);

	if (tcp_connect_timeout(ctx->cli_sock, addr, 250))  {
		LOG_ERROR("[%p] unable to connect to server with cli", ctx);
		closesocket(ctx->cli_sock);
		ctx->cli_sock = -1;
		return false;
	}

	LOG_INFO("[%p]: opened CLI socket %d", ctx, ctx->cli_sock);
	return true;
}



/*---------------------------------------------------------------------------*/
#define CLI_SEND_SLEEP (10000)
#define CLI_SEND_TO (1*500000)
#define CLI_KEEP_DURATION (15*60*1000)
#define CLI_PACKET 4096
char *cli_send_cmd(char *cmd, bool req, bool decode, struct thread_ctx_s *ctx) {
	char *packet;
	int wait;
	size_t len;
	char *rsp = NULL;

	mutex_lock(ctx->cli_mutex);

	if (!cli_open_socket(ctx)) {
		mutex_unlock(ctx->cli_mutex);
		return NULL;
	}

	packet = malloc(CLI_PACKET + 1);
	ctx->cli_timeout = gettime_ms() + CLI_KEEP_DURATION;
	wait = CLI_SEND_TO / CLI_SEND_SLEEP;

	cmd = cli_encode(cmd);

	if (req) len = sprintf(packet, "%s ?\n", cmd);
	else len = sprintf(packet, "%s\n", cmd);

	LOG_SDEBUG("[%p]: cmd %s", ctx, packet);

	send_packet((u8_t*) packet, len, ctx->cli_sock);

	// first receive the tag and then point to the last '\n'
	len = 0;
	while (wait)	{
		int k;
		fd_set rfds;
		struct timeval timeout = {0, CLI_SEND_SLEEP};

		FD_ZERO(&rfds);
		FD_SET(ctx->cli_sock, &rfds);

		k = select(ctx->cli_sock + 1, &rfds, NULL, NULL, &timeout);

		if (!k) {
			wait--;
			continue;
		}

		if (k < 0) break;

		k = recv(ctx->cli_sock, packet + len, CLI_PACKET - len, 0);
		if (k <= 0) break;

		len += k;
		packet[len] = '\0';
		if (strchr(packet, '\n') && strcasestr(packet, cmd)) {
			rsp = packet;
			break;
		}
	}

	if (!wait) {
		LOG_WARN("[%p]: Timeout waiting for CLI reponse (%s)", ctx, cmd);
	}

	LOG_SDEBUG("[%p]: rsp %s", ctx, rsp);

	if (rsp && ((rsp = strcasestr(rsp, cmd)) != NULL)) {
		rsp += strlen(cmd);
		while (*rsp && *rsp == ' ') rsp++;

		if (decode) rsp = cli_decode(rsp);
		else rsp = strdup(rsp);
		*(strrchr(rsp, '\n')) = '\0';
	}

	mutex_unlock(ctx->cli_mutex);

	NFREE(cmd);
	free(packet);

	return rsp;
}

/*--------------------------------------------------------------------------*/
sq_action_t sq_get_mode(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle %d", ctx, handle);
		return true;
	}

	sprintf(cmd, "%s mode", ctx->cli_id);
	rsp  = cli_send_cmd(cmd, true, false, ctx);

	if (!rsp) return SQ_NONE;
	if (!strcasecmp(rsp, "play")) return SQ_PLAY;
	if (!strcasecmp(rsp, "pause")) return SQ_PAUSE;
	if (!strcasecmp(rsp, "stop")) return SQ_STOP;

	return SQ_NONE;
}


/*--------------------------------------------------------------------------*/
bool sq_get_metadata(sq_dev_handle_t handle, metadata_t *metadata, bool next)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[1024];
	char *rsp, *p, *cur;

	metadata_init(metadata);

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle %d", ctx, handle);
		metadata_defaults(metadata);
		return false;
	}

	sprintf(cmd, "%s status - 2 tags:xcfldatgrKN", ctx->cli_id);
	rsp = cli_send_cmd(cmd, false, false, ctx);

	if (!rsp || !*rsp) {
		metadata_defaults(metadata);
		LOG_WARN("[%p]: cannot get metadata", ctx);
		return true;
	}

	// find the current index
	if ((p = cli_find_tag(rsp, "playlist_cur_index")) != NULL) {
		metadata->index = atoi(p);
		if (next) metadata->index++;
		free(p);
	}

	// need to make sure we rollover if end of list
	if ((p = cli_find_tag(rsp, "playlist_tracks")) != NULL) {
		int len = atoi(p);
		if (len) metadata->index %= len;
		free(p);
	}

	sprintf(cmd, "playlist%%20index%%3a%d ", metadata->index);

	if ((cur = strcasestr(rsp, cmd)) != NULL) {
		metadata->title = cli_find_tag(cur, "title");
		metadata->artist = cli_find_tag(cur, "artist");
		metadata->album = cli_find_tag(cur, "album");
		metadata->genre = cli_find_tag(cur, "genre");
		metadata->artwork = cli_find_tag(cur, "artwork_url");

		if ((p = cli_find_tag(cur, "duration")) != NULL) {
			metadata->duration = 1000 * atof(p);
			free(p);
		}

		if ((p = cli_find_tag(cur, "tracknum")) != NULL) {
			metadata->track = atol(p);
			free(p);
		}

		if ((p = cli_find_tag(cur, "remote")) != NULL) {
			metadata->remote = (atoi(p) == 1);
			free(p);
		}

		if (!metadata->artwork || !strlen(metadata->artwork)) {
			NFREE(metadata->artwork);
			if ((p = cli_find_tag(cur, "coverid")) != NULL) {
				(void)!asprintf(&metadata->artwork, "http://%s:%s/music/%s/cover_%s.jpg", ctx->server_ip, ctx->server_port, p, ctx->config.resolution);
				free(p);
			}
		}

		if (metadata->artwork && strncmp(metadata->artwork, "http", 4)) {
			char* artwork;

			p = strrchr(metadata->artwork, '.');
			if (*ctx->config.resolution && p && (strcasecmp(p, ".jpg") || strcasecmp(p, ".png"))) {
				*p = '\0';
				(void)!asprintf(&artwork, "http://%s:%s/%s_%s.%s", ctx->server_ip, ctx->server_port,
					*(metadata->artwork) == '/' ? metadata->artwork + 1 : metadata->artwork,
					ctx->config.resolution, p + 1);
			} else {
				(void)!asprintf(&artwork, "http://%s:%s/%s", ctx->server_ip, ctx->server_port,
					*(metadata->artwork) == '/' ? metadata->artwork + 1 : metadata->artwork);
			}

			free(metadata->artwork);
			metadata->artwork = artwork;
		}
	}

	NFREE(rsp);

	metadata_defaults(metadata);

	LOG_DEBUG("[%p]: idx %d\n\tartist:%s\n\talbum:%s\n\ttitle:%s\n\tgenre:%s\n\tduration:%d.%03d\n\tsize:%d\n\tcover:%s", ctx, metadata->index,
				metadata->artist, metadata->album, metadata->title,
				metadata->genre, div(metadata->duration, 1000).quot,
				div(metadata->duration,1000).rem, metadata->size,
				metadata->artwork ? metadata->artwork : "");

	return true;
}

/*--------------------------------------------------------------------------*/
u32_t sq_get_time(sq_dev_handle_t handle)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;
	u32_t time = 0;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle or CLI socket %d", ctx, handle);
		return 0;
	}

	sprintf(cmd, "%s time", ctx->cli_id);
	rsp = cli_send_cmd(cmd, true, true, ctx);
	if (rsp && *rsp) {
		time = (u32_t) (atof(rsp) * 1000);
	}
	else {
		LOG_ERROR("[%p] cannot gettime", ctx);
	}

	NFREE(rsp);
	return time;
}


/*---------------------------------------------------------------------------*/
bool sq_set_time(sq_dev_handle_t handle, char *pos)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128];
	char *rsp;

	if (!handle || !ctx->in_use) {
		LOG_ERROR("[%p]: no handle or cli socket %d", ctx, handle);
		return false;
	}

	sprintf(cmd, "%s time %s", ctx->cli_id, pos);
	LOG_INFO("[%p] time cmd %s", ctx, cmd);

	rsp = cli_send_cmd(cmd, false, true, ctx);
	if (!rsp) {
		LOG_ERROR("[%p] cannot settime %d", ctx, time);
		return false;
	}

	NFREE(rsp);
	return true;
}


/*--------------------------------------------------------------------------*/
void sq_notify(sq_dev_handle_t handle, sq_event_t event, ...)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];
	char cmd[128] = "", *rsp;

	LOG_SDEBUG("[%p] notif %d", ctx, event);

	// squeezelite device has not started yet or is off ...
	if (!ctx->running || !ctx->on || !handle || !ctx->in_use) return;

	va_list args;
	va_start(args, event);

	switch (event) {
		case SQ_PLAY_PAUSE: {
			sprintf(cmd, "%s pause", ctx->cli_id);
			break;
		}
		case SQ_PLAY: {
			sprintf(cmd, "%s play", ctx->cli_id);
			break;
		}
		case SQ_PAUSE: {
			sprintf(cmd, "%s mode", ctx->cli_id);
			rsp = cli_send_cmd(cmd, true, true, ctx);
			if (rsp && *rsp)
				sprintf(cmd, "%s pause %d", ctx->cli_id, strstr(rsp, "pause") ? 0 : 1);
			else
				sprintf(cmd, "%s pause", ctx->cli_id);
			break;
		}
		case SQ_STOP: {
			sprintf(cmd, "%s stop", ctx->cli_id);
			break;
		}
		case SQ_VOLUME: {
			char* volume = va_arg(args, char*);
			if (strstr(volume, "up")) sprintf(cmd, "%s mixer volume +5", ctx->cli_id);
			else if (strstr(volume, "down")) sprintf(cmd, "%s mixer volume -5", ctx->cli_id);
			else sprintf(cmd, "%s mixer volume %s", ctx->cli_id, volume);
			break;
		}
		case SQ_MUTE_TOGGLE: {
			sprintf(cmd, "%s mixer muting toggle", ctx->cli_id);
			break;
		}
		case SQ_PREVIOUS: {
			//sprintf(cmd, "%s playlist index -1", ctx->cli_id);
			sprintf(cmd, "%s button jump_rew", ctx->cli_id);
			break;
		}
		case SQ_NEXT: {
			sprintf(cmd, "%s playlist index +1", ctx->cli_id);
			break;
		}
		case SQ_SHUFFLE: {
			sprintf(cmd, "%s playlist shuffle 1", ctx->cli_id);
			break;
		}
		case SQ_FF_REW: {
			sprintf(cmd, "%s time %+d", ctx->cli_id, va_arg(args, s32_t));
			break;
		}
		case SQ_OFF: {
			sprintf(cmd, "%s power 0", ctx->cli_id);
			break;
		}

		default: break;
	 }

	va_end(args);

	if (*cmd) {
		rsp = cli_send_cmd(cmd, false, true, ctx);
		NFREE(rsp);
   }
}


/*---------------------------------------------------------------------------*/

void sq_init(struct in_addr host, char *model_name)
{
	sq_local_host = host;
	strcpy(sq_model_name, model_name);
	decode_init();
#if RESAMPLE
	soxr_loaded = register_soxr();
#endif
}

/*---------------------------------------------------------------------------*/
void sq_release_device(sq_dev_handle_t handle)
{
	if (handle) thread_ctx[handle - 1].in_use = false;
}

/*---------------------------------------------------------------------------*/
sq_dev_handle_t sq_reserve_device(void *MR, sq_callback_t callback)
{
	int ctx_i;
	struct thread_ctx_s *ctx;

	/* find a free thread context - this must be called in a LOCKED context */
	for  (ctx_i = 0; ctx_i < MAX_PLAYER; ctx_i++) if (!thread_ctx[ctx_i].in_use) break;

	if (ctx_i < MAX_PLAYER)	{
		// this sets a LOT of data to proper defaults (NULL, false ...)
		memset(&thread_ctx[ctx_i], 0, sizeof(struct thread_ctx_s));
		thread_ctx[ctx_i].in_use = true;
	} else {
		return false;
	}

	ctx = thread_ctx + ctx_i;
	ctx->self = ctx_i + 1;
	ctx->on = false;
	ctx->callback = callback;
	ctx->MR = MR;

	return ctx_i + 1;
}


/*---------------------------------------------------------------------------*/
bool sq_run_device(sq_dev_handle_t handle, struct raopcl_s *raopcl, sq_dev_param_t *param)
{
	struct thread_ctx_s *ctx = &thread_ctx[handle - 1];


	memcpy(&ctx->config, param, sizeof(sq_dev_param_t));

	sprintf(ctx->cli_id, "%02x:%02x:%02x:%02x:%02x:%02x",
						  ctx->config.mac[0], ctx->config.mac[1], ctx->config.mac[2],
						  ctx->config.mac[3], ctx->config.mac[4], ctx->config.mac[5]);

	if (stream_thread_init(ctx->config.streambuf_size, ctx) && output_raop_thread_init(raopcl, ctx->config.outputbuf_size, ctx)) {
		decode_thread_init(ctx);
	#if RESAMPLE
		if (param->resample) {
			process_init(param->resample_options, ctx);
		}
	#endif
		slimproto_thread_init(ctx);
		return true;
	} else {
		if (ctx->stream_running) stream_close(ctx);
		return false;
	}
}


/*--------------------------------------------------------------------------*/
void *sq_get_ptr(sq_dev_handle_t handle)
{
	if (!handle) return NULL;
	else return thread_ctx + handle - 1;
}



