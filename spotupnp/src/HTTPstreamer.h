/*
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#pragma once

#include <string>
#include <memory>
#include <inttypes.h>
#include <map>
#include <functional>

#include "BellTask.h"
#include "CDNTrackStream.h"
#ifdef _WIN32
#include "win32shim.h"
#endif

#include "HTTPmode.h"
#include "metadata.h"
#include "codecs.h"

typedef std::map<std::string, std::string> HTTPheaders;
typedef std::function<HTTPheaders(HTTPheaders)> onHeadersHandler;

/****************************************************************************************
 * Ring buffer (always rolls over)
 */
class ringBuffer {
private:
    uint8_t* buffer;
    uint8_t* read_p, * write_p, * wrap_p;

public:
    size_t size, total = 0;

    ringBuffer(size_t size = 5 * 1024 * 1024);
    ~ringBuffer(void) { delete[] buffer; }
    size_t used(void) { return write_p >= read_p ? write_p - read_p : size - (read_p - write_p); }
    size_t read(uint8_t* dst, size_t max, size_t min = 0);
    uint8_t* readInner(size_t& size);
    void setReadPtr(size_t offset) { read_p = buffer + (offset % this->size); }
    void write(const uint8_t* src, size_t size);
    void flush(void) { read_p = write_p = buffer; total = 0; }
};

/****************************************************************************************
 * Class to stream audio content with HTTP
 */
class HTTPstreamer : public bell::Task {
private:
    std::atomic<bool> isRunning = false;
    std::mutex runningMutex;
    std::string host;
    int listenSock = -1;
    uint16_t port;
    HTTPheaders headers;
    int64_t contentLength = HTTP_CL_NONE;
    std::unique_ptr<baseCodec> encoder;
    ringBuffer cache;
    size_t useCache;
    uint8_t scratch[32768];
    bool flow;
    struct {
        size_t interval, remain;
        size_t size, count;
        std::string trackId;
    } icy;

    void runTask();
    ssize_t streamBody(int sock, struct timeval& timeout);
    ssize_t sendChunk(int sock, uint8_t* data, ssize_t size);
    void getMetadata(cspot::CDNTrackStream::TrackInfo& track, metadata_t* metadata);
    onHeadersHandler onHeaders;
public:
    enum states { OFF, CONNECTING, STREAMING, DRAINING, DRAINED };
    std::atomic<states> state = CONNECTING;
    enum syncStates { WAIT_URL, WAIT_TIME, WAIT_SEEKTIME, WAIT_CROSSTIME, AIRED };
    std::atomic<syncStates> sync = WAIT_URL;
    std::string streamId;
    cspot::CDNTrackStream::TrackInfo trackInfo;
    int64_t offset;

    HTTPstreamer(struct in_addr addr, std::string id, unsigned index, std::string codec, 
                 bool flow, int64_t contentLength, 
                 cspot::CDNTrackStream::TrackInfo track, int32_t startOffset,
                 onHeadersHandler onHeaders);
    ~HTTPstreamer();
    void teardown();
    void flush(void);
    bool connect(int sock);
    bool feedPCMFrames(const uint8_t* data, size_t size);
    std::string getStreamUrl(void);
    void getMetadata(metadata_t* metadata);
    void setContentLength(int64_t contentLength);
};
