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

ringBuffer::ringBuffer(size_t size) {
    buffer = new uint8_t[size];
    this->size = size;
    this->write_p = this->read_p = buffer;
    this->wrap_p = buffer + size;
}

size_t ringBuffer::read(uint8_t* dst, size_t size, size_t min) {
    size = std::min(size, used());
    if (size < min) return 0;

    size_t cont = std::min(size, (size_t) (wrap_p - read_p));
    memcpy(dst, read_p, cont);
    memcpy(dst + cont, buffer, size - cont);

    read_p += size;
    if (read_p >= wrap_p) read_p -= this->size;
    return size;
}

uint8_t* ringBuffer::readInner(size_t& size) {
    // caller *must* consume ALL data
    size = std::min(size, used());
    size = std::min(size, (size_t)(wrap_p - read_p));

    uint8_t* p = read_p;
    read_p += size;
    if (read_p >= wrap_p) read_p -= this->size;

    return p;
}

void ringBuffer::write(const uint8_t* src, size_t size) {
    size_t cont = std::min(size, (size_t)(wrap_p - write_p));
    memcpy(write_p, src, cont);
    memcpy(buffer, src + cont, size - cont);

    write_p += size;
    if (write_p >= wrap_p) write_p -= this->size;

    if (used() + size >= this->size) {
        read_p = write_p + 1;
        if (read_p >= wrap_p) read_p = buffer;
    }

    total += size;
}

/****************************************************************************************
 * Class to stream audio content with HTTP
 */

HTTPstreamer::HTTPstreamer(struct in_addr addr, std::string id, unsigned index, std::string codec, 
                           bool flow, int64_t contentLength,
                           cspot::TrackInfo trackInfo, std::string_view trackUnique, int32_t startOffset,
                           onHeadersHandler onHeaders, EoSCallback onEoS) :
                           bell::Task("HTTP streamer", 32 * 1024, 0, 0) {
    this->streamId = id + "_" + std::to_string(index);
    this->trackUnique = trackUnique;
    this->listenSock = socket(AF_INET, SOCK_STREAM, 0);
    this->host = std::string(inet_ntoa(addr));
    this->flow = flow;
    this->trackInfo = trackInfo;
    this->onHeaders = onHeaders;
    this->onEoS = onEoS;
    this->icy.interval = 0;
    // for flow mode, start with a negative offset so that we can always substract
    this->offset = startOffset;

    if (codec.find("pcm") != std::string::npos) {
        encoder = createCodec(codecSettings::PCM);
    } else if (codec.find("wav") != std::string::npos) {
        codecSettings settings;
        encoder = createCodec(codecSettings::WAV);
    } else if (codec.find("flac") != std::string::npos || codec.find("flc") != std::string::npos) {
        flacSettings settings;
        (void) !sscanf(codec.c_str(), "%*[^:]:%d", &settings.level);
        encoder = createCodec(codecSettings::FLAC, &settings);
    } else if (codec.find("mp3") != std::string::npos) {
        mp3Settings settings;
        (void) !sscanf(codec.c_str(), "%*[^:]:%d", &settings.bitrate);
        encoder = createCodec(codecSettings::MP3, &settings);
    } else abort();

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
    // a real content-length might be provided by codec (offset is negative)
    uint64_t duration = trackInfo.duration - (-offset);
    uint64_t length = encoder->initialize(duration);
    if (contentLength == HTTP_CL_REAL) this->contentLength = length ? length : (duration * encoder->getBitrate() * 1.1) / (8 * 1000);
    else if (contentLength == HTTP_CL_KNOWN) this->contentLength = length ? length : HTTP_CL_NONE;
    else this->contentLength = contentLength;
}

void HTTPstreamer::getMetadata(metadata_t* metadata) {
    metadata->sample_rate = 44100;
    metadata->duration = trackInfo.duration;
    metadata->title = trackInfo.name.c_str();
    metadata->album = trackInfo.album.c_str();
    metadata->artist = trackInfo.artist.c_str();
    metadata->artwork = trackInfo.imageUrl.c_str();
}

void HTTPstreamer::flush() {
    state = OFF;
    cache.flush();
    encoder->flush();
    icy.trackId.clear();
}

