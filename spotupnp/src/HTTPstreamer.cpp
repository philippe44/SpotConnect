/*
 *  This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 *
 */

#include <memory>
#include <vector>
#include <inttypes.h>
#include <sstream>
#include <regex>
#include <algorithm>
#include <atomic>
#include <string>
#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#else
#include <ws2tcpip.h>
#endif

#include "Logger.h"

#include "HTTPstreamer.h"

#ifndef _WIN32
#include <unistd.h>
#define closesocket(s) close(s)
#endif

/****************************************************************************************
 * Ring buffer (always rolls over)
 */

ringBuffer::ringBuffer(size_t size) : cacheBuffer(size) {
    buffer = new uint8_t[size];
    this->write_p = this->read_p = buffer;
    this->wrap = buffer + size;
}

ssize_t ringBuffer::scope(size_t offset) {
    if (offset >= total) return offset - total + 1;
    else if (offset >= total - level()) return 0;
    else return offset - total + level();
}

void ringBuffer::setOffset(size_t offset) {
    if (offset >= total) read_p = write_p;
    else if (offset < total - level()) read_p = (write_p + 1) == wrap ? buffer : write_p + 1;
    else read_p = buffer + offset % size;
}

size_t ringBuffer::read(uint8_t* dst, size_t size, size_t min) {
    size = std::min(size, pending());
    if (size < min) return 0;

    size_t cont = std::min(size, (size_t) (wrap - read_p));
    memcpy(dst, read_p, cont);
    memcpy(dst + cont, buffer, size - cont);

    read_p += size;
    if (read_p >= wrap) read_p -= this->size;
    return size;
}

uint8_t* ringBuffer::readInner(size_t& size) {
    // caller *must* consume ALL data
    size = std::min(size, pending());
    size = std::min(size, (size_t)(wrap - read_p));

    uint8_t* p = read_p;

    read_p += size;
    if (read_p >= wrap) read_p -= this->size;
    return size ? p : NULL;
}

void ringBuffer::write(const uint8_t* src, size_t size) {
    size_t cont = std::min(size, (size_t)(wrap - write_p));
    memcpy(write_p, src, cont);
    memcpy(buffer, src + cont, size - cont);

    write_p += size;
    total += size;

    if (write_p >= wrap) write_p -= this->size;   
    if (level() == this->size - 1) read_p = (write_p + 1 == wrap) ? buffer : write_p + 1;
}

/****************************************************************************************
 * File buffer
 */

size_t fileBuffer::read(uint8_t* dst, size_t size, size_t min) {
    size = std::min(size, total);
    if (size < min) return 0;
   
    fseek(file, readOffset, SEEK_SET);
    size_t bytes = fread(dst, 1, size, file);
    readOffset += bytes;

    return bytes;
}

uint8_t* fileBuffer::readInner(size_t& size) {
    if (size > this->size) {
        delete[] buffer;
        buffer = new uint8_t[size];
        this->size = size;
    }

    // caller *must* consume ALL data
    size = std::min(size, total);

    fseek(file, readOffset, SEEK_SET);
    size = fread(buffer, 1, size, file);
    readOffset += size;

    return size ? buffer : NULL;
}

void fileBuffer::write(const uint8_t* src, size_t size) {
    fseek(file, 0, SEEK_END);
    fwrite(src, 1, size, file);
    total += size;
}

/****************************************************************************************
 * Class to stream audio content with HTTP
 */

