#include <string>
#include <streambuf>
#include <memory>
#include <vector>
#include <iostream>
#include <inttypes.h>
#include <fstream>
#include <cassert>
#include <cstdint>
#include <cstring>
#include "Logger.h"
#include "spotify.h"
#include "metadata.h"
#include "time.h"
#ifdef _WIN32
#include "win32shim.h"
#endif

#include "codecs.h"
#include "FLAC/stream_encoder.h"
extern "C" {
#include "layer3.h"
}

/****************************************************************************************
 * Ring buffer
 */

byteBuffer::byteBuffer(FILE* storage, size_t size) {
    buffer = new uint8_t[size];
    this->size = size;
    this->write_p = this->read_p = buffer;
    this->wrap_p = buffer + size; 
    this->storage = storage;
}

byteBuffer::~byteBuffer(void) { 
    std::scoped_lock lock(mutex); 
    delete[] buffer;
    if (storage) fclose(storage);
}

size_t byteBuffer::read(uint8_t* dst, size_t size, size_t min) {
    std::scoped_lock lock(mutex);
    size = std::min(size, _used());
    if (size < min) return 0;

    size_t cont = std::min(size, (size_t) (wrap_p - read_p));
    memcpy(dst, read_p, cont);
    memcpy(dst + cont, buffer, size - cont);

    read_p += size;
    if (read_p >= wrap_p) read_p -= this->size;
    return size;
}

uint8_t* byteBuffer::readInner(size_t &size) {
    // unless there is no enough data, we exit with mutex locked
    mutex.lock();

    // 0 means we want everything (contiguous)
    if (!size) size = this->size;

    // caller *must* consume ALL data
    size = std::min(size, _used());
    size = std::min(size, (size_t)(wrap_p - read_p));

    if (!size) {
        mutex.unlock();
        return NULL;
    }

    uint8_t* r = read_p; 
    read_p += size;
    if (read_p >= wrap_p) read_p -= this->size;
    
    return r;
}

bool byteBuffer::write(const uint8_t* src, size_t size) {
    std::scoped_lock lock(mutex);
    if (size > _space()) return false;

    size_t cont = std::min(size, (size_t)(wrap_p - write_p));
    memcpy(write_p, src, cont);
    memcpy(buffer, src + cont, size - cont);

    if (storage) fwrite(src, size, 1, storage);

    write_p += size;
    if (write_p >= wrap_p) write_p -= this->size;
    return true;
}

#ifdef __GNUC__
#define PACK( __Declaration__ ) __attribute__((__packed__)) __Declaration__ 
#endif

#ifdef _MSC_VER
#define PACK( __Declaration__ ) __pragma( pack(push, 1) ) __Declaration__ __pragma( pack(pop))
#endif

/****************************************************************************************
 * Base codec
 */

uint32_t baseCodec::index = 0;

baseCodec::baseCodec(codecSettings *settings, std::string mimeType, bool store) : settings(settings), mimeType(mimeType) {
    FILE* storage = NULL;
    
    if (store) {
        char type[5] = { 0 };
        (void)!sscanf(mimeType.c_str(), "audio/%4[^;]", type);
        auto name = "./stream-" + std::to_string(index++) + "." + type;
        storage = fopen(name.c_str(), "wb");
    }

    pcm = std::make_shared<byteBuffer>(storage);
    encoded = pcm;
}

size_t baseCodec::read(uint8_t* dst, size_t size, size_t min, bool drain) { 
    size_t bytes = encoded->read(dst, size, min);
    if (!bytes && drain) {
        baseCodec::drain();
        return encoded->read(dst, size, min);
    } else {
        return bytes;
    }
}

uint8_t* baseCodec::readInner(size_t& size, bool drain) { 
    uint8_t * data = encoded->readInner(size);
    if (!data && drain) {
        baseCodec::drain();
        return encoded->readInner(size);
    } else {
        return data;
    }
}

/****************************************************************************************
 * PCM codec
 */

class pcmCodec : public::baseCodec {
private:
    codecSettings settings;

public:
    pcmCodec(codecSettings *settings, bool store = false);
    virtual uint64_t initialize(int64_t duration);
    virtual size_t read(uint8_t* dst, size_t size, size_t min, bool drain);
    virtual uint8_t* readInner(size_t& size);
};

pcmCodec::pcmCodec(codecSettings *settings, bool store) : settings(*settings), 
                   baseCodec(&this->settings, "audio/L16;rate=44100;channels=2", store) {
    mimeType = "audio/L" + std::to_string(settings->size * 8) + ";rate=" + std::to_string(settings->rate) +
               ";channels=" + std::to_string(settings->channels);
}