bool HTTPstreamer::connect(int sock) {
    auto data = std::vector<uint8_t>();

    // this proceeds by reading the full HTTP headers in one block
    while (1) {
        char buffer[2048];
        int n = recv(sock, buffer, sizeof(buffer), 0);

        if (n <= 0)  return false;

        data.insert(data.end(), buffer, buffer + n);
        if (data.size() > 4 && memcmp("\r\n\r\n", data.data() - 4, 4)) break;
    }

    // regex to remove leading and trailing spaces
    std::regex expr("^\\s+|\\s+$");
    size_t offset = 0;

    auto nextLine = [&data, &offset](void) {
        uint8_t* start, * end;
        std::string line;
        end = start = data.data() + offset;

        // find eol
        while (*end != '\r' && *end != '\n' && offset++ <= data.size()) end++;

        if (offset < data.size()) {
            line = std::string(start, end);
            while ((*end == '\r' || *end == '\n') && offset++ <= data.size()) end++;
        }

        return line;
    };

    auto dump = std::string((char*)data.data(), data.size());
    CSPOT_LOG(info, "HTTP received =>\n%s", dump.c_str());

    // refuse connection when we have already sent everything
    if (state == DRAINED) {
        std::stringstream responseStr;

        responseStr << "HTTP/1.0 410 Gone\r\n";
        responseStr << "Connection: close\r\n";
        responseStr << "\r\n";

        send(sock, responseStr.str().c_str(), responseStr.str().size(), 0);
        CSPOT_LOG(info, "HTTP response =>\n%s", responseStr.str().c_str());

        // still, this is not an error case
        return true;
    }

    // find the streamId (crude, who says c++ is better than c, really...)
    auto request = nextLine();
    size_t pos = request.find("?id=");

    if (pos == std::string::npos) {
        CSPOT_LOG(error, "Incorrect HTTP request, can't find streamId %s", request.c_str());
        return false;
    }

    // check this is what's expected
    auto reqId = request.substr(pos + 4);
    pos = reqId.find(" ");
    reqId.resize(pos);

    if (reqId != streamId) {
        CSPOT_LOG(info, "Wrong client/request expected %s received %s", reqId.c_str(), streamId.c_str());
        return false;
    }

    bool isHTTP11 = request.find("HTTP/1.1") != std::string::npos;
    bool isHead = request.find("HEAD") != std::string::npos;
    int64_t length = contentLength;

    // parse headers
    for (auto line = nextLine(); !line.empty(); line = nextLine()) {
        size_t pos = line.find(':');
        if (pos == std::string::npos) continue;
        for (auto it = line.begin(); it != line.end(); ++it) *it = tolower(*it);
        headers[std::regex_replace(line.substr(0, pos), expr, "")] = std::regex_replace(line.substr(pos + 1), expr, "");
    }

    HTTPheaders response;

    // get optional headers from whoever wants to have a say
    if (onHeaders) {
        response = onHeaders(headers);
    }

    std::string status = "200 OK";
    useCache = false;
    bool useRange = false;
    bool isSonos = headers["user-agent"].find("sonos") != std::string::npos;

    // check if icy metadata is requested
    if (auto it = headers.find("icy-metadata"); it != headers.end() && flow) {
       icy.remain = icy.interval = std::max(sizeof(scratch), (size_t) 16384);
       response["icy-metaint"] = std::to_string(icy.interval);
    }

    /*
      There is a fair bit of HTTP soup below and the problem is many Sonos speakers. These crappy
      devices can't handle properly HTTP responses with no content-length (or in chunked mode) but
      mostly for mp3 and only to resume after a pause. When receiving flac, a pause causes they
      immediately terminate the HTTP connection and resume a while after resume with a range request.
      The response to this should be a 206 with Content-Range of "start-end / *" but they can't handle
      the lack of length so we send a 206 with no Content-Range, which should nto work but it does...
      For mp3, they close the connection immediately after the pause and re-open it a while after
      resume (I assume once watermark is low) but here, they don't issue a range request, just a 
      regular GET and if the answer does not contain a Content-Range (even in chunked mode) then they
      fail a while after (probably when buffer is consumed). So, regardeless of HTTP mode, we must
      send a fake Content-Range when we receive a GET and we have already sent something    
    */

    // handle range-request 
    if (auto it = headers.find("range"); it != headers.end() && cache.total) {
        size_t offset = 0;
        (void) !sscanf(it->second.c_str(), "bytes=%zu", &offset);
        if (offset && offset >= cache.total - cache.used()) {
            status = "206 Partial Content";
            // see note above
            if (!isSonos) {
                response["Content-Range"] = "bytes " + std::to_string(offset) + "-" + std::to_string(cache.used()) + "/*";
                useRange = true;
            }
            cache.setReadPtr(offset);
            useCache = true;
        }
    } else if (cache.total) {
        // client asking to re-open the resource, do that since begining if we can
        cache.setReadPtr(0);
        useCache = true;
        // see note above
        if (isSonos) length = INT64_MAX;
        CSPOT_LOG(info, "re-opening HTTP stream at %d", cache.total - cache.used());
    }

    // Chunked is compatible with range, as it it a message property, and not en entity one
    if (isHTTP11 && length == HTTP_CL_CHUNKED) response["Transfer-Encoding"] = "chunked";

    // c++ conversion to string is really a joke
    std::stringstream responseStr;
    responseStr << (isHTTP11 ? "HTTP/1.1 " : "HTTP/1.0 ") + status + "\r\n";
    for (auto it = response.cbegin(); it != response.cend(); ++it) responseStr << it->first + ": " + it->second + "\r\n";
    if (length > 0) responseStr << "Content-Length: " + std::to_string(length) + "\r\n";
    responseStr << "Server: spot-connect\r\n";
    responseStr << "Content-Type: " + encoder->mimeType + "\r\n";
    responseStr << "Connection: close\r\n";
    responseStr << "\r\n";
    
    send(sock, responseStr.str().c_str(), responseStr.str().size(), 0);
    CSPOT_LOG(info, "HTTP response =>\n%s", responseStr.str().c_str());

    return !isHead;
}

