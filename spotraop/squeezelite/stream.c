/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *  (c) Philippe, philippe_44@outlook.com for multi-instance modifications
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

// stream thread

#include "squeezelite.h"

#include <fcntl.h>

#if USE_SSL
#include "openssl/ssl.h"
#include "openssl/err.h"
#endif

extern log_level	stream_loglevel;
static log_level 	*loglevel = &stream_loglevel;

#define LOCK_S   mutex_lock(ctx->streambuf->mutex)
#define UNLOCK_S mutex_unlock(ctx->streambuf->mutex)

#if USE_SSL

static SSL_CTX *SSLctx = NULL;
static int SSLcount = 0;

static int _last_error(struct thread_ctx_s* ctx) {
	if (!ctx->ssl) return last_error();
	return ctx->ssl_error ? ECONNABORTED : ERROR_WOULDBLOCK;
}

static int _recv(struct thread_ctx_s *ctx, void *buffer, size_t bytes, int options) {
	if (!ctx->ssl) return recv(ctx->fd, buffer, bytes, options);
	int n = SSL_read(ctx->ssl, (u8_t*) buffer, bytes);
	if (n <= 0) {
		int err = SSL_get_error(ctx->ssl, n);
		if (err == SSL_ERROR_ZERO_RETURN) return 0;
		ctx->ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	}
	return n;
}

static int _send(struct thread_ctx_s *ctx, void *buffer, size_t bytes, int options) {
	if (!ctx->ssl) return send(ctx->fd, buffer, bytes, options);
	int n = 0;
	do {
		ERR_clear_error();
		if ((n = SSL_write(ctx->ssl, (u8_t*) buffer, bytes)) >= 0) break;
		int err = SSL_get_error(ctx->ssl, n);
		ctx->ssl_error = (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE);
	} while (!ctx->ssl_error);
	return n;
}

/*
can't mimic exactly poll as SSL is a real pain. Even if SSL_pending returns
0, there might be bytes to read but when select (poll) return > 0, there might
be no frame available. As well select (poll) < 0 does not mean that there is
no data pending
*/
static int _poll(struct thread_ctx_s *ctx, struct pollfd *pollinfo, int timeout) {
	if (!ctx->ssl) return poll(pollinfo, 1, timeout);
	if (pollinfo->events & POLLIN && SSL_pending(ctx->ssl)) {
		if (pollinfo->events & POLLOUT) poll(pollinfo, 1, 0);
		pollinfo->revents = POLLIN;
		return 1;
	}
	return poll(pollinfo, 1, timeout);
}
#else
#define _recv(ctx, buf, n, opt) recv(ctx->fd, buf, n, opt)
#define _send(ctx, buf, n, opt) send(ctx->fd, buf, n, opt)
#define _poll(ctx, pollinfo, timeout) poll(pollinfo, 1, timeout)
#define _last_error(x) last_error()
#endif

static bool send_header(struct thread_ctx_s *ctx) {
	char *ptr = ctx->stream.header;
	int len = ctx->stream.header_len;

	unsigned try = 0;
	ssize_t n;

	while (len) {
		n = _send(ctx, ptr, len, MSG_NOSIGNAL);
		if (n <= 0) {
			if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK && try < 10) {
				LOG_SDEBUG("[%p] retrying (%d) writing to socket", ctx, ++try);
				usleep(1000);
				continue;
			}
			LOG_WARN("[%p] failed writing to socket: %s", ctx, strerror(_last_error(ctx)));
			ctx->stream.disconnect = LOCAL_DISCONNECT;
			ctx->stream.state = DISCONNECT;
			wake_controller(ctx);
			return false;
		}
		LOG_SDEBUG("[%p] wrote %d bytes to socket", ctx, n);
		ptr += n;
		len -= n;
	}
	LOG_SDEBUG("[%p] wrote header", ctx);
	return true;
}

