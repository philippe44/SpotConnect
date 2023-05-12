#include <string>
#include <streambuf>
#include <Session.h>
#include <PlainConnection.h>
#include <memory>
#include <vector>
#include <iostream>
#include <inttypes.h>
#include <fstream>
#include <stdarg.h>
#include <deque>
#include "time.h"

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"
#endif
#include "Logger.h"
#include "Utils.h"
#include "ApResolve.h"
#include "MDNSService.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "BellHTTPServer.h"
#include "BellUtils.h"
#include "WrappedSemaphore.h"
#include "protobuf/metadata.pb.h"

extern "C" {
#include "cross_util.h"
}

#include "HTTPstreamer.h"
#include "spotify.h"
#include "metadata.h"
#include "codecs.h"

static uint16_t portBase, portRange;

/****************************************************************************************
 * Encapsulate pthread mutexes into basic_locable
 */
class shadowMutex {
private:
    pthread_mutex_t* mutex = NULL;
public:
    shadowMutex(pthread_mutex_t* mutex) : mutex(mutex) { }
    void lock() { pthread_mutex_lock(mutex); }
    int trylock() { return pthread_mutex_trylock(mutex); }
    void unlock() { pthread_mutex_unlock(mutex); }
};

/****************************************************************************************
 * Player's main class  & task
 */

class CSpotPlayer : public bell::Task {
private:
    std::string name;

    std::atomic<bool> isPaused = true;
    std::atomic<bool> isConnected = false;
    std::atomic<bool> isRunning = false;
    std::mutex runningMutex;
    shadowMutex playerMutex;
    bell::WrappedSemaphore clientConnected;
    std::string streamTrackUnique;
    int volume = 0;
    int32_t startOffset;

    uint64_t lastTimeStamp;
    uint32_t lastPosition;

    unsigned index = 0;

    std::string codec, id;
    struct in_addr addr;
    AudioFormat format;
    int64_t contentLength;

    struct shadowPlayer* shadow;
    std::unique_ptr<bell::MDNSService> mdnsService;

    std::deque<std::shared_ptr<HTTPstreamer>> streamers;
    std::shared_ptr<HTTPstreamer> player;

    bool flow;
    std::deque<uint32_t> flowMarkers;
    cspot::TrackInfo flowTrackInfo;
    
    std::unique_ptr<bell::BellHTTPServer> server;
    std::shared_ptr<cspot::LoginBlob> blob;
    std::unique_ptr<cspot::SpircHandler> spirc;

    size_t writePCM(uint8_t* data, size_t bytes, std::string_view trackId);
    auto postHandler(struct mg_connection* conn);
    void eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event);
    void trackHandler(std::string_view trackUnique);
    HTTPheaders onHeaders(HTTPheaders request);

    void runTask();
public:
    CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat audio, char* codec, bool flow,
        int64_t contentLength, struct shadowPlayer* shadow, pthread_mutex_t* mutex);
    ~CSpotPlayer();
    void disconnect();

    void friend notify(CSpotPlayer *self, enum shadowEvent event, va_list args);
    bool friend getMetaForUrl(CSpotPlayer* self, const std::string url, metadata_t* metadata);
};

CSpotPlayer::CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat format, char* codec, bool flow,
    int64_t contentLength, struct shadowPlayer* shadow, pthread_mutex_t* mutex) : bell::Task("playerInstance",
        48 * 1024, 0, 0),
    clientConnected(1), codec(codec), id(id), addr(addr), flow(flow),
    name(name), format(format), shadow(shadow), playerMutex(mutex) {
    this->contentLength = (flow && !contentLength) ? HTTP_CL_NONE : contentLength;
}

CSpotPlayer::~CSpotPlayer() {
    isRunning = isConnected = false;
    CSPOT_LOG(info, "player <%s> deletion pending", name.c_str());

    // unlock ourselves as we are waiting
    clientConnected.give();

    // manually unregister mDNS but all other item should be deleted automatically
    mdnsService->unregisterService();

    // then just wait
    std::scoped_lock lock(this->runningMutex);
    CSPOT_LOG(info, "done", name.c_str());
}

