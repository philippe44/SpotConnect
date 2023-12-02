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

/****************************************************************************************
 * Encapsulate pthread mutexes into basic_lockable
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

#define SMART_FLUSH
/* When user changes a queue, Spotify sends a replacement of the current playlist, with the
 * the first track being the curently playing one. CSpot tries to be smart about that and 
 * when the playing track is still being downloaded, it will not flush the player and re-send
 * it (at the current position) which obviously creates a gap but simply skip the first track
 * of the updated (no PLAYBACK_START event either). But if a player has a large buffer, then 
 * it is likely that TrackPlayer has moved to n+1 (or further) track and so the only option 
 * is to re-send track n. The SMART_FLUSH option tries to workaround that because when CSpot
 * has sent a full track, then the streamer in charge has it entirely. So because there is 
 * only 2 tracks in UPnP, when receiving a flush, we can clear streamers queue, let the 
 * current streamer (player) finish its job and ignore the PLAYBACK_START event, ignore the
 * audio data that will be sent as it belongs to track n, until we have a new track. 
 */

class CSpotPlayer : public bell::Task {
private:
    std::string name;
    std::string credentials;
    enum states { ABORT, LINKED, DISCO };
    std::atomic<states> state;

    std::atomic<bool> isPaused = true;
    std::atomic<bool> isRunning = false;
    std::atomic<bool> playlistEnd = false;
    std::atomic<bool> notify = true, flushed = false;
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
    void enableZeroConf(void);

    void runTask();
public:
    inline static std::string username = "", password = "";

    CSpotPlayer(char* name, char* id, char *credentials, struct in_addr addr, AudioFormat audio, char* codec, bool flow,
        int64_t contentLength, struct shadowPlayer* shadow, pthread_mutex_t* mutex);
    ~CSpotPlayer();
    void disconnect(bool abort = false);

    void friend notify(CSpotPlayer *self, enum shadowEvent event, va_list args);
    bool friend getMetaForUrl(CSpotPlayer* self, const std::string url, metadata_t* metadata);
};

CSpotPlayer::CSpotPlayer(char* name, char* id, char *credentials, struct in_addr addr, AudioFormat format, char* codec, bool flow,
    int64_t contentLength, struct shadowPlayer* shadow, pthread_mutex_t* mutex) : bell::Task("playerInstance",
        48 * 1024, 0, 0),
    clientConnected(1), codec(codec), id(id), addr(addr), flow(flow),
    name(name), credentials(credentials), format(format), shadow(shadow), 
    playerMutex(mutex) {
    this->contentLength = (flow && contentLength == HTTP_CL_REAL) ? HTTP_CL_NONE : contentLength;
}

CSpotPlayer::~CSpotPlayer() {
    state = ABORT;
    isRunning = false;
    CSPOT_LOG(info, "player <%s> deletion pending", name.c_str());

    // unlock ourselves as we might be waiting
    clientConnected.give();

    // manually unregister mDNS but all other item should be deleted automatically
    if (mdnsService) mdnsService->unregisterService();

    // cleanup HTTP server
    if (server) server->close();

    // then just wait
    std::scoped_lock lock(this->runningMutex);
    CSPOT_LOG(info, "done", name.c_str());
}