bool stream_disconnect(struct thread_ctx_s *ctx) {
	bool disc = false;
	LOCK_S;
#if USE_SSL
	if (ctx->ssl) {
		SSL_shutdown(ctx->ssl);
		SSL_free(ctx->ssl);
		ctx->ssl = NULL;
	}
#endif
	if (ctx->fd != -1) {
		closesocket(ctx->fd);
		ctx->fd = -1;
		disc = true;
	}
	ctx->stream.state = STOPPED;
	UNLOCK_S;
	return disc;
}

static void _disconnect(stream_state state, disconnect_code disconnect, struct thread_ctx_s *ctx) {
	ctx->stream.state = state;
	ctx->stream.disconnect = disconnect;
#if USE_SSL
	if (ctx->ssl) {
		SSL_shutdown(ctx->ssl);
		SSL_free(ctx->ssl);
		ctx->ssl = NULL;
	}
#endif
	closesocket(ctx->fd);
	ctx->fd = -1;
	wake_controller(ctx);
}

static int connect_socket(bool use_ssl, struct thread_ctx_s *ctx) {
	int sock = socket(AF_INET, SOCK_STREAM, 0);

	LOG_INFO("[%p] connecting to %s:%d", ctx, inet_ntoa(ctx->stream.addr.sin_addr), ntohs(ctx->stream.addr.sin_port));

	if (sock < 0) {
		LOG_ERROR("[%p] failed to create socket", ctx);
		return sock;
	}

	/* This is to force at least Windows to not have gigantic TCP buffer that cause a
	   FIN_WAIT_2 timeout on low bitrate files of a certain size. With no set of the value,
	   Windows uses a different amount than what it reports... oh well
	*/
	unsigned int opt, len = sizeof(opt);
	getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&opt, &len);
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (void*)&opt, sizeof(opt));
	LOG_INFO("[%p] set SO_RCVBUF at %d bytes", ctx, opt);

	set_nonblock(sock);
	set_nosigpipe(sock);

	if (tcp_connect_timeout(sock, ctx->stream.addr, 10*1000) < 0) {
		LOG_WARN("[%p] unable to connect to server", ctx);
		closesocket(sock);
		return -1;
	}

#if USE_SSL
	if (use_ssl) {
		ctx->ssl = SSL_new(SSLctx);
		SSL_set_fd(ctx->ssl, sock);

		// add SNI
		if (*ctx->stream.host) SSL_set_tlsext_host_name(ctx->ssl, ctx->stream.host);

		// try to connect (socket is non-blocking)
		while (1) {
			int status, err = 0;

			ERR_clear_error();
			status = SSL_connect(ctx->ssl);

			// successful negotiation
			if (status == 1) break;

			// error or non-blocking requires more time
			if (status < 0) {
				err = SSL_get_error(ctx->ssl, status);
				if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
			}

			LOG_WARN("[%p]: unable to open SSL socket %d (%d)", ctx, status, err);

			closesocket(sock);
			SSL_free(ctx->ssl);
			ctx->ssl = NULL;

			return -1;
		}
		LOG_INFO("[%p]: streaming with SSL", ctx);
	} else ctx->ssl = NULL;
#endif

	return sock;
}