size_t CSpotPlayer::writePCM(uint8_t* data, size_t bytes, std::string_view trackUnique) {
    // make sure we don't have a dead lock with a disconnect()
    if (!isRunning || isPaused) return 0;

    std::lock_guard lock(playerMutex);

    if (streamTrackUnique != trackUnique) {
        // safely pop drained players, otherwise only accept 2 players
        if (player && player->state == HTTPstreamer::DRAINED) streamers.pop_back();
        else if (streamers.size() > 1) return 0;

        CSPOT_LOG(info, "trackUniqueId update %s => %s", streamTrackUnique.c_str(), trackUnique.data());
        streamTrackUnique = trackUnique;
        trackHandler(trackUnique);
    }

    if (!streamers.empty() && streamers.front()->feedPCMFrames(data, bytes)) return bytes;
    else return 0;
}

auto CSpotPlayer::postHandler(struct mg_connection* conn) {
#ifdef BELL_ONLY_CJSON
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "status", 101);
    cJSON_AddStringToObject(obj, "statusString", "OK");
    cJSON_AddNumberToObject(obj, "spotifyError", 0);
#else
    nlohmann::json obj;
    // Prepare a success response for spotify
    obj["status"] = 101;
    obj["spotifyError"] = 0;
    obj["statusString"] = "OK";
#endif

    std::string body = "";
    auto requestInfo = mg_get_request_info(conn);
    if (requestInfo->content_length > 0) {
        body.resize(requestInfo->content_length);
        mg_read(conn, body.data(), requestInfo->content_length);
        mg_header hd[10];
        int num = mg_split_form_urlencoded(body.data(), hd, 10);
        std::map<std::string, std::string> queryMap;

        // Parse the form data
        for (int i = 0; i < num; i++) {
            queryMap[hd[i].name] = hd[i].value;
        }

        // Pass user's credentials to the blob
        blob->loadZeroconfQuery(queryMap);

        // We have the blob, proceed to login
        clientConnected.give();
    }

#ifdef BELL_ONLY_CJSON
    auto str = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string objStr(str);
    free(str);
    return server->makeJsonResponse(objStr);
#else
    return server->makeJsonResponse(obj.dump());
#endif
}

