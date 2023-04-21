/*
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2014, triode1@btinternet.com
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

// make may define: SELFPIPE, RESAMPLE, RESAMPLE_MP, LINKALL to influence build

// build detection
#include "platform.h"
#include "squeezedefs.h"

#if !defined(LOOPBACK)
#if LINUX && !defined(SELFPIPE)
#define EVENTFD   1
#define SELFPIPE  0
#define WINEVENT  0
#endif
#if (LINUX && !EVENTFD) || OSX || FREEBSD || SUNOS
#define EVENTFD   0
#define SELFPIPE  1
#define WINEVENT  0
#endif
#if WIN
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  1
#endif
#else
#define EVENTFD   0
#define SELFPIPE  0
#define WINEVENT  0
#undef LOOPBACK
#define LOOPBACK  1
#endif

#if !LINKALL

// dynamically loaded libraries at run time
#if LINUX || SUNOS
#define LIBFLAC "libFLAC.so.8"
#define LIBMAD  "libmad.so.0"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.3"
#define LIBOPUS "libopusfile.so.0"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#define LIBSOXR "libsoxr.so.0"
#endif

#if OSX
#define LIBFLAC "libFLAC.8.dylib"
#define LIBMAD  "libmad.0.dylib"
#define LIBMPG "libmpg123.0.dylib"
#define LIBVORBIS "libvorbisfile.3.dylib"
#define LIBTREMOR "libvorbisidec.1.dylib"
#define LIBOPUS "libopusfile.0.dylib"
#define LIBFAAD "libfaad.2.dylib"
#define LIBAVUTIL   "libavutil.%d.dylib"
#define LIBAVCODEC  "libavcodec.%d.dylib"
#define LIBAVFORMAT "libavformat.%d.dylib"
#define LIBSOXR "libsoxr.0.dylib"
#endif

#if WIN
#define LIBFLAC "libFLAC.dll"
#define LIBMAD  "libmad-0.dll"
#define LIBMPG "libmpg123-0.dll"
#define LIBVORBIS "libvorbisfile.dll"
#define LIBTREMOR "libvorbisidec.dll"
#define LIBOPUS "libopusfile-0.dll"
#define LIBFAAD "libfaad2.dll"
#define LIBAVUTIL   "avutil-%d.dll"
#define LIBAVCODEC  "avcodec-%d.dll"
#define LIBAVFORMAT "avformat-%d.dll"
#define LIBSOXR "libsoxr.dll"
#endif

#if FREEBSD
#define LIBFLAC "libFLAC.so.11"
#define LIBMAD  "libmad.so.2"
#define LIBMPG "libmpg123.so.0"
#define LIBVORBIS "libvorbisfile.so.6"
#define LIBTREMOR "libvorbisidec.so.1"
#define LIBOPUS "libopusfile.so.1"
#define LIBFAAD "libfaad.so.2"
#define LIBAVUTIL   "libavutil.so.%d"
#define LIBAVCODEC  "libavcodec.so.%d"
#define LIBAVFORMAT "libavformat.so.%d"
#endif

#endif // !LINKALL

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "squeezeitf.h"
#include "cross_log.h"
#include "cross_net.h"
#include "cross_util.h"

// we'll give venerable squeezelite the benefit of "old" int's
typedef uint64_t u64_t;
typedef int64_t  s64_t;
typedef uint32_t u32_t;
typedef int32_t  s32_t;
typedef uint16_t u16_t;
typedef int16_t  s16_t;
typedef uint8_t  u8_t;
typedef int8_t   s8_t;

#define SL_LITTLE_ENDIAN (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)

#define OUTPUTBUF_SIZE_CROSSFADE (OUTPUTBUF_SIZE * 12 / 10)

#define MAX_HEADER 4096 // do not reduce as icy-meta max is 4080

#define STREAM_THREAD_STACK_SIZE (1024 * 64)
#define DECODE_THREAD_STACK_SIZE (1024 * 128)
#define OUTPUT_THREAD_STACK_SIZE (1024 * 64)
#define SLIMPROTO_THREAD_STACK_SIZE  (1024 * 64)

#define mutex_type pthread_mutex_t
#define mutex_create(m) pthread_mutex_init(&m, NULL)
#define mutex_lock(m) pthread_mutex_lock(&m)
#define mutex_trylock(m) pthread_mutex_trylock(&m)
#define mutex_unlock(m) pthread_mutex_unlock(&m)
#define mutex_destroy(m) pthread_mutex_destroy(&m)
#define thread_type pthread_t
#if !WIN
#define mutex_create_p(m) \
	pthread_mutexattr_t attr; \
	pthread_mutexattr_init(&attr); \
	pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT); \
	pthread_mutex_init(&m, &attr); pthread_mutexattr_destroy(&attr)
#else
#define mutex_create_p mutex_create
#endif	

typedef u32_t frames_t;
typedef int sockfd;

#if !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

#if EVENTFD
#include <sys/eventfd.h>
#define event_event int
#define event_handle struct pollfd
#define wake_create(e) e = eventfd(0, 0)
#define wake_signal(e) eventfd_write(e, 1)
#define wake_clear(e) eventfd_t val; eventfd_read(e, &val)
#define wake_close(e) close(e)
#endif

#if SELFPIPE
#define event_handle struct pollfd
#define event_event struct wake
#define wake_create(e) pipe(e.fds); set_nonblock(e.fds[0]); set_nonblock(e.fds[1])
#define wake_signal(e) write(e.fds[1], ".", 1)
#define wake_clear(e) char c[10]; read(e, &c, 10)
#define wake_close(e) close(e.fds[0]); close(e.fds[1])
struct wake { 
	int fds[2];
};
#endif

#if LOOPBACK
#define event_handle struct pollfd
#define event_event struct wake
#define wake_create(e) _wake_create(&e)
#define wake_signal(e) send(e.fds[1], ".", 1, 0)
#define wake_clear(e) char c; recv(e, &c, 1, 0)
#define wake_close(e) closesocket(e.mfds); closesocket(e.fds[0]); closesocket(e.fds[1])
struct wake {
	int mfds;
	int fds[2];
};
void _wake_create(event_event*);
#endif

#if WINEVENT
#define event_event HANDLE
#define event_handle HANDLE
#define wake_create(e) e = CreateEvent(NULL, FALSE, FALSE, NULL)
#define wake_signal(e) SetEvent(e)
#define wake_close(e) CloseHandle(e)
#endif

#define MAX_SILENCE_FRAMES FRAMES_PER_BLOCK 		// 352 for RAOP protocol
#define FIXED_ONE  0x10000
#define MONO_RIGHT	0x02
#define MONO_LEFT	0x01

#define BYTES_PER_FRAME 4

// utils.c (non logging)
typedef enum { EVENT_TIMEOUT = 0, EVENT_READ, EVENT_WAKE } event_type;
struct thread_ctx_s;

char *next_param(char *src, char c);
u32_t gettime_ms(void);
void get_mac(u8_t *mac);
void set_nonblock(sockfd s);
int connect_timeout(sockfd sock, const struct sockaddr *addr, socklen_t addrlen, int timeout);
void server_addr(char *server, in_addr_t *ip_ptr, unsigned *port_ptr);
void set_readwake_handles(event_handle handles[], sockfd s, event_event e);
event_type wait_readwake(event_handle handles[], int timeout);
void packN(u32_t *dest, u32_t val);
void packn(u16_t *dest, u16_t val);
u32_t unpackN(u32_t *src);
u16_t unpackn(u16_t *src);
u32_t position_ms(struct thread_ctx_s *ctx, u32_t *ref);
#if OSX
void set_nosigpipe(sockfd s);
#else
#define set_nosigpipe(s)
#endif
#if WIN
void winsock_init(void);
void winsock_close(void);
void *dlopen(const char *filename, int flag);
void dlclose(void *handle);
void *dlsym(void *handle, const char *symbol);
char *dlerror(void);
int poll(struct pollfd *fds, unsigned long numfds, int timeout);
#endif
#if LINUX || FREEBSD
void touch_memory(u8_t *buf, size_t size);
#endif

// buffer.c
struct buffer {
	u8_t *buf;
	u8_t *readp;
	u8_t *writep;
	u8_t *wrap;
	size_t size;
	size_t base_size;
	mutex_type mutex;
};

// _* called with mutex locked
unsigned _buf_used(struct buffer *buf);
unsigned _buf_space(struct buffer *buf);
unsigned _buf_cont_read(struct buffer *buf);
unsigned _buf_cont_write(struct buffer *buf);
void _buf_inc_readp(struct buffer *buf, unsigned by);
void _buf_inc_writep(struct buffer *buf, unsigned by);
unsigned _buf_read(void *dst, struct buffer *src, unsigned btes);
int	 _buf_seek(struct buffer *src, unsigned from, unsigned by);
void _buf_move(struct buffer *buf, unsigned by);
void _buf_unwrap(struct buffer *buf, size_t cont);
void buf_flush(struct buffer *buf);
void buf_adjust(struct buffer *buf, size_t mod);
void _buf_resize(struct buffer *buf, size_t size);
void buf_init(struct buffer *buf, size_t size);
void buf_destroy(struct buffer *buf);

// slimproto.c
void slimproto_close(struct thread_ctx_s *ctx);
void slimproto_reset(struct thread_ctx_s *ctx);
void slimproto_thread_init(struct thread_ctx_s *ctx);
void wake_controller(struct thread_ctx_s *ctx);
void send_packet(u8_t *packet, size_t len, sockfd sock);
void wake_controller(struct thread_ctx_s *ctx);

// stream.c
typedef enum { STOPPED = 0, DISCONNECT, STREAMING_WAIT,
			   STREAMING_BUFFERING, STREAMING_FILE, STREAMING_HTTP, SEND_HEADERS, RECV_HEADERS } stream_state;
typedef enum { DISCONNECT_OK = 0, LOCAL_DISCONNECT = 1, REMOTE_DISCONNECT = 2, UNREACHABLE = 3, TIMEOUT = 4 } disconnect_code;

struct streamstate {
	stream_state state;
	disconnect_code disconnect;
	char *header;
	size_t header_len;
	int	endtok;
	bool sent_headers;
	bool cont_wait;
	u64_t bytes;
	u32_t last_read;
	unsigned threshold;
	u32_t meta_interval;
	u32_t meta_next;
	u32_t meta_left;
	bool  meta_send;
	size_t header_mlen;
	struct sockaddr_in addr;
	char host[256];
};

bool stream_thread_init(unsigned buf_size, struct thread_ctx_s *ctx);
void stream_close(struct thread_ctx_s *ctx);
void stream_file(const char *header, size_t header_len, unsigned threshold, struct thread_ctx_s *ctx);
void 		stream_sock(u32_t ip, u16_t port, bool use_ssl, const char *header, size_t header_len, unsigned threshold, bool cont_wait, struct thread_ctx_s *ctx);
bool stream_disconnect(struct thread_ctx_s *ctx);

// decode.c
typedef enum { DECODE_STOPPED = 0, DECODE_READY, DECODE_RUNNING, DECODE_COMPLETE, DECODE_ERROR } decode_state;

struct decodestate {
	decode_state state;
	bool new_stream;
	mutex_type mutex;
	void *handle;
#if PROCESS
	void *process_handle;
	bool direct;
	bool process;
#endif
};

#if PROCESS
struct processstate {
	u8_t *inbuf, *outbuf;
	unsigned max_in_frames, max_out_frames;
	unsigned in_frames, out_frames;
	unsigned in_sample_rate, out_sample_rate;
	unsigned long total_in, total_out;
};
#endif

struct codec {
	char id;
	char *types;
	unsigned min_read_bytes;
	unsigned min_space;
	void (*open)(u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx);
	void (*close)(struct thread_ctx_s *ctx);
	decode_state (*decode)(struct thread_ctx_s *ctx);
};

void decode_init(void);
void decode_end(void);
void decode_thread_init(struct thread_ctx_s *ctx);

void decode_close(struct thread_ctx_s *ctx);
void decode_flush(struct thread_ctx_s *ctx);
unsigned decode_newstream(unsigned sample_rate, int supported_rates[], struct thread_ctx_s *ctx);
bool codec_open(u8_t codec, u8_t sample_size, u32_t sample_rate, u8_t channels, u8_t endianness, struct thread_ctx_s *ctx);

#if PROCESS
// process.c
void process_samples(struct thread_ctx_s *ctx);
void process_drain(struct thread_ctx_s *ctx);
void process_flush(struct thread_ctx_s *ctx);
unsigned process_newstream(bool *direct, unsigned raw_sample_rate, int supported_rates[], struct thread_ctx_s *ctx);
void process_init(char *opt, struct thread_ctx_s *ctx);
void process_end(struct thread_ctx_s *ctx);
#endif

#if RESAMPLE
// resample.c
void resample_samples(struct thread_ctx_s *ctx);
bool resample_drain(struct thread_ctx_s *ctx);
bool resample_newstream(unsigned raw_sample_rate, int supported_rates[], struct thread_ctx_s *ctx);
void resample_flush(struct thread_ctx_s *ctx);
bool resample_init(char *opt, struct thread_ctx_s *ctx);
void resample_end(struct thread_ctx_s *ctx);
#endif

// output.c output_pack.c
typedef enum { OUTPUT_OFF = -1, OUTPUT_STOPPED = 0, OUTPUT_BUFFER, OUTPUT_RUNNING,
			   OUTPUT_PAUSE_FRAMES, OUTPUT_SKIP_FRAMES } output_state;

typedef enum { FADE_INACTIVE = 0, FADE_DUE, FADE_ACTIVE } fade_state;
typedef enum { FADE_UP = 1, FADE_DOWN, FADE_CROSS } fade_dir;
typedef enum { FADE_NONE = 0, FADE_CROSSFADE, FADE_IN, FADE_OUT, FADE_INOUT } fade_mode;


struct outputstate {
	output_state state;
	void *device;
	bool  track_started;
	int (* write_cb)(struct thread_ctx_s *ctx, frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags, s32_t cross_gain_in, s32_t cross_gain_out, s16_t **cross_ptr);
	unsigned start_frames;
	unsigned frames_played;
	unsigned frames_played_dmp;// frames played at the point delay is measured
	u32_t device_frames;
	unsigned current_sample_rate;
	unsigned default_sample_rate;
	int supported_rates[2];
	bool error_opening;
	u32_t updated;
	u32_t track_start_time;
	u32_t current_replay_gain;
	// was union
	union {
		u32_t pause_frames;
		u32_t skip_frames;
		u32_t start_at;
	};
	u8_t  *track_start;        // set in decode thread
	bool  detect_start_time;   // use in audio extractor
	u32_t gainL;               // set by slimproto
	u32_t gainR;               // set by slimproto
	u32_t next_replay_gain;    // set by slimproto
	unsigned threshold;        // set by slimproto
	fade_state fade;
	u8_t *fade_start;
	u8_t *fade_end;
	fade_dir fade_dir;
	fade_mode fade_mode;       // set by slimproto
	unsigned fade_secs;        // set by slimproto
	bool delay_active;
	int buf_frames;
	u8_t *buf;
	u8_t channels;
};

void output_init(const char *device, unsigned output_buf_size, unsigned rates[], struct thread_ctx_s *ctx);
void output_close(struct thread_ctx_s *ctx);
void output_flush(struct thread_ctx_s *ctx);
// _* called with mutex locked
frames_t _output_frames(frames_t avail, struct thread_ctx_s *ctx);
void _checkfade(bool, struct thread_ctx_s *ctx);
void wake_output(struct thread_ctx_s *ctx);

// output_raop.c
void output_init_common(void *device, unsigned output_buf_size, u32_t sample_rate, struct thread_ctx_s *ctx);
bool output_raop_thread_init(struct raopcl_s *raopcl, unsigned output_buf_size, struct thread_ctx_s *ctx);
void output_close_common(struct thread_ctx_s *ctx);

// output_pack.c
void _scale_frames(s16_t *outputptr, s16_t *inputptr, frames_t cnt, s32_t gainL, s32_t gainR, u8_t flags);
void _apply_cross(struct buffer *outputbuf, frames_t out_frames, s32_t cross_gain_in, s32_t cross_gain_out, s16_t **cross_ptr);
s32_t gain32(s32_t gain, s32_t value);
s32_t to_gain(float f);

// dop.c
#if DSD
bool is_flac_dop(u32_t *lptr, u32_t *rptr, frames_t frames);
void update_dop_marker(u32_t *ptr, frames_t frames);
void dop_silence_frames(u32_t *ptr, frames_t frames);
void dop_init(bool enable, unsigned delay);
#endif


/***************** main thread context**************/
typedef struct {
	u32_t updated;
	u32_t stream_start;			// vf : now() when stream started
	u32_t stream_full;			// v : unread bytes in stream buf
	u32_t stream_size;			// f : streambuf_size init param
	u64_t stream_bytes;         // v : bytes received for current stream
	u32_t output_full;			// v : unread bytes in output buf
	u32_t output_size;			// f : output_buf_size init param
	u32_t frames_played;        // number of samples (bytes / sample size) played
	u32_t current_sample_rate;
	u32_t last;
	stream_state stream_state;
	u32_t device_frames;
} status_t;

