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
#include <ApResolve.h>
#include "MDNSService.h"
#include "SpircHandler.h"
#include "LoginBlob.h"
#include "BellHTTPServer.h"
#
#include "CentralAudioBuffer.h"
#include "Logger.h"
#include "Utils.h"

#include "HTTPstreamer.h"

#include "spotify.h"
#include "metadata.h"
#include "time.h"
#include "codecs.h"

static uint16_t portBase, portRange;

/****************************************************************************************
 * Chunk manager class (task)
 */

class chunkManager : public bell::Task {
public:
    std::atomic<bool> isRunning = true;
    std::atomic<bool> isPaused = true;
    chunkManager(std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer, std::function<void()> trackHandler, std::function<bool(const uint8_t*, size_t)> audioHandler);
    void teardown();

private:
    std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer;
    std::function<void()> trackHandler;
    std::function<bool(uint8_t*, size_t)> audioHandler;
    std::mutex runningMutex;
    
    void runTask() override;
};

chunkManager::chunkManager(std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer, 
                            std::function<void()> trackHandler, std::function<bool(const uint8_t*, size_t)> audioHandler)
    : bell::Task("player", 16 * 1024, 0, 0) {
    this->isPaused = false;
    this->centralAudioBuffer = centralAudioBuffer;
    this->trackHandler = trackHandler;
    this->audioHandler = audioHandler;
    startTask();
}

void chunkManager::runTask() {
    std::scoped_lock lock(runningMutex);
    bell::CentralAudioBuffer::AudioChunk* chunk = NULL;
    size_t lastHash = 0;
    
    while (isRunning) {
        if (!isPaused) {
            if (!chunk) {
                chunk = centralAudioBuffer->readChunk();
            }

            if (!chunk || chunk->pcmSize == 0) {
                BELL_SLEEP_MS(25);
                chunk = NULL;
                continue;
            }

            // receiving first chunk of new track from Spotify server
            if (lastHash != chunk->trackHash) {
                CSPOT_LOG(info, "hash update %x => %x", lastHash, chunk->trackHash);
                lastHash = chunk->trackHash;
                trackHandler();
            }

            if (audioHandler(chunk->pcmData, chunk->pcmSize)) {
                chunk = NULL;
            } else {
                BELL_SLEEP_MS(25);
            }
        } else {
            BELL_SLEEP_MS(25);
        }
    }
}

void chunkManager::teardown() {
    isRunning = false;
    std::scoped_lock lock(runningMutex);
}

/****************************************************************************************
 * Player's main class  & task
 */

class CSpotPlayer : public bell::Task {
private:
    std::string name;
        
    std::mutex runningMutex;
    bell::WrappedSemaphore clientConnected;
    std::shared_ptr<bell::CentralAudioBuffer> centralAudioBuffer;
    int volume = 0;
    int32_t startOffset;
    std::atomic<int> expectedSync;
    std::unique_ptr<cspot::CDNTrackStream::TrackInfo> flowTrackInfo;
    
    unsigned index = 0;

    std::string codec, id;
    struct in_addr addr;
    AudioFormat format;
    bool flow;
    int64_t contentLength;

    struct shadowPlayer* shadow;
    std::unique_ptr<bell::MDNSService> mdnsService;

    // vector would be enough but it does not seem to be any faster than deque
    std::deque<std::shared_ptr<HTTPstreamer>> streamers;
    std::mutex streamersMutex;

    std::unique_ptr<bell::BellHTTPServer> server;
    std::shared_ptr<cspot::LoginBlob> blob;
    std::unique_ptr<cspot::SpircHandler> spirc;
    std::unique_ptr<chunkManager> chunker;
    
    auto postHandler(struct mg_connection* conn);
    void eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event);
    void trackHandler(void);
    HTTPheaders onHeaders(HTTPheaders request);
    void flowSync(cspot::CDNTrackStream::TrackInfo *trackInfo);
    
    void runTask();
public:
    CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat audio, char* codec,  bool flow, 
                int64_t contentLength, struct shadowPlayer* shadow);
    ~CSpotPlayer();
    std::atomic<bool> isRunning = false;
    void teardown();
    void disconnect();
    bool getMetaForUrl(const char* StreamUrl, metadata_t *metadata);
    void notify(enum shadowEvent event, va_list args);
};

CSpotPlayer::CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat format, char *codec, bool flow, 
                         int64_t contentLength, struct shadowPlayer* shadow) : bell::Task("playerInstance", 
                         48 * 1024, 0, 0), 
                         clientConnected(1), codec(codec), id(id), addr(addr), flow(flow), 
                         name(name), format(format), shadow(shadow) {
    this->contentLength = (flow && !contentLength) ? HTTP_CL_NONE : contentLength;  
}

