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
#include "CentralAudioBuffer.h"
#include "protobuf/metadata.pb.h"
extern "C" {
#include "cross_util.h"
#include "raop_client.h"
}

#include "spotify.h"
#include "metadata.h"

#define BYTES_PER_FRAME 4

static uint16_t portBase, portRange;

/****************************************************************************************
 * Chunk manager class (task)
 */

class chunkManager : public bell::Task {
public:
    std::atomic<bool> isRunning = true;
    std::atomic<bool> isPaused = true;
    chunkManager(std::function<void()> trackHandler, std::function<size_t(uint8_t* pcm, size_t size)> pcmWrite);
    size_t writePCM(uint8_t* data, size_t bytes, std::string_view trackId, size_t sequence);
    void teardown();
    void flush();

private:
    std::atomic<bool> flushed = false;
    std::unique_ptr<bell::CentralAudioBuffer> centralAudioBuffer;
    std::function<void()> trackHandler;
    std::function<size_t(uint8_t*, size_t)> raopWrite;
    std::mutex runningMutex;
    
    void runTask() override;
};

chunkManager::chunkManager(std::function<void()> trackHandler, std::function<size_t(uint8_t* pcm, size_t size)> raopWrite)
    : bell::Task("player", 16 * 1024, 0, 0) {
    this->isPaused = false;
    this->centralAudioBuffer = std::make_unique<bell::CentralAudioBuffer>(128);
    this->trackHandler = trackHandler;
    this->raopWrite = raopWrite;
    startTask();
}

void chunkManager::runTask() {
    std::scoped_lock lock(runningMutex);
    bell::CentralAudioBuffer::AudioChunk* chunk = NULL;
    size_t lastHash = 0, offset = 0;
    
    while (isRunning) {

        if (isPaused) {
            BELL_SLEEP_MS(25);
            continue;
        }

        // get another chunk if needed
        if (!chunk || chunk->pcmSize == 0 || flushed) {
            offset = 0;
            flushed = false;
            chunk = centralAudioBuffer->readChunk();
        }

        if (chunk && chunk->pcmSize != 0) {
            // receiving first chunk of new track from Spotify server
            if (lastHash != chunk->trackHash) {
                CSPOT_LOG(info, "hash update %x => %x", lastHash, chunk->trackHash);
                lastHash = chunk->trackHash;
                trackHandler();
            }

            size_t written = raopWrite(chunk->pcmData + offset, chunk->pcmSize - offset);
            offset += written;

            if (written) {
                if (offset == chunk->pcmSize) chunk = NULL;
            } else {
                BELL_SLEEP_MS(25);
            }
        }
    }
}

size_t chunkManager::writePCM(uint8_t* data, size_t bytes, std::string_view trackId, size_t sequence) {
    return centralAudioBuffer->writePCM(data, bytes, sequence);
}

void chunkManager::teardown() {
    isRunning = false;
    std::scoped_lock lock(runningMutex);
}

void chunkManager::flush() {
    centralAudioBuffer->clearBuffer();
    flushed = true;
}

/****************************************************************************************
 * Player's main class  & task
 */

class CSpotPlayer : public bell::Task {
private:     
    std::mutex runningMutex;
    bell::WrappedSemaphore clientConnected;
       
    std::string name;
    struct in_addr addr;
    AudioFormat format;
    int volume = 0;
    struct shadowPlayer* shadow;
    struct raopcl_s* raopClient;

    cspot::CDNTrackStream::TrackInfo trackInfo;
    int startOffset;
    uint64_t startTime = 0;
    std::atomic<uint64_t> stopTime = 0;
    uint64_t pauseElapsed;
    bool streamAck;
    size_t frameSize;
    uint32_t delay;
    size_t scratchSize;
    uint8_t scratch[MAX_SAMPLES_PER_CHUNK * BYTES_PER_FRAME];

    std::unique_ptr<bell::MDNSService> mdnsService;

    std::unique_ptr<bell::BellHTTPServer> server;
    std::shared_ptr<cspot::LoginBlob> blob;
    std::unique_ptr<cspot::SpircHandler> spirc;
    std::unique_ptr<chunkManager> chunker;
    
    auto postHandler(struct mg_connection* conn);
    void eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event);
    void trackHandler(void);
    
    void runTask();
public:
    typedef enum { TRACK_INIT, TRACK_READY, TRACK_STREAMING, TRACK_END } TrackStatus;
    std::atomic<TrackStatus> trackStatus = TRACK_INIT;

    CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat audio, 
                size_t frameSize, uint32_t delay, struct shadowPlayer* shadow);
    ~CSpotPlayer();
    std::atomic<bool> isRunning = false;
    void teardown();
    void disconnect();
    void notify(enum shadowEvent event, va_list args);
};

CSpotPlayer::CSpotPlayer(char* name, char* id, struct in_addr addr, AudioFormat format, 
                         size_t frameSize, uint32_t delay, struct shadowPlayer* shadow) 
            : bell::Task("playerInstance", 48 * 1024, 0, 0), 
             clientConnected(1), addr(addr), name(name), shadow(shadow), 
             frameSize(frameSize), delay(delay) {
    this->raopClient = shadowRaop(shadow);
}

