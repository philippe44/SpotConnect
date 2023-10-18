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
#include "BellUtils.h"
#include "ApResolve.h"
#include "MDNSService.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "BellHTTPServer.h"
#include "WrappedSemaphore.h"
#include "protobuf/metadata.pb.h"
extern "C" {
#include "cross_util.h"
#include "raop_client.h"
}

#include "spotify.h"
#include "metadata.h"

#define BYTES_PER_FRAME 4

/****************************************************************************************
 * Player's main class  & task
 */

class CSpotPlayer : public bell::Task {
private:     
    std::mutex runningMutex;
    bell::WrappedSemaphore clientConnected;
    std::string streamTrackUnique;
    std::atomic<bool> isPaused = true;
    std::atomic<bool> isRunning = false, isConnected = false;
    std::string credentials;
    enum states { ABORT, LINKED, DISCO };
    std::atomic<states> state;
       
    std::string name;
    struct in_addr addr;
    AudioFormat format;
    int volume = 0;
    struct shadowPlayer* shadow;
    struct raopcl_s* raopClient;

    cspot::TrackInfo trackInfo;
    int startOffset;
    uint64_t startTime = 0;
    std::atomic<uint64_t> stopTime = 0;
    std::atomic<bool> flushed = false, notify = true;
    size_t frameSize;
    uint32_t delay;
    size_t scratchSize;
    uint8_t scratch[MAX_SAMPLES_PER_CHUNK * BYTES_PER_FRAME];

    std::unique_ptr<bell::MDNSService> mdnsService;

    std::unique_ptr<bell::BellHTTPServer> server;
    std::shared_ptr<cspot::LoginBlob> blob;
    std::unique_ptr<cspot::SpircHandler> spirc;
    
    void info2meta(metadata_t* metadata);
    auto postHandler(struct mg_connection* conn);
    void eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event);
    size_t writePCM(uint8_t* pcm, size_t bytes, std::string_view trackId);
    void enableZeroConf(void);
    
    void runTask();

public:
    typedef enum { TRACK_INIT, TRACK_READY, TRACK_STREAMING, TRACK_END } TrackStatus;
    std::atomic<TrackStatus> trackStatus = TRACK_INIT;
    inline static uint16_t portBase = 0, portRange = 1;
    inline static std::string username = "", password = "";

    CSpotPlayer(char* name, char* id, char *credentials, struct in_addr addr, AudioFormat audio, 
                size_t frameSize, uint32_t delay, struct shadowPlayer* shadow);
    ~CSpotPlayer();
    void disconnect(bool abort = false);
    void friend notify(CSpotPlayer *self, enum shadowEvent event, va_list args);
};

CSpotPlayer::CSpotPlayer(char* name, char* id, char *credentials, struct in_addr addr, AudioFormat format, 
                         size_t frameSize, uint32_t delay, struct shadowPlayer* shadow) 
            : bell::Task("playerInstance", 48 * 1024, 0, 0), 
            clientConnected(1), addr(addr), name(name), credentials(credentials), 
            shadow(shadow), frameSize(frameSize), delay(delay) {
    this->raopClient = shadowRaop(shadow);
}

void CSpotPlayer::disconnect(bool abort) {
    state = abort ? ABORT : DISCO;
    CSPOT_LOG(info, "Disconnecting %s", name.c_str());
    raopcl_disconnect(raopClient);
}

void CSpotPlayer::info2meta(metadata_t *metadata) {
    metadata->title = trackInfo.name.c_str();
    metadata->album = trackInfo.album.c_str();
    metadata->artist = trackInfo.artist.c_str();
    metadata->artwork = trackInfo.imageUrl.c_str();
    metadata->duration = trackInfo.duration;
}

CSpotPlayer::~CSpotPlayer() {
    isRunning = false;
    state = ABORT;
    clientConnected.give();
    CSPOT_LOG(info, "player <%s> deletion pending", name.c_str());

    if (mdnsService) mdnsService->unregisterService();

    // then just wait    
    std::scoped_lock lock(this->runningMutex);
    CSPOT_LOG(info, "done", name.c_str());
}