void CSpotPlayer::trackHandler(std::string_view trackUnique) {
    // player's mutex is already locked
    
    // switch current streamer to draining state except in flow mode
    if (!streamers.empty() && !flow) {
        streamers.front()->state = HTTPstreamer::DRAINING;
        CSPOT_LOG(info, "draining track %s", streamers.front()->streamId.c_str());
    }
      
    auto newTrackInfo = spirc->getTrackQueue()->getTrackInfo(trackUnique);
    CSPOT_LOG(info, "new track id %s => <%s>", newTrackInfo.trackId.c_str(), newTrackInfo.name.c_str());

    // create a new streamer an run it, unless in flow mode
    if (streamers.empty() || !flow) {
        auto streamer = std::make_shared<HTTPstreamer>(addr, id, index++, codec, flow, contentLength,
                                                       newTrackInfo, trackUnique, streamers.empty() ? -startOffset : 0,
                                                       [this](std::map<std::string, std::string> headers) {
                                                            return this->onHeaders(headers);
                                                        }, nullptr);

        CSPOT_LOG(info, "loading with id %s", streamer->streamId.c_str());

        // be careful that streamer's offset is negative
        metadata_t metadata = { 0 };
        streamer->getMetadata(&metadata);
        metadata.duration += streamer->offset;
        
        // in flow mode, metadata are ignored by LOAD
        if (flow) flowMarkers.push_front(metadata.duration);
       
        // position is optional, shadow player might use it or not
        shadowRequest(shadow, SPOT_LOAD, streamer->getStreamUrl().c_str(), &metadata, (uint32_t)-streamer->offset);

        // play unless already paused
        if (!isPaused) shadowRequest(shadow, SPOT_PLAY);
 
        //streamers.emplace_front(streamer);
        streamers.push_front(streamer);
        streamer->startTask();
    } else {
        CSPOT_LOG(info, "flow track of duration %d will start at %u", newTrackInfo.duration, flowMarkers.front());
        player->trackInfo = newTrackInfo;
        flowMarkers.push_front(flowMarkers.front() + newTrackInfo.duration);
    }
}

 void CSpotPlayer::eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event) {
    switch (event->eventType) {
    case cspot::SpircHandler::EventType::PLAYBACK_START: {
        // avoid conflicts with data callback
        std::scoped_lock lock(playerMutex);

        shadowRequest(shadow, SPOT_STOP);

        // memorize position for when track's beginning will be detected
        startOffset = std::get<int>(event->data);
        CSPOT_LOG(info, "new track will start at %d", startOffset);

        // clean slate => wipe-out queue and pointers
        streamTrackUnique.clear();
        streamers.clear();
        flowMarkers.clear();
        player.reset();

        // Spotify servers do not send volume at connection
        spirc->setRemoteVolume(volume);
        break;
    }
    case cspot::SpircHandler::EventType::PLAY_PAUSE:
        CSPOT_LOG(info, "play/pause");
        isPaused = std::get<bool>(event->data);
        if (player || !streamers.empty()) {
            shadowRequest(shadow, isPaused ? SPOT_PAUSE : SPOT_PLAY);
        }
        break;
    case cspot::SpircHandler::EventType::FLUSH:
        CSPOT_LOG(info, "flush");
        break;
    case cspot::SpircHandler::EventType::NEXT:
    case cspot::SpircHandler::EventType::PREV: {  
        std::scoped_lock lock(playerMutex);
        // send when there is no next, just stop
        CSPOT_LOG(info, "next/prev");
        shadowRequest(shadow, SPOT_STOP);
        break;
    }
    case cspot::SpircHandler::EventType::DISC:
        disconnect();
        break;
    case cspot::SpircHandler::EventType::SEEK: {
        /* Seek does not exist for shadow's player but we need to keep the current streamer. So
         * stop that should close the current connection and PLAY should open a new one, all on 
         * the same url/streamer */
        std::lock_guard lock(playerMutex);

        if (!player && streamers.empty()) {
            CSPOT_LOG(info, "pressing next to quickly, really...");
            break;
        }

        // we might not have detected track yet but we don't want to re-detect
        auto streamer = player ? player : streamers.back();
        streamer->flush();
        streamer->offset = -std::get<int>(event->data);
        CSPOT_LOG(info, "seeking from streamer %s at %u", streamer->streamId.c_str(), -streamer->offset);

        // re-insert streamer whether it was player or not
        streamers.clear();
        flowMarkers.clear();
        streamers.push_front(streamer);
        streamTrackUnique = streamer->trackUnique;
        lastPosition = 0;
        
        shadowRequest(shadow, SPOT_STOP);

        // be careful that streamer's offset is negative
        metadata_t metadata = { 0 };
        streamer->setContentLength(contentLength);

        // in flow mode, need to restore trackInfo from what was the most current
        if (flow) {
            streamer->trackInfo = flowTrackInfo;
            streamer->getMetadata(&metadata);
            metadata.duration += streamer->offset;
            flowMarkers.push_front(metadata.duration);
        } else {
            streamer->getMetadata(&metadata);
            metadata.duration += streamer->offset;
        }

        shadowRequest(shadow, SPOT_LOAD, streamer->getStreamUrl().c_str(), &metadata, -streamer->offset);
        if (!isPaused) shadowRequest(shadow, SPOT_PLAY);
        break;
    }
    case cspot::SpircHandler::EventType::DEPLETED:
        streamers.front()->state = HTTPstreamer::DRAINING;
        CSPOT_LOG(info, "playlist ended, no track left to play");
        break;
    case cspot::SpircHandler::EventType::VOLUME:
        volume = std::get<int>(event->data);
        shadowRequest(shadow, SPOT_VOLUME, volume);
        break;
    case cspot::SpircHandler::EventType::TRACK_INFO: {
        /* We can't use this directly to to set player->trackInfo because with ICY mode, the metadata
         * is marked in the stream not in realtime. But we still need to memorize it if/when a seek is
         * request as we will not know where we are in the data stream then */
        flowTrackInfo = std::get<cspot::TrackInfo>(event->data);
        CSPOT_LOG(info, "current trackInfo id %s => <%s>", flowTrackInfo.trackId.c_str(), flowTrackInfo.name.c_str());
        break;
    }
    default:
        break;
    }
}