ssize_t HTTPstreamer::sendChunk(int sock, uint8_t* data, ssize_t size) {
    if (contentLength == HTTP_CL_CHUNKED) {
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

    if (contentLength == HTTP_CL_CHUNKED) {
        send(sock, "\r\n", 2, 0);
    }

    totalOut += size;
    return size;
}

ssize_t HTTPstreamer::streamBody(int sock, struct timeval& timeout) {
    ssize_t size = 0;

    // cache has priority
    if (useCache) {
        size = cache.read(scratch, sizeof(scratch));
        if (!size) useCache = false;
    }

    // not using cache or empty cache, get fresh data from encoder
    if (!size) {
        size = encoder->read(scratch, sizeof(scratch), 0, state == DRAINING);
        // cache what we have anyways
        cache.write(scratch, size);
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
        char buffer[1024];
            
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
        if (offset) sendChunk(sock, (uint8_t*)scratch, offset);
        size -= offset;

        // then send icy data
        sendChunk(sock, (uint8_t*) buffer, len_16 * 16 + 1);
        icy.remain = icy.interval;
    }

    ssize_t sent = sendChunk(sock, (uint8_t*) scratch + offset, size);

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

std::string HTTPstreamer::getStreamUrl(void) {
    return "http://" + host + ":" + std::to_string(port) + HTTP_BASE_URL + streamId;
}

void HTTPstreamer::runTask() {
    std::scoped_lock lock(runningMutex);
    isRunning = true;
   
    int sock = -1;
    struct timeval timeout = { 0, 25 * 1000 };

    while (isRunning) {
        ssize_t sent;
        fd_set rfds;
        int size = 0;      
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
        }

        // terminate connection if required by HTTP peer
        if (n < 0 || (!success && state <= CONNECTING) || state == DRAINED) {
            CSPOT_LOG(info, "HTTP close %u", sock);
            closesocket(sock);
            sock = -1;
            if (state == STREAMING) state = CONNECTING;
            continue;
        }

        // state is tested twice because streamBody is a call that needs to be made
        if (state >= STREAMING && ((n = streamBody(sock, timeout)) == 0) && state == DRAINING) {
            CSPOT_LOG(info, "HTTP finished in:%" PRIu64 " out:%" PRIu64 " for %d (id: % s)", totalIn, totalOut, sock, streamId.c_str());
            // chunked-encoding terminates by a last empty chunk ending sequence
            if (contentLength == HTTP_CL_CHUNKED) send(sock, "0\r\n\r\n", 5, 0);
            flush();
            shutdown(sock, SHUT_RDWR);
            closesocket(sock);
            sock = -1;
            state = DRAINED;
            if (onEoS) onEoS(this);
        } else if (n < 0) {
            // something happened in streamBody, let's close the socket and wait for next request
            CSPOT_LOG(info, "closing socket %d", sock);
            closesocket(sock);
            sock = -1;
        } else {
            timeout.tv_usec = 50 * 1000;
        }
    }

    isRunning = false;
}