static void *stream_thread(struct thread_ctx_s *ctx) {
	while (ctx->stream_running) {

		struct pollfd pollinfo;
		size_t space;

		LOCK_S;

		/*
		It is required to use min with buf_space as it is the full space - 1,
		otherwise, a write to full would be authorized and the write pointer
		would wrap to the read pointer, making impossible to know if the buffer
		is full or empty. This has the consequence, though, that the buffer can
		never be totally full and can only wrap once the read pointer has moved
		so it is impossible to count on having a proper multiply of any number
		of bytes in the buffer
		*/
		space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

		if (ctx->fd < 0 || !space || ctx->stream.state <= STREAMING_WAIT) {
			UNLOCK_S;
			usleep(100 * 1000);
			continue;
		}

		if (ctx->stream.state == STREAMING_FILE) {

			int n = read(ctx->fd, ctx->streambuf->writep, space);
			if (n == 0) {
				LOG_INFO("[%p] end of stream", ctx);
				_disconnect(DISCONNECT, DISCONNECT_OK, ctx);
			}
			if (n > 0) {
				_buf_inc_writep(ctx->streambuf, n);
				ctx->stream.bytes += n;
				LOG_SDEBUG("[%p] ctx->streambuf read %d bytes", ctx, n);
			}
			if (n < 0) {
				LOG_WARN("[%p] error reading: %s", ctx, strerror(_last_error(ctx)));
				_disconnect(DISCONNECT, REMOTE_DISCONNECT, ctx);
			}

			UNLOCK_S;
			continue;

		} else {

			pollinfo.fd = ctx->fd;
			pollinfo.events = POLLIN;
			if (ctx->stream.state == SEND_HEADERS) {
				pollinfo.events |= POLLOUT;
			}
		}

		UNLOCK_S;

		if (_poll(ctx, &pollinfo, 100)) {

			LOCK_S;

			// check socket has not been closed while in poll
			if (ctx->fd < 0) {
				UNLOCK_S;
				continue;
			}

			if ((pollinfo.revents & POLLOUT) && ctx->stream.state == SEND_HEADERS) {
				if (send_header(ctx)) ctx->stream.state = RECV_HEADERS;
				ctx->stream.header_mlen = ctx->stream.header_len;
				ctx->stream.header_len = 0;
				UNLOCK_S;
				continue;
			}

			if (pollinfo.revents & (POLLIN | POLLHUP)) {

				// get response headers
				if (ctx->stream.state == RECV_HEADERS) {

					// read one byte at a time to catch end of header
					char c;

					int n = _recv(ctx, &c, 1, 0);
					if (n <= 0) {
						if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
							UNLOCK_S;
							continue;
						}
						LOG_WARN("[%p] error reading headers: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
#if USE_SSL
						if (!ctx->ssl && !ctx->stream.header_len) {
							int sock;

							// let's restart with SSL this time
							ctx->stream.header_len = ctx->stream.header_mlen;
							closesocket(ctx->fd);
							ctx->fd = -1;
							LOG_INFO("[%p] now attempting with SSL", ctx);

							// stay locked for slimproto (I know it can be long)
							sock = connect_socket(true, ctx);

							if (sock >= 0) {
								ctx->fd = sock;
								ctx->stream.state = SEND_HEADERS;
								UNLOCK_S;
								continue;
							}
						}
#endif
						_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
						UNLOCK_S;
						continue;
					}

					*(ctx->stream.header + ctx->stream.header_len) = c;
					ctx->stream.header_len++;

					if (ctx->stream.header_len > MAX_HEADER - 1) {
						LOG_ERROR("[%p] received headers too long: %u", ctx, ctx->stream.header_len);
						_disconnect(DISCONNECT, LOCAL_DISCONNECT, ctx);
					}

					if (ctx->stream.header_len > 1 && (c == '\r' || c == '\n')) {
						ctx->stream.endtok++;
						if (ctx->stream.endtok == 4) {
							*(ctx->stream.header + ctx->stream.header_len) = '\0';
							LOG_INFO("[%p] headers: len: %d\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
							ctx->stream.state = ctx->stream.cont_wait ? STREAMING_WAIT : STREAMING_BUFFERING;
							wake_controller(ctx);
						}
					} else {
						ctx->stream.endtok = 0;
					}

					UNLOCK_S;
					continue;
				}

				// receive icy meta data

				if (ctx->stream.meta_interval && ctx->stream.meta_next == 0) {
					if (ctx->stream.meta_left == 0) {
						// read meta length
						u8_t c;
						int n = _recv(ctx, &c, 1, 0);
						if (n <= 0) {
							if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p] error reading icy meta: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
							UNLOCK_S;
							continue;
						}
						ctx->stream.meta_left = 16 * c;
						ctx->stream.header_len = 0; // amount of received meta data
						// MAX_HEADER must be more than meta max of 16 * 255
					}

					if (ctx->stream.meta_left) {
						int n = _recv(ctx, ctx->stream.header + ctx->stream.header_len, ctx->stream.meta_left, 0);
						if (n <= 0) {
							if (n < 0 && _last_error(ctx) == ERROR_WOULDBLOCK) {
								UNLOCK_S;
								continue;
							}
							LOG_WARN("[%p] error reading icy meta: %s", ctx, n ? strerror(_last_error(ctx)) : "closed");
							_disconnect(STOPPED, LOCAL_DISCONNECT, ctx);
							UNLOCK_S;
							continue;
						}
						ctx->stream.meta_left -= n;
						ctx->stream.header_len += n;
					}

					if (ctx->stream.meta_left == 0) {
						if (ctx->stream.header_len) {
							*(ctx->stream.header + ctx->stream.header_len) = '\0';
							LOG_INFO("[%p] icy meta: len: %u\n%s", ctx, ctx->stream.header_len, ctx->stream.header);
							ctx->stream.meta_send = true;
							wake_controller(ctx);
						}
						ctx->stream.meta_next = ctx->stream.meta_interval;
						UNLOCK_S;
						continue;
					}

				// stream body into streambuf
				} else {
					space = min(_buf_space(ctx->streambuf), _buf_cont_write(ctx->streambuf));

					if (ctx->stream.meta_interval) {
						space = min(space, ctx->stream.meta_next);
					}

					int n = _recv(ctx, ctx->streambuf->writep, space, 0);
					if (n == 0) {
						LOG_INFO("[%p] end of stream (t:%lld)", ctx, ctx->stream.bytes);
						_disconnect(DISCONNECT, DISCONNECT_OK, ctx);
					}
					if (n < 0 && _last_error(ctx) != ERROR_WOULDBLOCK) {
						LOG_WARN("[%p] error reading: %s (%d)", ctx, strerror(_last_error(ctx)), _last_error(ctx));
						_disconnect(DISCONNECT, REMOTE_DISCONNECT, ctx);
					}

					if (n > 0) {
						_buf_inc_writep(ctx->streambuf, n);
						ctx->stream.bytes += n;
						wake_output(ctx);
						if (ctx->stream.meta_interval) {
							ctx->stream.meta_next -= n;
						}
					} else {
						UNLOCK_S;
						continue;
					}

					if (ctx->stream.state == STREAMING_BUFFERING && ctx->stream.bytes > ctx->stream.threshold) {
						ctx->stream.state = STREAMING_HTTP;
						wake_controller(ctx);
					}

					LOG_DEBUG("[%p] streambuf read %d bytes", ctx, n);
				}
			}

			UNLOCK_S;

		}
		else {
			LOG_SDEBUG("[%p] poll timeout", ctx);
		}
	}

#if USE_SSL
	if (!--SSLcount) {
		SSL_CTX_free(SSLctx);
		SSLctx = NULL;
	}
#endif

	return 0;
}


