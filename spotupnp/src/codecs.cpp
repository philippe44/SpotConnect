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

baseCodec::baseCodec(std::string mimeType, bool store) : mimeType(mimeType) {
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


/****************************************************************************************
 * PCM codec
 */

void pcmCodec::pcmParam(uint32_t rate, uint8_t channels, uint8_t size) {
    baseCodec:pcmParam(rate, channels, size);
    mimeType = "audio/L" + std::to_string(size * 8) + ";rate=" + std::to_string(rate) +
        ";channels=" + std::to_string(channels);
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

size_t pcmCodec::read(uint8_t* dst, size_t size, size_t min) {
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
    return (((uint64_t) rate * channels * size * duration) / 1000) & ~1LL;
}

/****************************************************************************************
 * WAV codec
 */

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
    uint64_t length = duration ? ((uint64_t) rate * channels * size * duration) / 1000 : UINT32_MAX;
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
    ok &= FLAC__stream_encoder_set_channels(codec, channels);
    ok &= FLAC__stream_encoder_set_bits_per_sample(codec, size * 8);
    ok &= FLAC__stream_encoder_set_sample_rate(codec, rate);
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

mp3Codec::mp3Codec(int bitrate, bool store) : baseCodec("audio/mpeg", store), bitrate(bitrate) {
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

    config.wave.samplerate = rate;
    config.wave.channels = PCM_STEREO;
    config.mpeg.bitr = bitrate;
    config.mpeg.mode = STEREO;
    mp3 = shine_initialise(&config);

    blockSize = shine_samples_per_pass((shine_t)mp3) * 4;
    scratch = new int16_t[blockSize];

    return 0;
}

bool mp3Codec::pcmWrite(const uint8_t* data, size_t size) {
    bool done = pcm->write(data, size);
    auto space = std::max(blockSize, 16384);

    while (encoded->space() >= space && pcm->used() > blockSize) {
        int len;
        pcm->read((uint8_t*) scratch, blockSize);
        uint8_t* coded = shine_encode_buffer_interleaved((shine_t) mp3, scratch, &len);
        encoded->write(coded, len);
    }
    
    return done;
}

void mp3Codec::drain(void) {
    if (drained || encoded->space() < std::max(blockSize, 16384)) return;
    int len;
    uint8_t* coded = shine_flush((shine_t) mp3, &len);
    encoded->write(coded, len);
    drained = true;
}