uint8_t* pcmCodec::readInner(size_t& size) {
    uint8_t* data = pcm->readInner(size);

    // will only locked if not empty
    if (!size) return NULL;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // pcm needs byte swapping on little endian CPU
    uint16_t* swap = (uint16_t*)data;
    for (size_t i = size / 2; i; i--) {
#ifdef _WIN32
        * swap++ = _byteswap_ushort(*swap);
#else
        * swap++ = __builtin_bswap16(*swap);
#endif
    }
#endif

    return data;
}

size_t pcmCodec::read(uint8_t* dst, size_t size, size_t min, bool drain) {
    size_t bytes = encoded->read(dst, size, min);
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // pcm needs byte swapping on little endian CPU
    uint16_t* swap = (uint16_t*) dst;
    for (int i = bytes /= 2; i; i--) {
#ifdef _WIN32
        * swap++ = _byteswap_ushort(*swap);
#else
        * swap++ = __builtin_bswap16(*swap);
#endif
    }
#endif
    return bytes;
}

uint64_t pcmCodec::initialize(int64_t duration) {
    return (((uint64_t) settings.rate * settings.channels * settings.size * duration) / 1000) & ~1LL;
}

/****************************************************************************************
 * WAV codec
 */

class wavCodec : public::baseCodec {
private:
    codecSettings settings;
    size_t position = 0;

public:
    wavCodec(codecSettings *settings, bool store = false) : settings(*settings), baseCodec(&this->settings, "audio/wav", store) {  }
    virtual uint64_t initialize(int64_t duration);
};

uint64_t wavCodec::initialize(int64_t duration) {
    struct PACK(header {
        uint8_t	 chunkId[4];
        uint32_t chunkSize;
        uint8_t	 format[4];
        uint8_t	 subchunk1Id[4];
        uint32_t subchunk1Size;
        uint16_t audioFormat;
        uint16_t channels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
        uint8_t	 subchunk2Id[4];
        uint32_t subchunk2Size;
    } header) = {
            { 'R','I','F','F' },
            UINT32_MAX,
            { 'W','A','V','E' },
            { 'f','m','t',' ' },
            16, 1, 2,
            UINT32_MAX,
            UINT32_MAX,
            4, 16,
            { 'd','a','t','a' },
            UINT32_MAX,
    };

    // get the maximum payload size
    uint64_t length = duration ? ((uint64_t) settings.rate * settings.channels * settings.size * duration) / 1000 : UINT32_MAX;
    length = std::min(length, (uint64_t) (UINT32_MAX - offsetof(struct header, subchunk2Id)));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    header.subchunk2Size = length;
    header.sampleRate = 44100;
    header.byteRate = 44100 * 2 * 2;
    header.chunkSize = offsetof(struct header, subchunk2Id) + length;
#else
    header.subchunk2Size = __builtin_bswap32(length);
    header.sampleRate = __builtin_bswap32(44100);;
    header.byteRate = __builtin_bswap32(44100 * 2 * 2);
    header.chunkSize = __builtin_bswap32(offsetof(struct header, subchunk2Id) + length);
#endif

    // write header in the encoded buffer
    encoded->write((uint8_t*) &header, sizeof(header));

    return length + sizeof(header);
}

/****************************************************************************************
 * FLAC codec
 */

class flacCodec : public::baseCodec {
private:
    flacSettings settings;
    void* flac = NULL;
    int level;
    bool drained = false;

public:
    flacCodec(flacSettings *settings, bool store = false) : settings(*settings), baseCodec(&this->settings, "audio/flac", store), level(level) { }
    virtual ~flacCodec(void);
    virtual int getBitrate(void) { return baseCodec::getBitrate() * 0.7; }
    virtual uint64_t initialize(int64_t duration);
    virtual bool pcmWrite(const uint8_t* data, size_t size);
    virtual void drain(void);
};

flacCodec::~flacCodec(void) {
    if (flac) FLAC__stream_encoder_delete((FLAC__StreamEncoder*)flac);
}

uint64_t flacCodec::initialize(int64_t duration) {
    // clean any current decoder 
    if (flac) FLAC__stream_encoder_delete((FLAC__StreamEncoder*)flac);
    drained = false;

    auto flacWrite = [](const FLAC__StreamEncoder* encoder, const FLAC__byte buffer[],
        size_t bytes, unsigned samples, unsigned current_frame, void* client_data) {
            if (((flacCodec*)client_data)->encoded->write(buffer, bytes)) return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
            else return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    };

    FLAC__StreamEncoder* codec = FLAC__stream_encoder_new();
    flac = codec;

    FLAC__bool ok = FLAC__stream_encoder_set_verify(codec, false);
    ok &= FLAC__stream_encoder_set_compression_level(codec, level);
    ok &= FLAC__stream_encoder_set_channels(codec, settings.channels);
    ok &= FLAC__stream_encoder_set_bits_per_sample(codec, settings.size * 8);
    ok &= FLAC__stream_encoder_set_sample_rate(codec, settings.rate);
    ok &= FLAC__stream_encoder_set_blocksize(codec, 1024);
    ok &= FLAC__stream_encoder_set_streamable_subset(codec, true);
    ok &= !FLAC__stream_encoder_init_stream(codec, flacWrite, NULL, NULL, NULL, this);

    if (!ok) {
        CSPOT_LOG(error, "Cannot set FLAC parameters");
    }

    return 0;
}