HTTPstreamer::HTTPstreamer(struct in_addr addr, std::string id, unsigned index, std::string codec, 
                           bool flow, int64_t contentLength, int cacheMode, 
                           cspot::TrackInfo trackInfo, std::string_view trackUnique, int32_t startOffset,
                           onHeadersHandler onHeaders, EoSCallback onEoS) :
                           trackUnique(trackUnique), flow(flow), trackInfo(trackInfo), cacheMode(cacheMode), 
                           bell::Task("HTTP streamer", 32 * 1024, 0, 0) {
    this->streamId = id + "_" + std::to_string(index);
    this->listenSock = socket(AF_INET, SOCK_STREAM, 0);
    this->host = std::string(inet_ntoa(addr));
    this->onHeaders = onHeaders;
    this->onEoS = onEoS;
    this->icy.interval = 0;
    // for flow mode, start with a negative offset so that we can always substract
    this->offset = startOffset;
    if (cacheMode == HTTP_CACHE_DISK && !flow) this->cache = std::make_unique<fileBuffer>();
    else this->cache = std::make_unique<ringBuffer>();

    codecSettings settings;

    if (codec.find("pcm") != std::string::npos) {
        encoder = createCodec(codecSettings::PCM, settings);
    } else if (codec.find("wav") != std::string::npos) {
        encoder = createCodec(codecSettings::WAV, settings);
    } else if (codec.find("flac") != std::string::npos || codec.find("flc") != std::string::npos) {
        (void)!sscanf(codec.c_str(), "%*[^:]:%d", &settings.flac.level);
        encoder = createCodec(codecSettings::FLAC, settings);
    } else if (codec.find("opus") != std::string::npos) {
        (void)!sscanf(codec.c_str(), "%*[^:]:%d", &settings.opus.bitrate);
        encoder = createCodec(codecSettings::OPUS, settings);
    } else if (codec.find("vorbis") != std::string::npos) {
        (void)!sscanf(codec.c_str(), "%*[^:]:%d", &settings.vorbis.bitrate);
        encoder = createCodec(codecSettings::VORBIS, settings);
    } else if (codec.find("aac") != std::string::npos) {
        (void)!sscanf(codec.c_str(), "%*[^:]:%d", &settings.aac.bitrate);
        encoder = createCodec(codecSettings::AAC, settings);
    } else if (codec.find("mp3") != std::string::npos) {
        (void) !sscanf(codec.c_str(), "%*[^:]:%d", &settings.mp3.bitrate);
        encoder = createCodec(codecSettings::MP3, settings);
    } else throw std::runtime_error("unknown codec");

    // now estimate the content-length
    setContentLength(contentLength);
  
    struct sockaddr_in host;
    host.sin_addr = addr;
    host.sin_family = AF_INET;

    for (int count = 0, offset = rand();; count++, offset++) {
        host.sin_port = htons(portBase + (offset % portRange));
        if (!bind(listenSock, (const sockaddr*) &host, sizeof(host))) break;
        if (!portBase || count == portRange) throw std::runtime_error("can't bind on port" + std::string(strerror(errno)));
    }

    socklen_t len = sizeof(host);
    getsockname(listenSock, (struct sockaddr*) &host, &len);
    this->port = ntohs(host.sin_port);
    CSPOT_LOG(info, "Bound to port %u", this->port);

    this->streamUrl = "http://" + this->host + ":" + std::to_string(this->port) + HTTP_BASE_URL + "." + this->encoder->id() + "?id=" + this->streamId;

    if (::listen(listenSock, 1) < 0) {
        throw std::runtime_error("listen failed on port " +
            std::to_string(this->port) + ": " +
            std::string(strerror(errno)));
    }
}

HTTPstreamer::~HTTPstreamer() {
    isRunning = false;
    std::scoped_lock lock(runningMutex);
    if (listenSock > 0) closesocket(listenSock);
    CSPOT_LOG(info, "HTTP streamer %s deleted", streamId.c_str());
}

void HTTPstreamer::setContentLength(int64_t contentLength) {
    // a real content-length (< 0 means estimated) might be provided by codec (offset is negative)
    uint64_t duration = trackInfo.duration - (-offset);
    int64_t length = encoder->initialize(duration);

    if (!length) throw std::runtime_error("can't initialize codec");

    // add 20% headroom when it is estimated base on known duration
    if (contentLength == HTTP_CL_REAL) this->contentLength = length < 0 && duration ? abs(length) * 1.20 : abs(length);
    else if (contentLength == HTTP_CL_KNOWN) this->contentLength = length > 0 ? length : HTTP_CL_NONE;
    else this->contentLength = contentLength;
}