CSpotPlayer::~CSpotPlayer() {
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

void CSpotPlayer::trackHandler(void) {
    /* In theory we should wait for "delay" before notifying playback. Also,
     * note that we can't move to TRACK_PENDING here as we have not yet 
     * received metadata (duration) */
    if (trackStatus != TRACK_INIT) startOffset = 0;
    startTime = gettime_ms64() + delay;
}

/* Most events are handled locally when possible. They could indeed be fowarded to the 
 * shadow player but it just add messages for nothing, this part is really the glue, so
 * do as much as possible here
 */
 void CSpotPlayer::eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event) {
    switch (event->eventType) {
    case cspot::SpircHandler::EventType::PLAYBACK_START: {
        chunker->flush();
        scratchSize = 0;
        startTime = 0;
        trackStatus = TRACK_INIT;

        CSPOT_LOG(info, "Start with track id %s => <%s>", spirc->getTrackPlayer()->getCurrentTrackInfo().trackId.c_str(),
                         spirc->getTrackPlayer()->getCurrentTrackInfo().name.c_str());

        // need to let shadow do as we don't know player's IP and port
        startOffset = std::get<int>(event->data);
        shadowRequest(shadow, SPOT_LOAD);

        // Spotify servers do not send volume at connection
        spirc->setRemoteVolume(volume);
        break;
    }
    case cspot::SpircHandler::EventType::TRACK_INFO: {
        metadata_t metadata = { 0 };
        trackInfo = std::get<cspot::CDNTrackStream::TrackInfo>(event->data);
        metadata.title = trackInfo.name.c_str();
        metadata.album = trackInfo.album.c_str();
        metadata.artist = trackInfo.artist.c_str();
        metadata.artwork = trackInfo.imageUrl.c_str();
        metadata.duration = trackInfo.duration;
        CSPOT_LOG(info, "Got next track id %s => <%s>", trackInfo.trackId.c_str(), trackInfo.name.c_str());

        // need to let shadow do as we don't know if metadata are allowed
        shadowRequest(shadow, SPOT_METADATA, &metadata);

        // ready for setting progress when track has started
        trackStatus = TRACK_READY;
        break;
    }
    case cspot::SpircHandler::EventType::PLAY_PAUSE: {
        chunker->isPaused = std::get<bool>(event->data);
        if (chunker->isPaused) {
            raopcl_pause(raopClient);
            raopcl_flush(raopClient);
            stopTime = gettime_ms64() + 15 * 1000;
            startOffset = raopcl_get_progress_ms(raopClient);
        } else {
            // if we are starting from a timeout disconnect, need to resend everything
            if (!stopTime && trackStatus == TRACK_STREAMING) {
                metadata_t metadata = { 0 };
                metadata.title = trackInfo.name.c_str();
                metadata.album = trackInfo.album.c_str();
                metadata.artist = trackInfo.artist.c_str();
                metadata.artwork = trackInfo.imageUrl.c_str();
                metadata.duration = trackInfo.duration;
                CSPOT_LOG(info, "Restart after disconnected");

                // need to let shadow do as we don't know if metadata are allowed
                shadowRequest(shadow, SPOT_METADATA, &metadata);
                trackStatus = TRACK_READY;

            }
            stopTime = 0;
            shadowRequest(shadow, SPOT_PLAY);
        }
        break;
    }
    case cspot::SpircHandler::EventType::NEXT:
    case cspot::SpircHandler::EventType::PREV:
        chunker->flush();
        raopcl_flush(raopClient);
        break;
    case cspot::SpircHandler::EventType::FLUSH:
        // sent when there is no next but NEXT is pressed
        chunker->flush();
        raopcl_disconnect(raopClient);
        break;
    case cspot::SpircHandler::EventType::DISC:
        disconnect();
        break;
    case cspot::SpircHandler::EventType::SEEK:
        chunker->flush();
        scratchSize = 0;
        startOffset = std::get<int>(event->data);
        raopcl_flush(raopClient);
        // must be done last to make sure the busy loop does not act before
        trackStatus = TRACK_READY;
        break;
    case cspot::SpircHandler::EventType::DEPLETED:
        trackStatus = TRACK_END;
        stopTime = gettime_ms64() + delay + 5000;
        CSPOT_LOG(info, "Playlist ended, no track left to play");
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
     // always accept volume command
     if (event == SHADOW_VOLUME) {
         int volume = va_arg(args, int);
        if (spirc) spirc->setRemoteVolume(volume);
        this->volume = volume;
        return;
    }

    // might have no spirc
    if (!spirc) return;
    
    switch (event) {      
    case SHADOW_PLAY:
        spirc->setPause(false);
        break;
    case SHADOW_PAUSE:
        spirc->setPause(true);
        break;
    case SHADOW_STOP:
        // @FIXME: is there some case for teardown?
        spirc->setPause(true);
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

    mdnsService->unregisterService();

    // then just wait    
    std::scoped_lock lock(this->runningMutex);

    CSPOT_LOG(info, "Player %s fully stopped", name.c_str());
}

void CSpotPlayer::disconnect() {
    CSPOT_LOG(info, "Disconnecting %s", name.c_str());
    chunker->flush();
    raopcl_disconnect(raopClient);
    chunker->teardown();
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
    mdnsService = MDNSService::registerService(blob->getDeviceName(), "_spotify-connect", "_tcp", "", serverPort,
            { {"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"} });

    // gone with the wind...
    while (isRunning) {
        clientConnected.wait();

        // we might just be woken up to exit
        if (!isRunning) break;

        CSPOT_LOG(info, "Spotified client connected for %s", name.c_str());

        auto ctx = cspot::Context::createFromBlob(blob);
        ctx->config.audioFormat = format;

        ctx->session->connectWithRandomAp();
        auto token = ctx->session->authenticate(blob);

        // Auth successful
        if (token.size() > 0) {
            spirc = std::make_unique<cspot::SpircHandler>(ctx);

            // Create a player, pass the tack handler
            chunker = std::make_unique<chunkManager>(
                [this](void) {
                    return trackHandler();
                },
                [this](uint8_t* pcm, size_t bytes) {
                    if (!raopcl_accept_frames(raopClient)) return (size_t)0;

                    // do we have enough samples (unless it's last packet)
                    if (trackStatus != TRACK_END && scratchSize + bytes < frameSize * BYTES_PER_FRAME) {
                        memcpy(scratch + scratchSize, pcm, bytes);
                        scratchSize += bytes;
                        return bytes;
                    }

                    uint8_t* data = pcm;
                    uint64_t playtime;
                    size_t consumed = min(bytes, frameSize * BYTES_PER_FRAME);

                    // use optional residual data from previous chunk                        
                    if (scratchSize) {
                        consumed = min(frameSize * BYTES_PER_FRAME - scratchSize, bytes);
                        memcpy(scratch + scratchSize, pcm, consumed);
                        data = scratch;
                    }

                    // sending chunk will exit FLUSHED state (might be last packet)
                    raopcl_send_chunk(raopClient, data, (consumed + scratchSize) / BYTES_PER_FRAME, &playtime);
                    scratchSize = 0;

                    // once we are not FLUSHED, position can be updated
                    if (trackStatus == TRACK_READY) {
                        CSPOT_LOG(info, "Setting track position %d / %d", startOffset, trackInfo.duration);
                        raopcl_set_progress_ms(raopClient, startOffset, trackInfo.duration);
                        spirc->updatePositionMs(startOffset - delay);
                        trackStatus = TRACK_STREAMING;
                    }

                    return consumed;
                });

            // set call back to calculate a hash on trackId
            spirc->getTrackPlayer()->setDataCallback(
                [this](uint8_t* data, size_t bytes, std::string_view trackId, size_t sequence) {
                    return chunker->writePCM(data, bytes, trackId, sequence);
                });

            // set event (PLAY, VOLUME...) handler
            spirc->setEventHandler(
                [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
                    eventHandler(std::move(event));
            });

            // Start handling mercury messages
            ctx->session->startTask();

            // exit when player has stopped (received a DISC)
            while (chunker->isRunning) {
                ctx->session->handlePacket();

                if (startTime && gettime_ms64() >= startTime) {
                    spirc->notifyAudioReachedPlayback();
                    startTime = 0;
                } else if (stopTime && gettime_ms64() >= stopTime) {
                    stopTime = 0;
                    raopcl_disconnect(raopClient);

                    if (trackStatus == TRACK_END) {
                        CSPOT_LOG(info, "Last track finished");
                        trackStatus = TRACK_INIT;
                        spirc->setPause(true);
                    } else {
                        CSPOT_LOG(info, "Teardown RAOP connection on timeout at %d", startOffset);
                    }
                }
            }

            spirc->disconnect();
            CSPOT_LOG(info, "Disconnecting player %s", name.c_str());
        }
    }

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

struct spotPlayer* spotCreatePlayer(char* name, char *id, struct in_addr addr, int oggRate, size_t frameSize, uint32_t delay, struct shadowPlayer* shadow) {
    AudioFormat format = AudioFormat_OGG_VORBIS_160;

    if (oggRate == 320) format = AudioFormat_OGG_VORBIS_320;
    else if (oggRate == 96) format = AudioFormat_OGG_VORBIS_96;

    auto player = new CSpotPlayer(name, id, addr, format, frameSize, delay, shadow);
    if (player->startTask()) return (struct spotPlayer*) player;

    delete player;
    return NULL;
}

void spotDeletePlayer(struct spotPlayer* spotPlayer) {
    auto player = (CSpotPlayer*) spotPlayer;
    player->teardown();
    delete player;
}

void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...) {
    va_list args;
    va_start(args, event);
    ((CSpotPlayer*) spotPlayer)->notify(event, args);
    va_end(args);
}