bool flacCodec::pcmWrite(const uint8_t* data, size_t len) {
    if (encoded->space() < std::max(len * 2, (size_t)16384)) return false;
    //assert((size & 0x03) != 0);

    auto flacSamples = new FLAC__int32[len];
    for (size_t i = 0; i < len / 2; i++, data += 2) flacSamples[i] = *(int16_t*)data;
    FLAC__stream_encoder_process_interleaved((FLAC__StreamEncoder*)flac, flacSamples, len / 4);
    delete[] flacSamples;

    return true;
}

void flacCodec::drain(void) {
    if (drained) return;
    FLAC__stream_encoder_finish((FLAC__StreamEncoder*)flac);
    drained = true;
}


/****************************************************************************************
 * MP3 codec
 */

class mp3Codec : public::baseCodec {
private:
    void* mp3 = NULL;
    bool drained = false;
    int blockSize;
    mp3Settings settings;
    int16_t* scratch;

    void process(void);

public:
    mp3Codec(mp3Settings *settings, bool store = false);
    virtual ~mp3Codec(void);
    virtual int getBitrate(void) { return settings.bitrate * 1000; }
    virtual uint64_t initialize(int64_t duration);
    virtual size_t read(uint8_t* dst, size_t size, size_t min = 0, bool drain = false);
    virtual uint8_t* readInner(size_t& size, bool drain = false);
    virtual void drain(void);
};

mp3Codec::mp3Codec(mp3Settings *settings, bool store) : settings(*settings), baseCodec(&this->settings, "audio/mpeg", store) {
    pcm.reset();
    pcm = std::make_shared<byteBuffer>();
}

mp3Codec::~mp3Codec(void) {
    if (mp3) shine_close((shine_t) mp3);
    delete scratch;
}

uint64_t mp3Codec::initialize(int64_t duration) {
    struct PACK({
        uint8_t	 id[3];
        uint8_t  version[2];
        uint8_t	 flags;
        uint8_t  size[4];
        uint8_t  tit[3];
        uint8_t  titSize[3];
        char     title[32];
    } header) = {
            { 'I','D','3' },
            { 3, 0 },
            0, 
            { 0, 0, 0, 38 },
            { 'T','I','T' },
            { 0, 0, 32 },
            "SpotConnect",
    };

    // write header with no modification, just so that player thinks it's a file
    // @TODO: why does this not work?
    encoded->write((uint8_t*)&header, sizeof(header));

    // clean any current decoder 
    if (mp3) shine_close((shine_t)mp3);
    drained = false;

    // create a new encoder    
    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);

    config.wave.samplerate = settings.rate;
    config.wave.channels = PCM_STEREO;
    config.mpeg.bitr = settings.bitrate;
    config.mpeg.mode = STEREO;
    mp3 = shine_initialise(&config);

    blockSize = shine_samples_per_pass((shine_t)mp3) * 4;
    scratch = new int16_t[blockSize];

    return 0;
}

void mp3Codec::process(void) {
    auto space = std::max(blockSize, 16384);
    while (encoded->space() >= space && pcm->used() > blockSize) {
        int len;
        pcm->read((uint8_t*)scratch, blockSize);
        uint8_t* coded = shine_encode_buffer_interleaved((shine_t)mp3, scratch, &len);
        encoded->write(coded, len);
    }
}

size_t mp3Codec::read(uint8_t* dst, size_t size, size_t min, bool drain) { 
    process();
    return baseCodec::read(dst, size, min, drain); 
}

uint8_t* mp3Codec::readInner(size_t& size, bool drain) { 
    process();
    return baseCodec::readInner(size, drain); 
}

void mp3Codec::drain(void) {
    if (drained || encoded->space() < std::max(blockSize, 16384)) return;
    int len;
    uint8_t* coded = shine_flush((shine_t) mp3, &len);
    encoded->write(coded, len);
    drained = true;
}

/****************************************************************************************
 * Interface that will figure out which derived class to create
 */

std::unique_ptr<baseCodec> createCodec(codecSettings::type type, codecSettings* settings, bool store) {
    codecSettings defaults;
    if (!settings) settings = &defaults;

    switch (type) {
    case codecSettings::PCM:
        return std::make_unique<pcmCodec>(settings, store);
        break;
    case codecSettings::WAV:
        return std::make_unique<wavCodec>(settings, store);
        break;
    case codecSettings::FLAC:
        return std::make_unique<flacCodec>((flacSettings*)settings, store);
        break;
    case codecSettings::MP3:
        return std::make_unique<mp3Codec>((mp3Settings*)settings, store);
        break;
    default:
        return nullptr;
    }
}