void HTTPstreamer::getMetadata(metadata_t* metadata) {
    metadata->sample_rate = 44100;
    metadata->duration = trackInfo.duration;
    metadata->title = trackInfo.name.c_str();
    metadata->album = trackInfo.album.c_str();
    metadata->artist = trackInfo.artist.c_str();
    metadata->artwork = trackInfo.imageUrl.c_str();
    metadata->track = trackInfo.number;
    metadata->disc = trackInfo.discNumber;
}

void HTTPstreamer::flush() {
    totalOut = 0;
    state = OFF;
    cache->flush();
    encoder->flush();
    icy.trackId.clear();
}

bool HTTPstreamer::connect(int sock) {
    auto data = std::vector<uint8_t>();

    // get the HTTP headers by chunks (there should be no body)
    while (1) {
        uint8_t buffer[256];
        int n = recv(sock, (char*) buffer, sizeof(buffer), 0);

        if (n <= 0)  return false;

        data.insert(data.end(), buffer, buffer + n);
        if (data.size() >= 4 && !memcmp(data.data() + data.size() - 4, "\r\n\r\n", 4)) break;
    }

    // regex to remove leading and trailing spaces
    std::regex expr("^\\s+|\\s+$");
    size_t offset = 0;

    auto nextLine = [&data, &offset](void) {
        uint8_t* start, * end;
        std::string line;
        end = start = data.data() + offset;

        // find eol
        for (end = start = data.data() + offset;
             offset < data.size() && *end != '\r' && *end != '\n';
             end++, offset++);

        if (offset < data.size()) {
            line = std::string(start, end);
            for (; (*end == '\r' || *end == '\n') && offset < data.size(); end++, offset++);
        }

        return line;
    };

    auto dump = std::string((char*)data.data(), data.size());
    CSPOT_LOG(info, "HTTP received =>\n%s", dump.c_str());

    // get the streamId 
    auto request = nextLine();

    if (request.find("?id=") == std::string::npos) {
        CSPOT_LOG(error, "Incorrect HTTP request, can't find streamId %s", request.c_str());
        return false;
    }

    // check this is what's expected
    if (request.find(streamId) == std::string::npos) {
        CSPOT_LOG(info, "Wrong client/request %s not in  url %s", streamId.c_str(), request.c_str());
        return false;
    }

    HTTPheaders response, headers;

    // parse headers
    for (auto line = nextLine(); !line.empty(); line = nextLine()) {
        size_t pos = line.find(':');
        if (pos == std::string::npos) continue;
        for (auto it = line.begin(); it != line.end(); ++it) *it = tolower(*it);
        headers[std::regex_replace(line.substr(0, pos), expr, "")] = std::regex_replace(line.substr(pos + 1), expr, "");
    }

    // get optional headers from whoever wants to have a say
    if (onHeaders) response = onHeaders(headers);

    std::string status = "200 OK";
    chunked = request.find("HTTP/1.1") != std::string::npos && contentLength == HTTP_CL_CHUNKED;

    bool sendBody = request.find("HEAD") == std::string::npos;
    bool isSonos = headers["user-agent"].find("sonos") != std::string::npos;
    // if we know the real length because it's a redo, then tell it if authorized
    int64_t length = (state == DRAINED && (contentLength >= 0 || contentLength == HTTP_CL_KNOWN)) ? totalOut : contentLength;
    
    // check if icy metadata is requested
    if (auto it = headers.find("icy-metadata"); it != headers.end() && flow) {
       icy.remain = icy.interval = std::max(sizeof(scratch), (size_t) 16384);
       response["icy-metaint"] = std::to_string(icy.interval);
    }

    // check various DLNA fields
    if (auto it = headers.find("transferMode.dlna.org"); it != headers.end()) response["transferMode.dlna.org"] = it->second;
    if (auto it = headers.find("getcontentFeatures.dlna.org"); it != headers.end()) {
        char* DLNA_ORG = makeDLNA_ORG(encoder->id().c_str(), cacheMode != HTTP_CACHE_MEM, flow);
        response["contentFeatures.dlna.org"] = DLNA_ORG;
        free(DLNA_ORG);
    }
    if (auto it = headers.find("getAvailableSeekRange.dlna.org"); it != headers.end() && cache->total) {
        response["contentFeatures.dlna.org"] = "availableSeekRange.dlna.org: 0 bytes=" +
                                               std::to_string(cache->total - (cacheMode == HTTP_CACHE_MEM ? cache->level() : 0)) +
                                               "-" + std::to_string(cache->total - 1);
    }

    /* There is a fair bit of HTTP soup below and the problem is many Sonos speakers. When paused
     * (on mp3 or flac) they will leave the connection open, then close and and re-open it on 
     * resume but request the whole file. Still even if given properly the whole resource, they 
     * fails once they have received about what they already got. We can fool them by sending 
     * ANYTHING with a content-length (required) of an insane value. Then we close the connection 
     * (they will close it anyway) which forces them to re-open it again and this time it asks 
     * for a proper range request but we need to answer 206 without a content-range (which is not 
     * compliant) or they fail as well */

    // by default, use cache and restart from oldest (might change that below)
    useCache = true;
    cache->setOffset(0);

    // handle range-request 
    if (auto it = headers.find("range"); it != headers.end() && cache->total) {
        size_t offset = 0;
        (void) !sscanf(it->second.c_str(), "bytes=%zu", &offset);

        // this is not an initial request (there is cache), so if offset is 0, we are all set
        if (offset) {
            if (state != DRAINED && cache->total == offset) {
                // special case where we just continue so we'll do a 200 with no cache
                useCache = false;
            } else if (cache->scope(offset) == 0) {
                // first try to see if we can serve that
                status = "206 Partial Content";
                // see note above
                if (!isSonos) response["Content-Range"] = "bytes " + std::to_string(offset) + 
                                                          "-" + std::to_string(cache->total - 1) + "/*";
                // do not sent content-length on PartialResponse
                cache->setOffset(offset);
                CSPOT_LOG(info, "service partial-content %zu-%zu (length:%" PRId64 ")", offset, cache->total - 1, length);
                length = 0;
            } else if (state == DRAINED && offset >= cache->total) {
                // there is an offset out of scope and we are drained, we are tapping in estimated length
                sendBody = false;
                status = "416 Range Not Satisfiable";
                response.clear();
                response["Content-Range"] = "bytes */" + std::to_string(cache->total);
                CSPOT_LOG(info, "can't serve offset %zu (cached:%zu)", offset, cache->total);
            } else {
                // this likely means we are being probed toward the end of the file (which we don't have)
                status = "206 Partial Content";
                size_t avail = std::min(cache->total, (size_t) (length - offset));
                cache->setOffset(cache->total - avail);
                response["Content-Range"] = "bytes " + std::to_string(offset) +
                    "-" + std::to_string(offset + avail - 1) + "/" + std::to_string(length);
                CSPOT_LOG(info, "being probed at %zu but have %zu/%" PRId64 ", using offset at %zu", offset,
                                 cache->total, length, cache->total - avail);
                length = 0;
            }
        }
    } else if (cache->total) {
        // restart from the beginning if we have cache (see note above regarding Sonos)
        if (isSonos) length = INT64_MAX;
        CSPOT_LOG(info, "service with cache from %zu (cached:%zu)", cache->total - cache->level(), cache->total);
    } else {
        // initial request, don't use cache (there is non anyway)
        useCache = false;
    }

    // c++ conversion to string is really a joke
    std::stringstream responseStr;
    responseStr << (chunked ? "HTTP/1.1 " : "HTTP/1.0 ") + status + "\r\n";
    
    if (sendBody) {
        if (length > 0) {
            chunked = false;
            responseStr << "Content-Length: " + std::to_string(length) + "\r\n";
        } else if (chunked) {
            responseStr << "Transfer-Encoding: chunked\r\n";
        }
    }

    // send accumulated headers
    for (auto it = response.cbegin(); it != response.cend(); ++it) responseStr << it->first + ": " + it->second + "\r\n";
    responseStr << "Server: spot-connect\r\n";
    responseStr << "Accept-Ranges: bytes\r\n";
    responseStr << "Content-Type: " + encoder->mimeType + "\r\n";
    responseStr << "Connection: close\r\n";
    responseStr << "\r\n";
    
    send(sock, responseStr.str().c_str(), responseStr.str().size(), 0);
    CSPOT_LOG(info, "HTTP response =>\n%s", responseStr.str().c_str());

    return sendBody;
}