CSpotPlayer::~CSpotPlayer() {
}

auto CSpotPlayer::postHandler(struct mg_connection* conn) {
    nlohmann::json obj;
    // Prepare a success response for spotify
    obj["status"] = 101;
    obj["spotifyError"] = 0;
    obj["statusString"] = "ERROR-OK";

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

    return server->makeJsonResponse(obj.dump());
}

void CSpotPlayer::trackHandler(void) {
    // protect against an unlikely race with eventHandler
    std::scoped_lock lock(streamersMutex);

    // existing streamer enters draining state, expect in flow mode
    if (!streamers.empty() && !flow) {
        for (auto it = streamers.begin(); it != streamers.end();) {
            if ((*it)->state == HTTPstreamer::DRAINED) it = streamers.erase(it);
            else ++it;
        }
        streamers.front()->state = HTTPstreamer::DRAINING;
        CSPOT_LOG(info, "Draining track %s", streamers.front()->streamId.c_str());
    }

    // this is the track that is streamed, not one on air (if any)
    auto newTrackInfo = spirc->getTrackPlayer()->getCurrentTrackInfo();
    CSPOT_LOG(info, "got next track id %s => <%s>", newTrackInfo.trackId.c_str(), newTrackInfo.name.c_str());

    // create a new streamer an run it, unless in flow mode
    if (streamers.empty() || !flow) {
        auto streamer = std::make_shared<HTTPstreamer>(addr, id, index++, codec, flow, contentLength,
                                                       newTrackInfo, streamers.empty() ? -startOffset : 0,
                                                       [this](std::map<std::string, std::string> headers) {
                                                            return this->onHeaders(headers);
                                                        } );
        // be careful that streamer's offset is negative
        metadata_t metadata = { 0 };
        streamer->getMetadata(&metadata);
        metadata.duration = flow ? 0 : (metadata.duration + streamer->offset);

        // position is optional, shadow player might use it or not
        shadowRequest(shadow, SPOT_LOAD, streamer->getStreamUrl().c_str(), &metadata, (uint32_t)  -streamer->offset);
        if (!chunker->isPaused) shadowRequest(shadow, SPOT_PLAY);

        streamers.emplace_front(streamer);
        streamer->startTask();
    } else {
        // 2nd or more track in flow mode
        auto streamer = streamers.front();

        if (streamer->sync != HTTPstreamer::AIRED) {
            CSPOT_LOG(info, "waiting for flow track %s to be aired", streamer->streamId.c_str());
            this->flowTrackInfo = std::make_unique<cspot::CDNTrackStream::TrackInfo>(newTrackInfo);
        } else {
            flowSync(&newTrackInfo);
        }
    }

    // we need to acquire synchronization
    expectedSync++;
}