size_t CSpotPlayer::writePCM(uint8_t* data, size_t bytes, std::string_view trackUnique) {
    // make sure we don't have a dead lock with a disconnect()
    if (!isRunning || isPaused) return 0;

#ifndef SMART_FLUSH
    if (flushed) return 0;
#endif

    std::lock_guard lock(playerMutex);

    if (streamTrackUnique != trackUnique) {
        // we can only accept 2 players (UPnP nextURI is one max)
        if (streamers.size() > 1) return 0;

#ifdef SMART_FLUSH
        flushed = false;
#endif
        CSPOT_LOG(info, "trackUniqueId update %s => %s", streamTrackUnique.c_str(), trackUnique.data());
        streamTrackUnique = trackUnique;
        trackHandler(trackUnique);
    }

#ifdef SMART_FLUSH
    if (flushed) return bytes;
#endif

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
#ifdef SMART_FLUSH
        // when flushed in this mode, ignore first PLAYBACK_START
        if (flushed && streamTrackUnique != player->trackUnique) {
            streamers.clear();
            // make sure we don't falsy detect the re-send of current track
            streamTrackUnique = player->trackUnique;
            break;
        }
#endif
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
        playlistEnd = false;

#ifndef SMART_FLUSH
        // exit flushed state while transferring that to notify
        notify = !flushed;
        flushed = false;
#endif

        // Spotify servers do not send volume at connection
        spirc->setRemoteVolume(volume);
        break;
    }
    case cspot::SpircHandler::EventType::PLAY_PAUSE: {
        std::scoped_lock lock(playerMutex);
        isPaused = std::get<bool>(event->data);
        CSPOT_LOG(info, isPaused ? "Pause" : "Play");
        if (player || !streamers.empty()) {
            shadowRequest(shadow, isPaused ? SPOT_PAUSE : SPOT_PLAY);
        }
        break;
    }
    case cspot::SpircHandler::EventType::FLUSH: {
        std::scoped_lock lock(playerMutex);
        CSPOT_LOG(info, "flush");
        flushed = true;
#ifndef SMART_FLUSH
        shadowRequest(shadow, SPOT_STOP);
#endif
        break;
    }
    case cspot::SpircHandler::EventType::NEXT:
    case cspot::SpircHandler::EventType::PREV: {  
        std::scoped_lock lock(playerMutex);
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
            CSPOT_LOG(info, "trying to seek before track has started");
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
        playlistEnd = true;
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
        CSPOT_LOG(info, "started track id %s => <%s>", flowTrackInfo.trackId.c_str(), flowTrackInfo.name.c_str());
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
            if (self->notify) self->spirc->notifyAudioReachedPlayback();
            else self->notify = true;
        }
        break;
    }
    case SHADOW_TRACK: {
        auto url = std::string(va_arg(args, char*));

        // nothing to do if we are already the active player
        if (self->streamers.empty() || (self->player && url.find(self->player->getStreamUrl()) != std::string::npos)) return;    

        // remove previous streamers till we reach new url (should be only one)
        while (url.find(self->streamers.back()->getStreamUrl()) == std::string::npos) {
            self->streamers.pop_back();
            // we should NEVER be here
            if (self->streamers.empty()) return;
        }

        // now we can set current player
        self->player = self->streamers.back();

        // finally, get ready for time position and inform spotify that we are playing
        self->lastPosition = 0;
        if (self->notify) self->spirc->notifyAudioReachedPlayback();
        else self->notify = true;

        // avoid weird cases where position is either random or last seek (will be corrected by SHADOW_TIME)
        self->spirc->updatePositionMs(0);

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
        if (self->player && self->playlistEnd) {
            self->playlistEnd = false;
            self->spirc->notifyAudioEnded();
        } else {
            // disconnect on unexpected STOP (free up player from Spotify)
            self->disconnect(true);
        }
        break;
    default:
        break;
    }
}

void CSpotPlayer::disconnect(bool abort) {
    // shared playerMutex is already locked
    CSPOT_LOG(info, "Disconnecting %s", name.c_str());
    state = abort ? ABORT : DISCO;
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

    if (req) {
        // see if parent wants to add headers in response
        for (auto resp = shadowHeaders(shadow, req); resp;) {
            response[resp->key] = resp->value;
            free(resp->key);
            free(resp->value);
            auto item = resp;
            resp = resp->next;
            free(item);
        }
        // free the C header structure
        do {
            auto item = req;
            req = req->next;
            delete item;
        } while (req);
     }
   
    return response;
}