ssize_t HTTPstreamer::sendChunk(int sock, uint8_t* data, ssize_t size, bool count) {
    if (chunked) {
        char chunk[16];
        sprintf(chunk, "%zx\r\n", size);
        if (send(sock, chunk, strlen(chunk), 0) < 0) return 0;
    }

    ssize_t bytes = size;

    while (bytes) {
        ssize_t sent = send(sock, (char*) data + size - bytes, bytes, 0);
        // might be chunked mode, but no reason to send and end-of-chunk
        if (sent < 0) return size - bytes;
        bytes -= sent;
    }

    if (chunked) {
        send(sock, "\r\n", 2, 0);
    }

    if (count) totalOut += size;
    return size;
}

ssize_t HTTPstreamer::streamBody(int sock, struct timeval& timeout) {
    ssize_t size = 0;

    // cache has priority
    if (useCache) {
        size = cache->read(scratch, sizeof(scratch));
        if (!size) useCache = false;
    }

    // not using cache or empty cache, get fresh data from encoder
    if (!size) {
        size = encoder->read(scratch, sizeof(scratch), 0, state == DRAINING);
        // cache what we have anyway
        cache->write(scratch, size);
    }

    // we really have nothing, let caller decide what's next
    if (!size) {
        timeout.tv_usec = 50 * 1000;
        return 0;
    }

    int offset = 0;

    // check if ICY sending is active (len < ICY_INTERVAL)
    if (icy.interval && size > icy.remain) {
        int len_16 = 0;
        char buffer[255*16+1];
            
        if (icy.trackId != trackInfo.trackId) {
            const char* format, *artist = trackInfo.artist.c_str();
                
            // there is room for 1 extra byte at the beginning for length
            if (trackInfo.imageUrl.size()) format = "NStreamTitle='%s%s%s';StreamURL='%s';";
            else format = "NStreamTitle='%s%s%s';";
            len_16 = snprintf(buffer, sizeof(buffer), format, artist, *artist ? " - " : "", trackInfo.name.c_str(), trackInfo.imageUrl.c_str()) - 1;
            len_16 = (len_16 + 15) / 16;

            icy.trackId = trackInfo.trackId;
            CSPOT_LOG(info, "ICY update %s", buffer + 1);
        }

        buffer[0] = len_16;

        // send remaining data first
        offset = icy.remain;
        if (offset) sendChunk(sock, (uint8_t*)scratch, offset, !useCache);
        size -= offset;

        // then send icy data
        sendChunk(sock, (uint8_t*) buffer, len_16 * 16 + 1, false);
        icy.remain = icy.interval;
    }

    ssize_t sent = sendChunk(sock, (uint8_t*) scratch + offset, size, !useCache);
    
    // update remaining count with desired length
    if (icy.interval) icy.remain -= size;

    if (sent != size) {
#ifdef _WIN32
        int error = WSAGetLastError();
#else
        int error = errno;
#endif
        CSPOT_LOG(error, "HTTP error %d for %s => send %zd / %zd (%d)", error, streamId.c_str(), sent, size, sock);
        return sent - size;
    }

    timeout.tv_usec = 0;   
    return size;
}