typedef enum {TRACK_STOPPED = 0, TRACK_STARTED, TRACK_PAUSED} track_status_t;

#define SERVER_VERSION_LEN	32
#define MAX_PLAYER		32

struct thread_ctx_s {
	int 	self;
	int 	autostart;
	bool	running;
	bool	in_use;
	bool	on;
	char   last_command;
	sq_dev_param_t	config;
	mutex_type mutex;
	bool 	sentSTMu, sentSTMo, sentSTMl, sentSTMd;
	u32_t 	new_server;
	char 	*new_server_cap;
	char	fixed_cap[128], var_cap[128];
	status_t			status;
	struct streamstate	stream;
	struct outputstate 	output;
	struct decodestate 	decode;
#if PROCESS
	struct processstate	process;
#endif
	struct codec		*codec;
	struct buffer		__s_buf;
	struct buffer		__o_buf;
	struct buffer		*streambuf;
	struct buffer		*outputbuf;
	unsigned outputbuf_size, streambuf_size;
	in_addr_t 	slimproto_ip;
	unsigned 	slimproto_port;
	char		server_version[SERVER_VERSION_LEN + 1];
	char		server_port[5+1];
	char		server_ip[4*(3+1)+1];
	u16_t		cli_port;
	sockfd 		sock, fd, cli_sock;
#if USE_SSL
	void		*ssl;
	bool		ssl_error;
#endif
	char		cli_id[18];		// (6*2)+(5*':')+NULL
	mutex_type	cli_mutex;
	u32_t		cli_timeout;
	int bytes_per_frame;		// for output
	bool	output_running;		// for output.c
	bool	stream_running;		// for stream.c
	bool	decode_running;		// for decode.c
	thread_type output_thread;	// output.c child thread
	thread_type stream_thread;	// stream.c child thread
	thread_type decode_thread;	// decode.c child thread
	thread_type	thread;			// main instance thread
	struct sockaddr_in serv_addr;
	#define MAXBUF 4096
	event_event	wake_e;
	struct 	{				// scratch memory for slimprot_run (was static)
		 u8_t 	buffer[MAXBUF];
		 u32_t	last;
		 char	header[MAX_HEADER];
	} slim_run;
	sq_callback_t	callback;
	void			*MR;
	u8_t *silencebuf;
};

extern struct thread_ctx_s 	thread_ctx[MAX_PLAYER];
extern char					sq_model_name[];
extern struct in_addr		sq_local_host;
extern u16_t 				sq_local_port;

// codecs
#define MAX_CODECS 16

extern struct codec *codecs[MAX_CODECS];

struct codec*	register_flac(void);
void		 	deregister_flac(void);
struct codec*	register_pcm(void);
void 		 	deregister_pcm(void);
struct codec*	register_mad(void);
void 			deregister_mad(void);
struct codec*	register_mpg(void);
void			deregister_mpg(void);
struct codec*	register_faad(void);
void 			deregister_faad(void);
struct codec*	register_vorbis(void);
void 			deregister_vorbis(void);
struct codec*	register_alac(void);
void 			deregister_alac(void);
struct codec*	register_opus(void);
void 			deregister_opus(void);

#if RESAMPLE
bool register_soxr(void);
void deregister_soxr(void);
#endif
extern bool soxr_loaded;