void CSpotPlayer::enableZeroConf(void) {
    int serverPort = 0;
    server = std::make_unique<bell::BellHTTPServer>(serverPort);
    serverPort = server->getListeningPorts()[0];

    CSPOT_LOG(info, "ZeroConf mode (port %d)", serverPort);

    server->registerGet("/spotify_info", [this](struct mg_connection* conn) {
        return server->makeJsonResponse(this->blob->buildZeroconfInfo());
        });

    server->registerPost("/spotify_info", [this](struct mg_connection* conn) {
        return postHandler(conn);
        });

    // Register mdns service, for spotify to find us
    mdnsService = MDNSService::registerService(blob->getDeviceName(), "_spotify-connect", "_tcp", "", serverPort,
        { {"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"} });
}

void CSpotPlayer::runTask() {
    std::scoped_lock lock(this->runningMutex);
    isRunning = true;
    bool zeroConf = false;

    blob = std::make_unique<cspot::LoginBlob>(name);

    if (!username.empty() && !password.empty()) {
        blob->loadUserPass(username, password);
        CSPOT_LOG(info, "User/Password mode");
    }
    else if (!credentials.empty()) {
        blob->loadJson(credentials);
        CSPOT_LOG(info, "Reusable credentials mode");
    } else {
        zeroConf = true;
        enableZeroConf();
    }

    // gone with the wind...
    while (isRunning) {
        // with zeroConf we are active as soon as we received a connection
        if (zeroConf) clientConnected.wait();

        // we might just be woken up to exit
        if (!isRunning) break;
        state = LINKED;

        CSPOT_LOG(info, "Spotify client launched for %s", name.c_str());

        auto ctx = cspot::Context::createFromBlob(blob);
        ctx->config.audioFormat = format;

        // seems that mbedtls can catch error that are not fatal, so we should continue
        try {
            ctx->session->connectWithRandomAp();
        } catch (const std::runtime_error& e) {
            CSPOT_LOG(error, "AP connect error <%s> (try again later)", e.what());
            BELL_SLEEP_MS(1000);
            continue;
        }

        ctx->config.authData = ctx->session->authenticate(blob);

        // Auth successful
        if (ctx->config.authData.size() > 0) {
            // send credentials to owner in case it wants to do something with them
            shadowRequest(shadow, SPOT_CREDENTIALS, ctx->getCredentialsJson().c_str());

            spirc = std::make_unique<cspot::SpircHandler>(ctx);

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

            // exit when received an ABORT or a DISCO in ZeroConf mode 
            while (state == LINKED) {
                ctx->session->handlePacket();
                if (state == DISCO && !zeroConf) state = LINKED;
            }

            spirc->disconnect();
            spirc.reset();
            CSPOT_LOG(info, "disconnecting player <%s>", name.c_str());
        } else {
            CSPOT_LOG(error, "failed authentication, forcing ZeroConf");
            if (!zeroConf) enableZeroConf();
            zeroConf = true;
        }
    }

    CSPOT_LOG(info, "terminating player <%s>", name.c_str());
}

/****************************************************************************************
 * C interface functions
 */

void spotOpen(uint16_t portBase, uint16_t portRange, char *username, char* password) {
    if (!bell::bellGlobalLogger) {
        bell::setDefaultLogger();
        bell::enableTimestampLogging(true);
    }
    HTTPstreamer::portBase = portBase;
    if (portRange) HTTPstreamer::portRange = portRange;
    if (username) CSpotPlayer::username = username;
    if (password) CSpotPlayer::password = password;
}

void spotClose(void) {
    delete bell::bellGlobalLogger;
}

struct spotPlayer* spotCreatePlayer(char* name, char *id, char * credentials, struct in_addr addr, int oggRate, 
                                        char *codec, bool flow, int64_t contentLength, 
                                        struct shadowPlayer* shadow, pthread_mutex_t *mutex) {
    AudioFormat format = AudioFormat_OGG_VORBIS_160;

    if (oggRate == 320) format = AudioFormat_OGG_VORBIS_320;
    else if (oggRate == 96) format = AudioFormat_OGG_VORBIS_96;

    auto player = new CSpotPlayer(name, id, credentials, addr, format, codec, flow, contentLength, shadow, mutex);
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