bool HTTPstreamer::feedPCMFrames(const uint8_t* data, size_t size) {
    if (isRunning && encoder->pcmWrite(data, size)) {
        totalIn += size;
        return true;
    } else {
        return false;
    }
}

void HTTPstreamer::runTask() {
    std::scoped_lock lock(runningMutex);
    isRunning = true;

    int sock = -1;
    struct timeval timeout = { 0, 25 * 1000 };

    while (isRunning) {
        fd_set rfds;
        bool success = true;

        if (sock == -1) {
            struct timeval timeout = { 0, 50 * 1000 };

            FD_ZERO(&rfds);
            FD_SET(listenSock, &rfds);

            if (select(listenSock + 1, &rfds, NULL, NULL, &timeout) > 0) {
                sock = accept(listenSock, NULL, NULL);
            }

            if (sock == -1 || !isRunning) continue;
            CSPOT_LOG(info, "got HTTP connection %u", sock);
        }

        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        int n = select(sock + 1, &rfds, NULL, NULL, &timeout);

        if (n > 0) {
            success = connect(sock);
            // we might already be in draining mode
            if (success && state <= STREAMING) state = STREAMING;
            else if (state == DRAINED) useCache = true;
        }

        // terminate connection if required by HTTP peer
        if (n < 0 || (!success && state <= CONNECTING)) {
            CSPOT_LOG(info, "HTTP close %u (sent:%zu)", sock, totalOut);
            closesocket(sock);
            sock = -1;
            if (state == STREAMING) state = CONNECTING;
            continue;
        }

        // try to stream some data 
        ssize_t sent = state >= STREAMING || (state == DRAINED && useCache) ? streamBody(sock, timeout) : 0;

        if (state >= DRAINING && !sent) {
           // chunked-encoding terminates by a last empty chunk ending sequence
           if (chunked) send(sock, "0\r\n\r\n", 5, 0);

           CSPOT_LOG(info, "closing socket %d (sent:%zu), now lingering", sock, totalOut);
           if (state == DRAINING && onEoS) onEoS(this);
           state = DRAINED;      

           shutdown(sock, SHUT_RDWR);
           closesocket(sock);
           sock = -1;
        } else if (sent < 0) {
            // something happened in streamBody, let's close the socket and wait for next request
            CSPOT_LOG(info, "early closing socket %d (sent:%zu)", sock, totalOut);
            closesocket(sock);
            sock = -1;
        } else {
            timeout.tv_usec = 50 * 1000;
        }
    }

    isRunning = false;
}