/*---------------------------------------------------------------------------*/
bool stream_thread_init(unsigned streambuf_size, struct thread_ctx_s *ctx) {

	pthread_attr_t attr;

	LOG_DEBUG("[%p] streambuf size: %u", ctx, streambuf_size);

	ctx->streambuf = &ctx->__s_buf;

	buf_init(ctx->streambuf, ((streambuf_size / (BYTES_PER_FRAME * 3)) * BYTES_PER_FRAME * 3));
	if (ctx->streambuf->buf == NULL) {
		LOG_ERROR("[%p] unable to malloc buffer", ctx);
		return false;
	}

#if USE_SSL
	if (!SSLctx) {
		SSLctx = SSL_CTX_new(SSLv23_client_method());
		if (SSLctx) SSL_CTX_set_options(SSLctx, SSL_OP_NO_SSLv2);
	}
	SSLcount++;
	ctx->ssl = NULL;
#endif

	ctx->stream_running = true;
	ctx->stream.state = STOPPED;
	ctx->stream.header = malloc(MAX_HEADER);
	ctx->stream.header[0] = '\0';
	ctx->fd = -1;

	touch_memory(ctx->streambuf->buf, ctx->streambuf->size);

	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN + STREAM_THREAD_STACK_SIZE);
	pthread_create(&ctx->stream_thread, &attr, (void *(*)(void*)) stream_thread, ctx);
	pthread_attr_destroy(&attr);

	return true;
}