void CSpotPlayer::flowSync(cspot::CDNTrackStream::TrackInfo *trackInfo) {
    auto streamer = streamers.front();

    // accumulate offsetand update track's info and acquire time position
    streamer->offset += streamers.front()->trackInfo.duration;
    streamer->trackInfo = *trackInfo;
    streamer->sync = HTTPstreamer::WAIT_CROSSTIME;
}

 void CSpotPlayer::eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event) {
    switch (event->eventType) {
    case cspot::SpircHandler::EventType::PLAYBACK_START: {
        centralAudioBuffer->clearBuffer();
        shadowRequest(shadow, SPOT_STOP);

        // memorize position for when track's beginning will be detected
        startOffset = std::get<int>(event->data);
        expectedSync = 0;
        flowTrackInfo.reset();

        CSPOT_LOG(info, "start with track id %s => <%s>", spirc->getTrackPlayer()->getCurrentTrackInfo().trackId.c_str(),
                         spirc->getTrackPlayer()->getCurrentTrackInfo().name.c_str());

        // unlikely but we still might conflict with chunkManager task's loop
        std::scoped_lock lock(streamersMutex);

        // remove all streamers (shared_ptr's destructor will stop the tasks)
        streamers.clear();

        // Spotify servers do not send volume at connection
        spirc->setRemoteVolume(volume);
        break;
    }
    case cspot::SpircHandler::EventType::PLAY_PAUSE:
        chunker->isPaused = std::get<bool>(event->data);
        if (!streamers.empty()) {
            shadowRequest(shadow, chunker->isPaused ? SPOT_PAUSE : SPOT_PLAY);
        }
        break;
    case cspot::SpircHandler::EventType::NEXT:
    case cspot::SpircHandler::EventType::PREV:
    case cspot::SpircHandler::EventType::FLUSH: {
        // send when there is no next, just clean everything
        centralAudioBuffer->clearBuffer();
        shadowRequest(shadow, SPOT_STOP);
        std::scoped_lock lock(streamersMutex);
        streamers.clear();
        break;
    }
    case cspot::SpircHandler::EventType::DISC:
        disconnect();
        break;
    case cspot::SpircHandler::EventType::SEEK: {
        /* Seek does not exist for shadow's player but we need to keep the current streamer. So
         * first stop frame feeding then empty pcm buffer and finally stop that should close the
         * current connection and PLAY should open a new one, all on the same url/streamer */
        centralAudioBuffer->clearBuffer();

        std::scoped_lock lock(streamersMutex);

        // remove all streamers except the on the is being aired (if any)
        if (streamers.size() > 1) for (auto it = streamers.begin(); it != streamers.end();) {
            if ((*it)->sync != HTTPstreamer::AIRED) it = streamers.erase(it);
            else ++it;
        }

        auto streamer = streamers.front();
        streamer->flush();
        shadowRequest(shadow, SPOT_STOP);

        // be careful that streamer's offset is negative
        metadata_t metadata = { 0 };
        streamer->getMetadata(&metadata);
        streamer->offset = -std::get<int>(event->data);
        streamer->setContentLength(contentLength);
        metadata.duration = flow ? 0 : (metadata.duration + streamer->offset);

        // if track is already playing, need to re-synchronize ONLY time with Spotify
        if (streamer->sync > HTTPstreamer::WAIT_URL) streamer->sync = HTTPstreamer::WAIT_TIME;
        expectedSync = 1;

        shadowRequest(shadow, SPOT_LOAD, streamer->getStreamUrl().c_str(), &metadata, -streamer->offset);
        if (!chunker->isPaused) shadowRequest(shadow, SPOT_PLAY);
        break;
    }
    case cspot::SpircHandler::EventType::DEPLETED:
        for (auto it = streamers.begin(); it != streamers.end();) {
            if ((*it)->state == HTTPstreamer::DRAINED) it = streamers.erase(it);
            else (*it++)->state = HTTPstreamer::DRAINING;
        }
        CSPOT_LOG(info, "playlist ended, no track left to play");
        break;
    case cspot::SpircHandler::EventType::VOLUME:
        volume = std::get<int>(event->data);
        shadowRequest(shadow, SPOT_VOLUME, volume);
        break;
    default:
        break;
    }
}

void CSpotPlayer::notify(enum shadowEvent event, va_list args) {
    // volume can be handled at anytime
    if (event == SHADOW_VOLUME) {
        int volume = va_arg(args, int);
        if (spirc) spirc->setRemoteVolume(volume);
        this->volume = volume;
        return;
    }

    if (!spirc) return;
    
    switch (event) {
    case SHADOW_TIME: {
        if (!expectedSync) break;
        
        uint32_t position = va_arg(args, uint32_t);
        std::scoped_lock lock(streamersMutex);

        for (auto it = streamers.rbegin(); it != streamers.rend(); ++it) {
            if (((*it)->sync != HTTPstreamer::WAIT_TIME || position > 10000) &&
                ((*it)->sync != HTTPstreamer::WAIT_CROSSTIME || position < (*it)->offset)) continue;

            // we have to wait to have acquired all parameters before moving to next track
            if ((*it)->sync == HTTPstreamer::WAIT_CROSSTIME) {
                CSPOT_LOG(info, "track %s started by CROSSTIME", (*it)->streamId.c_str());
                spirc->notifyAudioReachedPlayback();
            }
        
            // now we have acquired time synchronization for that player, all done
            (*it)->sync = HTTPstreamer::AIRED;
            position -= (*it)->offset;
            spirc->updatePositionMs(position);

            // we might have a pending WAIT_CROSSTIME sync
            if (flowTrackInfo) {
                flowSync(flowTrackInfo.release());
            }

            expectedSync--;
            CSPOT_LOG(info, "updating position to %d (offset is %" PRId64 ")", position, streamers.front()->offset);
        }
        break;
    }
    case SHADOW_TRACK: {
        if (!expectedSync) break;

        char* url = va_arg(args, char*);
        std::scoped_lock lock(streamersMutex);

        for (auto it = streamers.rbegin(); it != streamers.rend(); ++it) {
            if ((*it)->sync != HTTPstreamer::WAIT_URL || (*it)->getStreamUrl() != url) continue;
            (*it)->sync = HTTPstreamer::WAIT_TIME;
            CSPOT_LOG(info, "track %s started by URL", (*it)->streamId.c_str());
            spirc->notifyAudioReachedPlayback();
        }
        break;
    }
    case SHADOW_PLAY:
        spirc->setPause(false);
        break;
    case SHADOW_PAUSE:
        spirc->setPause(true);
        break;
    case SHADOW_STOP:
        if (streamers.front()->state != HTTPstreamer::DRAINED) {
            // a non expected STOP is a disconnect, it frees up player from Spotify
            disconnect();
        } else {
            // otherwise it means we have finished playing
            spirc->setPause(true);
        }
        break;
    default:
        break;
    }
}