/* DLNA.ORG_CI: conversion indicator parameter (integer)
 *     0 not transcoded
 *     1 transcoded
 */
typedef enum {
    DLNA_ORG_CONVERSION_NONE = 0,
    DLNA_ORG_CONVERSION_TRANSCODED = 1,
} dlna_org_conversion_t;

/* DLNA.ORG_OP: operations parameter (string)
 *     "00" (or "0") neither time seek range nor range supported
 *     "01" range supported
 *     "10" time seek range supported
 *     "11" both time seek range and range supported
 */
typedef enum {
    DLNA_ORG_OPERATION_NONE = 0x00,
    DLNA_ORG_OPERATION_RANGE = 0x01,
    DLNA_ORG_OPERATION_TIMESEEK = 0x10,
} dlna_org_operation_t;

/* DLNA.ORG_FLAGS, padded with 24 trailing 0s
 *     8000 0000  31  senderPaced
 *     4000 0000  30  lsopTimeBasedSeekSupported
 *     2000 0000  29  lsopByteBasedSeekSupported
 *     1000 0000  28  playcontainerSupported
 *      800 0000  27  s0IncreasingSupported
 *      400 0000  26  sNIncreasingSupported
 *      200 0000  25  rtspPauseSupported
 *      100 0000  24  streamingTransferModeSupported
 *       80 0000  23  interactiveTransferModeSupported
 *       40 0000  22  backgroundTransferModeSupported
 *       20 0000  21  connectionStallingSupported
 *       10 0000  20  dlnaVersion15Supported
 *
 *     Example: (1 << 24) | (1 << 22) | (1 << 21) | (1 << 20)
 *       DLNA.ORG_FLAGS=0170 0000[0000 0000 0000 0000 0000 0000] // [] show padding
 */