size_t CSpotPlayer::writePCM(uint8_t* pcm, size_t bytes, std::string_view trackUnique) {
    // make sure we don't have a dead lock with a disconnect()
    if (!isRunning || isPaused || flushed) return 0;

    if (streamTrackUnique != trackUnique) {
        CSPOT_LOG(info, "trackUniqueId update %s => %s", streamTrackUnique.c_str(), trackUnique.data());
        streamTrackUnique = trackUnique;

        if (trackStatus != TRACK_INIT) startOffset = 0;

        /* We could send the notifyAudio() here but that would be delay seconds too early so it's
         * not great to use timers instead of events but in that case it's for sure that we'll
         * start at the set time - airplay is just a long wire */
        startTime = gettime_ms64() + delay;
    }

    if (!raopcl_accept_frames(raopClient)) return (size_t) 0;

    // do we have enough samples (unless it's last packet)
    if (trackStatus != TRACK_END && scratchSize + bytes < frameSize * BYTES_PER_FRAME) {
        memcpy(scratch + scratchSize, pcm, bytes);
        scratchSize += bytes;
        return bytes;
    }

    uint8_t* data = pcm;
    uint64_t playtime;
    size_t consumed = min(bytes, frameSize * BYTES_PER_FRAME);

    // use optional residual data from previous call
    if (scratchSize) {
        consumed = min(frameSize * BYTES_PER_FRAME - scratchSize, bytes);
        memcpy(scratch + scratchSize, pcm, consumed);
        data = scratch;
    }

    // sending chunk will exit FLUSHED state (might be last packet)
    raopcl_send_chunk(raopClient, data, (consumed + scratchSize) / BYTES_PER_FRAME, &playtime);
    scratchSize = 0;

    return consumed;
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

/* Most events are handled locally when possible. They could indeed be fowarded to the
 * shadow player but it just add messages for nothing, this part is really the glue, so
 * do as much as possible here */
void CSpotPlayer::eventHandler(std::unique_ptr<cspot::SpircHandler::Event> event) {
    switch (event->eventType) {
    case cspot::SpircHandler::EventType::PLAYBACK_START: {
        streamTrackUnique.clear();
        scratchSize = 0;
        startTime = 0;
        trackStatus = TRACK_INIT;

        // exit flushed state while transferring that to notify
        notify = !flushed;
        flushed = false;

        // because we might start "paused", make sure we stop everything
        raopcl_stop(raopClient);
        raopcl_flush(raopClient);
        
        // need to let shadow do as we don't know player's IP and port
        startOffset = std::get<int>(event->data);
        shadowRequest(shadow, SPOT_LOAD);

        CSPOT_LOG(info, "new track will start at %d", startOffset);

        // Spotify servers do not send volume at connection
        spirc->setRemoteVolume(volume);
        break;
    }
    case cspot::SpircHandler::EventType::TRACK_INFO: {
        metadata_t metadata = { 0 };
        trackInfo = std::get<cspot::TrackInfo>(event->data);
        info2meta(&metadata);
        CSPOT_LOG(info, "Got next track id %s => <%s>", trackInfo.trackId.c_str(), trackInfo.name.c_str());

        // need to let shadow do as we don't know if metadata are allowed
        shadowRequest(shadow, SPOT_METADATA, &metadata);

        // ready for setting progress when track has started
        trackStatus = TRACK_READY;
        break;
    }
    case cspot::SpircHandler::EventType::PLAY_PAUSE: {
        isPaused = std::get<bool>(event->data);
        if (isPaused) {
            raopcl_pause(raopClient);
            raopcl_flush(raopClient);
            stopTime = gettime_ms64() + 15 * 1000;
            startOffset = raopcl_get_progress_ms(raopClient);
        } else {
            // if we are starting from a timeout disconnect, need to resend everything
            if (!stopTime && trackStatus == TRACK_STREAMING) {
                metadata_t metadata = { 0 };
                info2meta(&metadata);
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
    case cspot::SpircHandler::EventType::FLUSH:
        // we are in flushed state
        flushed = true;
    case cspot::SpircHandler::EventType::NEXT:
    case cspot::SpircHandler::EventType::PREV:
        raopcl_stop(raopClient);
        raopcl_flush(raopClient);
        break;
    case cspot::SpircHandler::EventType::DISC:
        disconnect();
        break;
    case cspot::SpircHandler::EventType::SEEK:
        // Don't create a false notifyAudio... if we have moved to next track
        if (trackInfo.duration - raopcl_get_progress_ms(raopClient) < delay) {
            CSPOT_LOG(info, "too close to end of track, can't seek");
            break;
        }

        scratchSize = 0;
        startOffset = std::get<int>(event->data);
        raopcl_stop(raopClient);
        raopcl_flush(raopClient);

        // must be done last to make sure the busy loop does not act before
        trackStatus = TRACK_READY;
        break;
    case cspot::SpircHandler::EventType::DEPLETED:
        trackStatus = TRACK_END;
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

void notify(CSpotPlayer* self, enum shadowEvent event, va_list args) {
     // always accept volume command
     if (event == SHADOW_VOLUME) {
        int volume = va_arg(args, int);
        if (self->spirc) self->spirc->setRemoteVolume(volume);
        self->volume = volume;
        return;
    }

    // might have no spirc
    if (!self->spirc) return;
    
    switch (event) {      
    case SHADOW_NEXT:
        self->spirc->nextSong();
        break;
    case SHADOW_PREV:
        self->spirc->previousSong();
        break;
    case SHADOW_PLAY:
        self->spirc->setPause(false);
        break;
    case SHADOW_PAUSE:
        self->spirc->setPause(true);
        break;
    case SHADOW_PLAY_TOGGLE:
        self->spirc->setPause(!self->isPaused);
        break;
    case SHADOW_STOP:
        self->disconnect(true);
        break;
    default:
        break;
    }
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
    } else if (!credentials.empty()) {
        blob->loadJson(credentials);
        CSPOT_LOG(info, "Reusable credentials mode");
    } else {
        zeroConf = true;
        enableZeroConf();
    }

    // gone with the wind...
    while (isRunning) {
        uint64_t keepAlive = 0;
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
        }
        catch (const std::runtime_error& e) {
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

            // exit when received an ABORT or a DISCO in ZeroConf mode 
            while (state == LINKED) {
                ctx->session->handlePacket();
                uint64_t now = gettime_ms64();

                // HomePods require a keepalive on RTSP session
                if (keepAlive && now - keepAlive >= 15 * 1000LL) {
                    CSPOT_LOG(debug, "keepAlive %s", name.c_str());
                    raopcl_keepalive(raopClient);
                    keepAlive = now;
                }

                /* We must be sure that we are not in FLUSHED state otherwise the set_progress() will be
                 * ignored and later get_progress() will we incorrect. We could put that in the PCM loop 
                 * after accept_frames() returns  */
                if (trackStatus == TRACK_READY && raopcl_state(raopClient) == RAOP_STREAMING) {
                    CSPOT_LOG(info, "Setting track position %d / %d", startOffset, trackInfo.duration);
                    raopcl_set_progress_ms(raopClient, startOffset, trackInfo.duration);
                    spirc->updatePositionMs(startOffset);
                    trackStatus = TRACK_STREAMING;
                }
    
                // last track has played to the end
                if (trackStatus == TRACK_END && !raopcl_is_playing(raopClient)) {
                    CSPOT_LOG(info, "last track finished");
                    trackStatus = TRACK_INIT;
                    raopcl_disconnect(raopClient);
                    spirc->notifyAudioEnded();
                } 
                
                // new track has reached DAC, this is "delay" after change of identifier
                if (startTime && now >= startTime) {
                    if (notify) spirc->notifyAudioReachedPlayback();
                    notify = true;
                    startTime = 0;
                    keepAlive = now;
                } 
                
                // when paused disconnect the raopcl connection after a while
                if (stopTime && now >= stopTime) {
                    stopTime = 0;
                    raopcl_disconnect(raopClient);
                    CSPOT_LOG(info, "teardown RAOP connection on timeout at %d", startOffset);
                    keepAlive = 0;
                }

                // make sure keep alive is silent when disconnected 
                if (state == DISCO && !zeroConf) {
                    state = LINKED;
                    keepAlive = 0;
                }
            }

            spirc->disconnect();
            spirc.reset();
            CSPOT_LOG(info, "disconnecting player %s", name.c_str());
        } else {
            CSPOT_LOG(error, "failed authentication, forcing ZeroConf");
            if (!zeroConf) enableZeroConf();
            zeroConf = true;
        }
    }

    CSPOT_LOG(info, "terminating player %s", name.c_str());
}

/****************************************************************************************
 * C interface functions
 */

void spotOpen(uint16_t portBase, uint16_t portRange, char *username, char *password) {
    if (!bell::bellGlobalLogger) {
        bell::setDefaultLogger();
        bell::enableTimestampLogging(true);
    }
    CSpotPlayer::portBase = portBase;
    if (portRange) CSpotPlayer::portRange = portRange;
    if (username) CSpotPlayer::username = username;
    if (password) CSpotPlayer::password = password;
}

void spotClose(void) {
}

struct spotPlayer* spotCreatePlayer(char* name, char *id, char *credentials, struct in_addr addr, int oggRate, size_t frameSize, uint32_t delay, struct shadowPlayer* shadow) {
    AudioFormat format = AudioFormat_OGG_VORBIS_160;

    if (oggRate == 320) format = AudioFormat_OGG_VORBIS_320;
    else if (oggRate == 96) format = AudioFormat_OGG_VORBIS_96;

    auto player = new CSpotPlayer(name, id, credentials, addr, format, frameSize, delay, shadow);
    if (player->startTask()) return (struct spotPlayer*) player;

    delete player;
    return NULL;
}

void spotDeletePlayer(struct spotPlayer* spotPlayer) {
    auto player = (CSpotPlayer*) spotPlayer;
    delete player;
}

void spotNotify(struct spotPlayer* spotPlayer, enum shadowEvent event, ...) {
    va_list args;
    va_start(args, event);
    notify((CSpotPlayer*)spotPlayer, event, args);
    va_end(args);
}