void CSpotPlayer::teardown() {
    isRunning = false;

    // we might never have been active, so there is no chunker
    if (chunker) chunker->teardown();
    clientConnected.give();

    // stop all streamers (shaed_ptr's destructor will be called)
    streamers.clear();
    mdnsService->unregisterService();

    // then just wait    
    std::scoped_lock lock(this->runningMutex);

    CSPOT_LOG(info, "Player %s fully stopped", name.c_str());
}

void CSpotPlayer::disconnect() {
    CSPOT_LOG(info, "Disconnecting %s", name.c_str());
    centralAudioBuffer->clearBuffer();
    shadowRequest(shadow, SPOT_STOP);
    chunker->teardown();
    // no need to protect streamers as the chunkManager is already down    
    for (auto it = streamers.begin(); it != streamers.end(); ++it) (*it)->teardown();
    streamers.clear();
}

bool CSpotPlayer::getMetaForUrl(const char* streamUrl, metadata_t *metadata) {
    for (auto it = streamers.begin(); it != streamers.end(); ++it) {
        if ((*it)->getStreamUrl() == streamUrl) {
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

        CSPOT_LOG(info, "Spotified client connected for %s", name.c_str());

        centralAudioBuffer = std::make_shared<bell::CentralAudioBuffer>(128);
        auto ctx = cspot::Context::createFromBlob(blob);
        ctx->config.audioFormat = format;

        ctx->session->connectWithRandomAp();
        auto token = ctx->session->authenticate(blob);

        // Auth successful
        if (token.size() > 0) {
            spirc = std::make_unique<cspot::SpircHandler>(ctx);

            // set call back to calculate a hash on trackId
            spirc->getTrackPlayer()->setDataCallback(
                [this](uint8_t* data, size_t bytes, std::string_view trackId, size_t sequence) {
                    return centralAudioBuffer->writePCM(data, bytes, sequence);
            });

            // set event (PLAY, VOLUME...) handler
            spirc->setEventHandler(
                [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
                    eventHandler(std::move(event));
            });

            // Start handling mercury messages
            ctx->session->startTask();

            // Create a player, pass the tack handler
            chunker = std::make_unique<chunkManager>(centralAudioBuffer,
                [this](void) {
                    return trackHandler();
                }, 
                [this](const uint8_t* data, size_t bytes) {
                    std::scoped_lock lock(streamersMutex);
                    return !streamers.empty() ? streamers.front()->feedPCMFrames(data, bytes) : true;
             });

            // exit when player has stopped (received a DISC)
            while (chunker->isRunning) {
                ctx->session->handlePacket();
            }

            spirc->disconnect();
            CSPOT_LOG(info, "disconnecting player %s", name.c_str());
        }
    }

    // TOOD: need to clean somethign here
    CSPOT_LOG(info, "Terminating player %s", name.c_str());
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
                                        char *codec, bool flow, int64_t contentLength, struct shadowPlayer* shadow) {
    AudioFormat format = AudioFormat_OGG_VORBIS_160;

    if (oggRate == 320) format = AudioFormat_OGG_VORBIS_320;
    else if (oggRate == 96) format = AudioFormat_OGG_VORBIS_96;

    auto player = new CSpotPlayer(name, id, addr, format, codec, flow, contentLength, shadow);
    if (player->startTask()) return (struct spotPlayer*) player;

    delete player;
    return NULL;
}

void spotDeletePlayer(struct spotPlayer* spotPlayer) {
    auto player = (CSpotPlayer*) spotPlayer;
    player->teardown();
    delete player;
}

bool spotGetMetaData(struct spotPlayer* spotPlayer, const char* streamUrl, metadata_t *metadata) {
    return ((CSpotPlayer*) spotPlayer)->getMetaForUrl(streamUrl, metadata);
 }

void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...) {
    va_list args;
    va_start(args, event);
    ((CSpotPlayer*) spotPlayer)->notify(event, args);
    va_end(args);
}