typedef enum {
    DLNA_ORG_FLAG_SENDER_PACED = (1 << 31),
    DLNA_ORG_FLAG_TIME_BASED_SEEK = (1 << 30),
    DLNA_ORG_FLAG_BYTE_BASED_SEEK = (1 << 29),
    DLNA_ORG_FLAG_PLAY_CONTAINER = (1 << 28),

    DLNA_ORG_FLAG_S0_INCREASE = (1 << 27),
    DLNA_ORG_FLAG_SN_INCREASE = (1 << 26),
    DLNA_ORG_FLAG_RTSP_PAUSE = (1 << 25),
    DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE = (1 << 24),

    DLNA_ORG_FLAG_INTERACTIVE_TRANSFERT_MODE = (1 << 23),
    DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE = (1 << 22),
    DLNA_ORG_FLAG_CONNECTION_STALL = (1 << 21),
    DLNA_ORG_FLAG_DLNA_V15 = (1 << 20),
} dlna_org_flags_t;

char* makeDLNA_ORG(const char *codec, bool infiniteCache, bool live) {
    const char* DLNAOrgPN = "";
        
    if (!strcasecmp(codec, "mp3")) DLNAOrgPN = "DLNA.ORG_PN=MP3;";
    else if (!strcasecmp(codec, "aac")) DLNAOrgPN = "DLNA.ORG_PN=MP3;";
    else if (!strcasecmp(codec, "pcm") || !strcasecmp(codec, "wav")) DLNAOrgPN = "DLNA.ORG_PN=MP3;";

    /* OP set means that the full resource must be accessible, but Sn can still increase. It
     * is exclusive with b29 (DLNA_ORG_FLAG_BYTE_BASED_SEEK) of FLAGS which is limited random
     * access and when that is set, player shall not expect full access to already received
     * bytes and for example, S0 can increase (it does not have to). When live is set, either
     * because we have no duration (it's a webradio) or we are in flow, we have to set S0
     * because we lose track of the head. Caller can still decide to have infinite cache...
     * The value for Sn is questionable as it actually changes only for live stream but we
     * don't have access to it until we have received full content. As it is supposed to
     * represent what is accessible, not the media itself, we'll always set it. We can still use
     * partial cache, so b29 shall be set (then OP shall not be). If user has opted-out file
     * cache (or no fake), we can only do b29. In any case, we don't support time-based seek */
    
     uint32_t org_op = infiniteCache ? DLNA_ORG_OPERATION_RANGE : 0;
     uint32_t org_flags = DLNA_ORG_FLAG_STREAMING_TRANSFERT_MODE | DLNA_ORG_FLAG_BACKGROUND_TRANSFERT_MODE |
                          DLNA_ORG_FLAG_CONNECTION_STALL | DLNA_ORG_FLAG_DLNA_V15 |
                          DLNA_ORG_FLAG_SN_INCREASE;

     if (live) org_flags |= DLNA_ORG_FLAG_S0_INCREASE;
     if (!infiniteCache) org_flags |= DLNA_ORG_FLAG_BYTE_BASED_SEEK;

     size_t n = snprintf(NULL, 0, "%sDLNA.ORG_OP=%02u;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
                              DLNAOrgPN, org_op, org_flags);

     char* DLNA = (char*) malloc(n + 1);
     (void) !snprintf(DLNA, n + 1, "%sDLNA.ORG_OP=%02u;DLNA.ORG_CI=0;DLNA.ORG_FLAGS=%08x000000000000000000000000",
                                                 DLNAOrgPN, org_op, org_flags);
     return DLNA;
}

