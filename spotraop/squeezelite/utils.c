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
#include <fcntl.h>

// cmdline parsing
char *next_param(char *src, char c) {
	static char *str = NULL;
	char *ptr, *ret;
	if (src) str = src;
	if (str && (ptr = strchr(str, c))) {
		ret = str;
		*ptr = '\0';
		str = ptr + 1;
	} else {
		ret = str;
		str = NULL;
	}

	return ret && ret[0] ? ret : NULL;
}

void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr) {
	struct addrinfo *res = NULL;
	struct addrinfo hints;
	const char *port = NULL;
	
	if (strtok(server, ":")) {
		port = strtok(NULL, ":");
		if (port) {
			*port_ptr = atoi(port);
		}
	}

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	
	getaddrinfo(server, NULL, &hints, &res);
	
	if (res && res->ai_addr) {
		*ip_ptr = ((struct sockaddr_in*)res->ai_addr)->sin_addr.s_addr;
	} 
	
	if (res) {
		freeaddrinfo(res);
	}
}

void set_readwake_handles(event_handle handles[], sockfd s, event_event e) {
#if WINEVENT
	handles[0] = WSACreateEvent();
	handles[1] = e;
	WSAEventSelect(s, handles[0], FD_READ | FD_CLOSE);
#elif SELFPIPE  || LOOPBACK
	handles[0].fd = s;
	handles[1].fd = e.fds[0];
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#else
	handles[0].fd = s;
	handles[1].fd = e;
	handles[0].events = POLLIN;
	handles[1].events = POLLIN;
#endif
}

event_type wait_readwake(event_handle handles[], int timeout) {
#if WINEVENT
	int wait = WSAWaitForMultipleEvents(2, handles, false, timeout, false);
	if (wait == WSA_WAIT_EVENT_0) {
		WSAResetEvent(handles[0]);
		return EVENT_READ;
	} else if (wait == WSA_WAIT_EVENT_0 + 1) {
		return EVENT_WAKE;
	} else {
		return EVENT_TIMEOUT;
	}
#else
	if (poll(handles, 2, timeout) > 0) {
		if (handles[0].revents) {
			return EVENT_READ;
		}
		if (handles[1].revents) {
			wake_clear(handles[1].fd);
			return EVENT_WAKE;
		}
	}
	return EVENT_TIMEOUT;
#endif
}

#if LOOPBACK
void _wake_create(event_event* e) {
	struct sockaddr_in addr;
	short port;
	socklen_t len;

	e->mfds = e->fds[0] = e->fds[1] = -1;
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	// create sending socket - will wait for connections
	addr.sin_port = 0;
	e->mfds = socket(AF_INET, SOCK_STREAM, 0);
	bind(e->mfds, (struct sockaddr*) &addr, sizeof(addr));
	len = sizeof(struct sockaddr);

	// get assigned port & listen
	getsockname(e->mfds, (struct sockaddr *) &addr, &len);
	port = addr.sin_port;
	listen(e->mfds, 1);

	// create receiving socket
	addr.sin_port = 0;
	e->fds[0] = socket(AF_INET, SOCK_STREAM, 0);
	bind(e->fds[0], (struct sockaddr*) &addr, sizeof(addr));

	// connect to sender (we listen so it can't be blocking)
	addr.sin_port = port;
	connect(e->fds[0], (struct sockaddr*) &addr, sizeof(addr));

	// this one will work or fail, but not block
	len = sizeof(struct sockaddr);
	e->fds[1] = accept(e->mfds, (struct sockaddr*) &addr, &len);
}
#endif

// pack/unpack to network byte order
void packN(u32_t *dest, u32_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr)   = (val >> 24) & 0xFF; *(ptr+1) = (val >> 16) & 0xFF; *(ptr+2) = (val >> 8) & 0xFF;	*(ptr+3) = val & 0xFF;
}

void packn(u16_t *dest, u16_t val) {
	u8_t *ptr = (u8_t *)dest;
	*(ptr) = (val >> 8) & 0xFF; *(ptr+1) = val & 0xFF;
}

u32_t unpackN(u32_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 24 | *(ptr+1) << 16 | *(ptr+2) << 8 | *(ptr+3);
} 

u16_t unpackn(u16_t *src) {
	u8_t *ptr = (u8_t *)src;
	return *(ptr) << 8 | *(ptr+1);
} 

/*
u64_t timeval_to_ntp(struct timeval tv, struct ntp_s* ntp)
{
	struct ntp_s local;

	local.seconds = tv.tv_sec + 0x83AA7E80;
	local.fraction = (((u64_t)tv.tv_usec) << 32) / 1000000;

	if (ntp) *ntp = local;

	return (((u64_t)local.seconds) << 32) + local.fraction;
}

u64_t get_ntp(struct ntp_s* ntp)
{
	struct timeval ctv;

	gettimeofday(&ctv, NULL);
	return timeval_to_ntp(ctv, ntp);
}
*/