// this is called with shared mutex locked
void notify(CSpotPlayer *self, enum shadowEvent event, va_list args) {
    // volume can be handled at anytime
    if (event == SHADOW_VOLUME) {
        int volume = va_arg(args, int);
        if (self->spirc) self->spirc->setRemoteVolume(volume);
        self->volume = volume;
        return;
    }

    if (!self->spirc) return;
    
    switch (event) {
    case SHADOW_TIME: {      
        uint32_t position = va_arg(args, uint32_t);

        if (!self->player) return;

        auto now = gettime_ms64();

        if (self->lastPosition == 0 || 
            self->lastPosition + now - self->lastTimeStamp > position + 5000 ||
            self->lastPosition + now - self->lastTimeStamp + 5000 < position) {

            CSPOT_LOG(info, "adjusting real position %u from %u (offset is %" PRId64 ")", position,
                            self->lastPosition ? (uint32_t) (self->lastPosition + now - self->lastTimeStamp) : 0, 
                            self->player->offset);

            // to avoid getting time twice when starting from 0
            self->lastPosition = position | 0x01;
            position -= self->player->offset;
            self->spirc->updatePositionMs(position);
        } else {
            self->lastPosition = position;
        }

        self->lastTimeStamp = now;

        // in flow mode, have we reached a new track marker
        if (self->flow && self->lastPosition >= self->flowMarkers.back()) {
            CSPOT_LOG(info, "new flow track at %u", self->flowMarkers.back());
            self->flowMarkers.pop_back();
            self->spirc->notifyAudioReachedPlayback();
        }
        break;
    }
    case SHADOW_TRACK: {
        char* url = va_arg(args, char*);

        // nothing to do if we are already the active player
        if (self->streamers.empty() || (self->player && self->player->getStreamUrl() == url)) return;    

        // remove all pending streamers that do not match url (should be none)
        while (self->streamers.back()->getStreamUrl() != url) self->streamers.pop_back();
        self->player = self->streamers.back();

        // finally, get ready for time position and inform spotify that we are playing
        self->lastPosition = 0;
        self->spirc->notifyAudioReachedPlayback();

        CSPOT_LOG(info, "track %s started by URL (%d)", self->player->streamId.c_str(), self->streamers.size());
        break;
    }
    case SHADOW_PLAY:
        self->spirc->setPause(false);
        break;
    case SHADOW_PAUSE:
        self->spirc->setPause(true);
        break;
    case SHADOW_STOP:
        if (self->player && self->player->state == HTTPstreamer::DRAINED) {
            // we have finished playing
            self->spirc->setPause(true);
            self->spirc->updatePositionMs(0);
        } else {
            // disconnect on unexpected STOP (free up player from Spotify)
            self->disconnect();
        }
        break;
    default:
        break;
    }
}

void CSpotPlayer::disconnect() {
    // shared playerMutex is already locked
    CSPOT_LOG(info, "Disconnecting %s", name.c_str());
    isConnected = false;
    shadowRequest(shadow, SPOT_STOP);
    streamers.clear();
    player.reset();
}

bool getMetaForUrl(CSpotPlayer* self, const std::string url, metadata_t* metadata) {
    for (auto it = self->streamers.begin(); it != self->streamers.end(); ++it) {
        if ((*it)->getStreamUrl() == url) {
            (*it)->getMetadata(metadata);
            return true;
        }
    }
    return false;
}