void stream_close(struct thread_ctx_s *ctx) {
	LOG_INFO("[%p] close stream", ctx);
	LOCK_S;
	ctx->stream_running = false;
	UNLOCK_S;
	pthread_join(ctx->stream_thread, NULL);
	free(ctx->stream.header);
	buf_destroy(ctx->streambuf);
}

void stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx) {
	buf_flush(ctx->streambuf);

	LOCK_S;

	ctx->stream.header_len = header_len;
	memcpy(ctx->stream.header, header, header_len);
	*(ctx->stream.header+header_len) = '\0';

	LOG_INFO("[%p] opening local file: %s", ctx, ctx->stream.header);

#if WIN
	ctx->fd = open(ctx->stream.header, O_RDONLY | O_BINARY);
#else
	ctx->fd = open(ctx->stream.header, O_RDONLY);
#endif

	ctx->stream.state = STREAMING_FILE;
	if (ctx->fd < 0) {
		LOG_WARN("[%p] can't open file: %s", ctx, ctx->stream.header);
		ctx->stream.state = DISCONNECT;
	}
	wake_controller(ctx);

	ctx->stream.cont_wait = false;
	ctx->stream.meta_interval = 0;
	ctx->stream.meta_next = 0;
	ctx->stream.meta_left = 0;
	ctx->stream.meta_send = false;
	ctx->stream.sent_headers = false;
	ctx->stream.bytes = 0;
	ctx->stream.threshold = threshold;

	UNLOCK_S;
}

void stream_sock(u32_t ip, u16_t port, bool use_ssl, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx) {
	int sock;
	char *p;

	memset(&ctx->stream.addr, 0, sizeof(ctx->stream.addr));
	ctx->stream.addr.sin_family = AF_INET;
	ctx->stream.addr.sin_addr.s_addr = ip;
	ctx->stream.addr.sin_port = port;

	*ctx->stream.host = '\0';
	if ((p = strcasestr(header,"Host:")) != NULL) {
		sscanf(p, "Host:%255s", ctx->stream.host);
		if ((p = strchr(ctx->stream.host, ':')) != NULL) *p = '\0';
	}

	port = ntohs(port);
	sock = connect_socket(use_ssl || port == 443, ctx);

	// try one more time with plain socket
	if (sock < 0 && port == 443 && !use_ssl) sock = connect_socket(false, ctx);

	if (sock < 0) {
		LOCK_S;
		ctx->stream.state = DISCONNECT;
		ctx->stream.disconnect = UNREACHABLE;
		UNLOCK_S;
		return;
	}

	buf_flush(ctx->streambuf);

	LOCK_S;

	ctx->fd = sock;
	ctx->stream.state = SEND_HEADERS;
	ctx->stream.cont_wait = cont_wait;
	ctx->stream.meta_interval = 0;
	ctx->stream.meta_next = 0;
	ctx->stream.meta_left = 0;
	ctx->stream.meta_send = false;
	ctx->stream.header_len = header_len;
	memcpy(ctx->stream.header, header, header_len);
	*(ctx->stream.header+header_len) = '\0';

	LOG_INFO("[%p] header: %s", ctx, ctx->stream.header);

	ctx->stream.sent_headers = false;
	ctx->stream.bytes = 0;
	ctx->stream.threshold = threshold;

	UNLOCK_S;
}


