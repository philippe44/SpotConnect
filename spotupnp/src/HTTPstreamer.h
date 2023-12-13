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
#include "TrackQueue.h"
#ifdef _WIN32
#include "win32shim.h"
#endif

#include "HTTPmode.h"
#include "metadata.h"
#include "codecs.h"

class HTTPstreamer;

typedef std::map<std::string, std::string> HTTPheaders;
typedef std::function<HTTPheaders(HTTPheaders)> onHeadersHandler;
typedef std::function<void(HTTPstreamer *self)> EoSCallback;

/****************************************************************************************
 * Cache buffer 
 */
class cacheBuffer {
private:
    uint8_t* read_p, * write_p, * wrap_p;

protected:
    uint8_t* buffer;
    size_t size;

public:
    size_t total = 0;

    cacheBuffer(size_t size) : size(size) { }
    virtual ~cacheBuffer(void) { };
    virtual size_t level(void) = 0;
    virtual size_t pending(void) = 0;
    virtual ssize_t scope(size_t offset) = 0;
    virtual size_t read(uint8_t* dst, size_t max, size_t min = 0) = 0;
    virtual uint8_t* readInner(size_t& size) = 0;
    virtual void setOffset(size_t offset) = 0;
    virtual void write(const uint8_t* src, size_t size) = 0;
    virtual void flush(void) = 0;
};

/****************************************************************************************
 * Ring buffer (always rolls over)
 */
class ringBuffer : public cacheBuffer {
private:
    uint8_t* read_p, * write_p, * wrap;

public:
    ringBuffer(size_t size = 8 * 1024 * 1024);
    ~ringBuffer(void) { delete[] buffer; }
    size_t level(void) { return total < size ? total : size - 1; }  
    size_t pending(void) { return write_p >= read_p ? write_p - read_p : wrap - read_p; }
    ssize_t scope(size_t offset);
    size_t read(uint8_t* dst, size_t max, size_t min = 0);
    uint8_t* readInner(size_t& size);
    void setOffset(size_t offset);
    void write(const uint8_t* src, size_t size);
    void flush(void) { read_p = write_p = buffer; total = 0; }
};

/****************************************************************************************
 * File buffer
 */
class fileBuffer : public cacheBuffer {
private:
    FILE* file;
    size_t readOffset = 0;

public:
    fileBuffer(size_t size = 128 * 1024) : cacheBuffer(size) { file = tmpfile(); buffer = new uint8_t[size]; }
    ~fileBuffer(void) { fclose(file); delete[] buffer; }
    size_t level(void) { return total; }
    size_t pending(void) { return total - readOffset; }
    ssize_t scope(size_t offset) { return offset >= total ? offset - total + 1 : 0; }
    size_t read(uint8_t* dst, size_t max, size_t min = 0);
    uint8_t* readInner(size_t& size);
    void setOffset(size_t offset) { readOffset = offset >= 0 ? offset : 0; }
    void write(const uint8_t* src, size_t size);
    void flush(void) { readOffset = total = 0; }
};

/****************************************************************************************
 * Class to stream audio content with HTTP
 */
class HTTPstreamer : public bell::Task {
private:
    std::atomic<bool> isRunning = false;
    std::mutex runningMutex;
    std::string host;
    std::string streamUrl;
    int listenSock = -1;
    uint16_t port;
    int64_t contentLength = HTTP_CL_NONE;
    std::unique_ptr<baseCodec> encoder;
    std::unique_ptr<cacheBuffer> cache;
    size_t useCache;
    uint8_t scratch[32768];
    bool flow;
    int cacheMode;
    struct {
        size_t interval, remain;
        size_t size, count;
        std::string trackId;
    } icy;

    void runTask();
    ssize_t streamBody(int sock, struct timeval& timeout);
    ssize_t sendChunk(int sock, uint8_t* data, ssize_t size, bool count);
    void getMetadata(cspot::TrackInfo& track, metadata_t* metadata);
    onHeadersHandler onHeaders;
    EoSCallback onEoS;

public:
    enum states { OFF, CONNECTING, STREAMING, DRAINING, DRAINED };
    std::atomic<states> state = CONNECTING;
    std::string streamId;
    cspot::TrackInfo trackInfo;
    std::string trackUnique;
    int64_t offset;
    inline static uint16_t portBase = 0, portRange = 1;
    uint64_t totalIn = 0, totalOut = 0;

    HTTPstreamer(struct in_addr addr, std::string id, unsigned index, std::string codec, 
                 bool flow, int64_t contentLength, int cacheMode,
                 cspot::TrackInfo track, std::string_view trackUnique, int32_t startOffset,
                 onHeadersHandler onHeaders, EoSCallback onEoS);
    ~HTTPstreamer();
    void flush(void);
    bool connect(int sock);
    bool feedPCMFrames(const uint8_t* data, size_t size);
    std::string getStreamUrl(void) { return streamUrl; }
    void getMetadata(metadata_t* metadata);
    void setContentLength(int64_t contentLength);
    std::string trackId() { return trackInfo.trackId; }
};