HTTPheaders CSpotPlayer::onHeaders(HTTPheaders request) {
    std::map<std::string, std::string> response;
    struct HTTPheaderList* req = NULL;
    
    for (auto& [key, value] : request) {
        auto item = new struct HTTPheaderList();
        item->key = (char*) key.c_str();
        item->value = (char*) value.c_str();
        item->next = req;
        req = item;
    }

    if (req) for (auto resp = shadowHeaders(shadow, req); resp;) {
        response[resp->key] = resp->value;
        free(resp->key);
        free(resp->value);
        auto item = resp;
        resp = resp->next;
        free(item);
     }
   
    return response;
}

void CSpotPlayer::runTask() {
    std::scoped_lock lock(this->runningMutex);
    isRunning = true;

    int serverPort = 0;

    server = std::make_unique<bell::BellHTTPServer>(serverPort);
    blob = std::make_unique<cspot::LoginBlob>(name);
    serverPort = server->getListeningPorts()[0];
    CSPOT_LOG(info, "Server using actual port %d", serverPort);

    server->registerGet("/spotify_info", [this](struct mg_connection* conn) {
       return server->makeJsonResponse(this->blob->buildZeroconfInfo());
    });

    server->registerPost("/spotify_info", [this](struct mg_connection* conn) {
        return postHandler(conn);
    });

    // Register mdns service, for spotify to find us
    mdnsService = MDNSService::registerService( blob->getDeviceName(), "_spotify-connect", "_tcp", "", serverPort,
            { {"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"} });

    // gone with the wind...
    while (isRunning) {
        clientConnected.wait();

        // we might just be woken up to exit
        if (!isRunning) break;

        CSPOT_LOG(info, "Spotify client connected for %s", name.c_str());

        auto ctx = cspot::Context::createFromBlob(blob);
        ctx->config.audioFormat = format;

        ctx->session->connectWithRandomAp();
        auto token = ctx->session->authenticate(blob);

        // Auth successful
        if (token.size() > 0) {
            spirc = std::make_unique<cspot::SpircHandler>(ctx);
            isConnected = true;

            // set call back to calculate a hash on trackId
            spirc->getTrackPlayer()->setDataCallback(
                [this](uint8_t* data, size_t bytes, std::string_view trackId) {
                    return writePCM(data, bytes, trackId);
                });

            // set event (PLAY, VOLUME...) handler
            spirc->setEventHandler(
                [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
                    eventHandler(std::move(event));
            });

            // Start handling mercury messages
            ctx->session->startTask();

            // exit when player has stopped (received a DISC)
            while (isConnected) {
                ctx->session->handlePacket();
            }

            spirc->disconnect();
            spirc.reset();
            CSPOT_LOG(info, "disconnecting player <%s>", name.c_str());
        }
    }

    CSPOT_LOG(info, "terminating player <%s>", name.c_str());
}

/****************************************************************************************
 * C interface functions
 */

void spotOpen(uint16_t portBase, uint16_t portRange) {
    static bool once = false;
    if (!once) {
        bell::setDefaultLogger();
        once = true;
    }
    ::portBase = portBase;
    ::portRange = portRange;
}

void spotClose(void) {
}

struct spotPlayer* spotCreatePlayer(char* name, char *id, struct in_addr addr, int oggRate, 
                                        char *codec, bool flow, int64_t contentLength, 
                                        struct shadowPlayer* shadow, pthread_mutex_t *mutex) {
    AudioFormat format = AudioFormat_OGG_VORBIS_160;

    if (oggRate == 320) format = AudioFormat_OGG_VORBIS_320;
    else if (oggRate == 96) format = AudioFormat_OGG_VORBIS_96;

    auto player = new CSpotPlayer(name, id, addr, format, codec, flow, contentLength, shadow, mutex);
    if (player->startTask()) return (struct spotPlayer*) player;

    delete player;
    return NULL;
}

void spotDeletePlayer(struct spotPlayer* spotPlayer) {
    auto player = (CSpotPlayer*) spotPlayer;
    delete player;
}

bool spotGetMetaForUrl(struct spotPlayer* spotPlayer, const char *url, metadata_t *metadata) {
    return getMetaForUrl((CSpotPlayer*)spotPlayer, url, metadata);
 }

void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...) {
    va_list args;
    va_start(args, event);
    notify((CSpotPlayer*)spotPlayer, event, args);
    va_end(args);
}


