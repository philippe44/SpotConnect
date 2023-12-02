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
#include "opusenc.h"
#include "vorbis/vorbisfile.h"
#include "vorbis/vorbisenc.h"
#include "faac.h"
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
size_t baseCodec::minSpace = 16384;

baseCodec::baseCodec(codecSettings settings, std::string mimeType, bool store) : settings(settings), mimeType(mimeType) {
    FILE* storage = NULL;

    // that's all we have for now
    if (settings.size != 2 || settings.channels != 2) throw std::out_of_range("codec only accepts stereo 16 bits samples");
 
    if (store) {
        auto name = "./stream-" + std::to_string(index++) + "." + id();
        storage = fopen(name.c_str(), "wb");
    }

    pcmBitrate = settings.rate * settings.channels * settings.size * 8;
    pcm = std::make_shared<byteBuffer>(storage);
    encoded = pcm;
}

size_t baseCodec::read(uint8_t* dst, size_t size, size_t min, bool drain) { 
    // we want to encode more than required but not too much to leave some CPU
    process(size * 2);
    size_t bytes = encoded->read(dst, size, min);

    if (!bytes && drain) {
        baseCodec::drain();
        return encoded->read(dst, size, min);
    } else {
        return bytes;
    }
}

uint8_t* baseCodec::readInner(size_t& size, bool drain) { 
    // we want to encode more than required but not too much to leave some CPU
    process(size * 2);
    uint8_t * data = encoded->readInner(size);

    if (!data && drain) {
        baseCodec::drain();
        return encoded->readInner(size);
    } else {
        return data;
    }
}

std::string baseCodec::id(void) {
    auto search = std::string("audio/");
    size_t pos = mimeType.find(search);
    if (pos != std::string::npos) return mimeType.substr(pos + search.size());
    return std::string();
}

/****************************************************************************************
 * PCM codec
 */

class pcmCodec : public::baseCodec {
public:
    pcmCodec(codecSettings settings, bool store = false);
    virtual int64_t initialize(int64_t duration) { return duration ? (((int64_t)pcmBitrate * duration) / (8 * 1000)) & ~1LL : -INT64_MAX; }
    virtual size_t read(uint8_t* dst, size_t size, size_t min, bool drain);
    virtual uint8_t* readInner(size_t& size, bool drain);
};

pcmCodec::pcmCodec(codecSettings settings, bool store) :
                   baseCodec(settings, "audio/L16;rate=44100;channels=2", store) {
    mimeType = "audio/L" + std::to_string(settings.size * 8) + ";rate=" + std::to_string(settings.rate) +
               ";channels=" + std::to_string(settings.channels);
}

uint8_t* pcmCodec::readInner(size_t& size, bool drain) {
    uint8_t* data = pcm->readInner(size);

    // will only locked if not empty
    if (!size) return NULL;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    // pcm needs byte swapping on little endian CPU
    uint16_t* swap = (uint16_t*)data;
    for (size_t i = size / settings.size; i; i--) {
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
    for (int i = bytes / settings.size; i; i--) {
#ifdef _WIN32
        * swap++ = _byteswap_ushort(*swap);
#else
        * swap++ = __builtin_bswap16(*swap);
#endif
    }
#endif
    return bytes;
}

/****************************************************************************************
 * WAV codec
 */

class wavCodec : public::baseCodec {
private:
    size_t position = 0;

public:
    wavCodec(codecSettings settings, bool store = false) : baseCodec(settings, "audio/wav", store) { }
    virtual int64_t initialize(int64_t duration);
};

int64_t wavCodec::initialize(int64_t duration) {
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
            16, 1, settings.channels,
            settings.rate,
            settings.rate * settings.channels* settings.size,
            4, (uint16_t) (settings.size * 8),
            { 'd','a','t','a' },
            UINT32_MAX,
    };

    // get the maximum payload size
    int64_t length = duration ? ((int64_t) pcmBitrate * duration) / (8*1000) : UINT32_MAX;
    length = std::min(length, (int64_t) (UINT32_MAX - offsetof(struct header, subchunk2Id)));

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    header.subchunk2Size = length;
    header.chunkSize = offsetof(struct header, subchunk2Id) + length;
#else
    header.subchunk2Size = __builtin_bswap32(length);
    header.chunkSize = __builtin_bswap32(offsetof(struct header, subchunk2Id) + length);
    headers.channels = __buils_bswap16(settings.channels);
    header.sampleRate = __builtin_bswap32(settings.rate);;
    header.byteRate = __builtin_bswap32(settings.rate * settings.channels * settings.size);
    header.bitsPerSample = __builtin_bswap16(settings.size * 8);
#endif

    // write header in the encoded buffer
    encoded->write((uint8_t*) &header, sizeof(header));

    return (length + sizeof(header)) * (duration ? 1 : -1);
}

/****************************************************************************************
 * FLAC codec
 */

class flacCodec : public::baseCodec {
private:
    FLAC__StreamEncoder* flac = NULL;
    bool drained = false;

public:
    flacCodec(codecSettings settings, bool store = false) : baseCodec(settings, "audio/flac", store) { }
    virtual ~flacCodec(void);
    virtual int64_t initialize(int64_t duration);
    virtual bool pcmWrite(const uint8_t* data, size_t size);
    virtual void drain(void);
};

flacCodec::~flacCodec(void) {
    if (flac) FLAC__stream_encoder_delete((FLAC__StreamEncoder*)flac);
}

int64_t flacCodec::initialize(int64_t duration) {
    // clean any current decoder 
    if (flac) FLAC__stream_encoder_delete((FLAC__StreamEncoder*)flac);
    drained = false;

    auto flacWrite = [](const FLAC__StreamEncoder* encoder, const FLAC__byte buffer[],
        size_t bytes, unsigned samples, unsigned current_frame, void* client_data) {
            if (((flacCodec*)client_data)->encoded->write(buffer, bytes)) return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
            else return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
    };

    flac = FLAC__stream_encoder_new();

    FLAC__bool ok = FLAC__stream_encoder_set_verify(flac, false);
    ok &= FLAC__stream_encoder_set_compression_level(flac, settings.flac.level);
    ok &= FLAC__stream_encoder_set_channels(flac, settings.channels);
    ok &= FLAC__stream_encoder_set_bits_per_sample(flac, settings.size * 8);
    ok &= FLAC__stream_encoder_set_sample_rate(flac, settings.rate);
    ok &= FLAC__stream_encoder_set_blocksize(flac, 0);
    ok &= FLAC__stream_encoder_set_streamable_subset(flac, true);
    ok &= !FLAC__stream_encoder_init_stream(flac, flacWrite, NULL, NULL, NULL, this);

    if (!ok) throw std::runtime_error("Cannot set FLAC parameters");

    return -(duration ? ((int64_t) pcmBitrate * duration * 0.7) / (8 * 1000) : INT64_MAX);
}

bool flacCodec::pcmWrite(const uint8_t* data, size_t len) {
    if (encoded->space() < std::max(len * 2, minSpace)) return false;
    //assert((size & 0x03) != 0);

    auto flacSamples = new FLAC__int32[len];
    for (size_t i = 0; i < len / settings.size; i++, data += settings.size) flacSamples[i] = *(int16_t*)data;
    FLAC__stream_encoder_process_interleaved((FLAC__StreamEncoder*)flac, flacSamples, len / (settings.size * settings.channels));
    delete[] flacSamples;

    return true;
}

void flacCodec::drain(void) {
    if (drained) return;
    FLAC__stream_encoder_finish((FLAC__StreamEncoder*)flac);
    drained = true;
}

/****************************************************************************************
 * AAC codec
 */

class aacCodec : public::baseCodec {
private:
    faacEncHandle aac = NULL;
    unsigned long inSamples = 0, outMaxBytes = 0;
    bool drained = false;
    uint8_t* inBuf = NULL, * outBuf = NULL;

    void process(size_t bytes);
    void cleanup(void);

public:
    aacCodec(codecSettings settings, bool store = false);
    virtual ~aacCodec(void) { cleanup(); }
    virtual int64_t initialize(int64_t duration);
    virtual void drain(void);
};

aacCodec::aacCodec(codecSettings settings, bool store) : baseCodec(settings, "audio/aac", false) {
    pcm.reset();
    pcm = std::make_shared<byteBuffer>();
}

void aacCodec::cleanup(void) {
    if (aac) {
        faacEncClose(aac);
        delete[] inBuf;
        delete[] outBuf;
    }
}

int64_t aacCodec::initialize(int64_t duration) {
    // clean any current decoder 
    cleanup();
    drained = false;

    // in case of failure, return 0
    aac = faacEncOpen(settings.rate, settings.channels, &inSamples, &outMaxBytes);    
    if (!aac) return 0;

    // inSamples is the *total* number of samples, not of frames...
    inBuf = new uint8_t[inSamples * settings.size];
    outBuf = new uint8_t[outMaxBytes];

    faacEncConfigurationPtr format = faacEncGetCurrentConfiguration(aac);
    format->bitRate = settings.aac.bitrate * 1000 / settings.channels;
    format->mpegVersion = MPEG4;
    format->bandWidth = 0;
    format->outputFormat = ADTS_STREAM;
    format->inputFormat = FAAC_INPUT_16BIT;
    faacEncSetConfiguration(aac, format);
    
    return -(duration ? ((int64_t)settings.aac.bitrate * duration) / 8 : INT64_MAX);
}

void aacCodec::process(size_t bytes) {
    size_t blockSize = inSamples * settings.size;
    while (encoded->space() >= outMaxBytes && pcm->used() >= blockSize && (ssize_t)bytes > 0) {
        pcm->read(inBuf, blockSize);
        int len = faacEncEncode(aac, (int32_t*) inBuf, inSamples, outBuf, outMaxBytes);
        encoded->write(outBuf, len);
        bytes -= len;
    }
}

void aacCodec::drain(void) {
    if (drained || encoded->space() < outMaxBytes) return;
    int len = faacEncEncode(aac, NULL, 0, outBuf, outMaxBytes);
    encoded->write(outBuf, len);
    drained = true;
}

/****************************************************************************************
 * MP3 codec
 */

class mp3Codec : public::baseCodec {
private:
    shine_t mp3 = NULL;
    bool drained = false;
    size_t blockSize;
    int16_t* scratch;

    void process(size_t bytes);
    void cleanup();

public:
    mp3Codec(codecSettings settings, bool store = false);
    virtual ~mp3Codec(void) { cleanup(); }
    virtual int64_t initialize(int64_t duration);
    virtual void drain(void);
    virtual std::string id() { return std::string("mp3"); }
};

mp3Codec::mp3Codec(codecSettings settings, bool store) : baseCodec(settings, "audio/mpeg", store) {
    pcm.reset();
    pcm = std::make_shared<byteBuffer>();
}

void mp3Codec::cleanup(void) {
    if (mp3) {
        shine_close(mp3);
        delete[] scratch;
    }
}

int64_t mp3Codec::initialize(int64_t duration) {
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

    // clean any current decoder 
    cleanup();
    drained = false;

    // write header with no modification, just so that player thinks it's a file
    encoded->write((uint8_t*)&header, sizeof(header));

    // create a new encoder    
    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);

    config.wave.samplerate = settings.rate;
    config.wave.channels = PCM_STEREO;
    config.mpeg.bitr = settings.mp3.bitrate;
    config.mpeg.mode = STEREO;
    mp3 = shine_initialise(&config);

    // shine_samples_per_pass is the number of samples BUT per channel
    blockSize = shine_samples_per_pass(mp3) * settings.channels;
    scratch = new int16_t[blockSize];
    blockSize *= settings.size;

    return -(duration ? ((int64_t)settings.mp3.bitrate * duration) / 8 : INT64_MAX);
}

void mp3Codec::process(size_t bytes) {
    auto space = std::max(blockSize, minSpace);
    int len;
    while (encoded->space() >= space && pcm->used() >= blockSize && (ssize_t) bytes > 0) {
        pcm->read((uint8_t*)scratch, blockSize);
        uint8_t* coded = shine_encode_buffer_interleaved(mp3, scratch, &len);
        encoded->write(coded, len);
        bytes -= len;
    }
}

void mp3Codec::drain(void) {
    if (drained || encoded->space() < std::max(blockSize, minSpace)) return;
    int len;
    uint8_t* coded = shine_flush(mp3, &len);
    encoded->write(coded, len);
    drained = true;
}

/****************************************************************************************
 * OPUS codec
 */

class opusCodec : public::baseCodec {
private:
    OggOpusEnc* opus = NULL;
    bool drained = false;
    
public:
    opusCodec(codecSettings settings, bool store = false) : baseCodec(settings, "audio/ogg;codecs=opus", store) { }
    virtual ~opusCodec(void);
    virtual int64_t initialize(int64_t duration);
    virtual bool pcmWrite(const uint8_t* data, size_t size);
    virtual void drain(void);
    virtual std::string id() { return std::string("ops"); }
};

opusCodec::~opusCodec(void) {
    if (opus) ope_encoder_destroy(opus);
}

int64_t opusCodec::initialize(int64_t duration) {  
    // clean any current decoder 
    if (opus) ope_encoder_destroy(opus);
    drained = false;

    OpusEncCallbacks callbacks = {
        .write = [](void* user_data, const unsigned char* ptr, opus_int32 len) {
                    return ((opusCodec*)user_data)->encoded->write(ptr, len) ? 0 : 1;
        }, 
        .close = [](void* user_data) {
                    return 0;
        }
    };

    OggOpusComments* comments = ope_comments_create();
    opus = ope_encoder_create_callbacks(&callbacks, this, comments, settings.rate, settings.channels, 1, NULL);
    ope_comments_destroy(comments);

    // in case of failure, return 0
    if (!opus) return 0;

    int bitrate = settings.opus.bitrate * 1000;
    if (bitrate) ope_encoder_ctl(opus, OPUS_SET_BITRATE(bitrate));
    else ope_encoder_ctl(opus, OPUS_GET_BITRATE(&bitrate));
   
    return -(duration ? ((int64_t)bitrate * duration) / 8 : INT64_MAX);
}

bool opusCodec::pcmWrite(const uint8_t * data, size_t len) {
    // we do not block (at least it should not happen)
    if (encoded->space() < std::max(len * 2, minSpace)) return false;
    return ope_encoder_write(opus, (opus_int16*)data, len / (settings.channels * settings.size)) == 0;
}

void opusCodec::drain(void) {
    if (drained || encoded->space() < minSpace) return;
    ope_encoder_drain(opus);
    drained = true;
}

/****************************************************************************************
 * VORBIS codec
 */

class vorbisCodec : public::baseCodec {
private:
    vorbis_info info;
    vorbis_block block;
    vorbis_dsp_state dsp;
    bool initialized = false;
    
    ogg_stream_state stream;

    bool drained = false;

    void process(size_t bytes);
    void cleanup(void);

public:
    vorbisCodec(codecSettings settings, bool store = false);
    virtual ~vorbisCodec(void) { cleanup(); }
    virtual int64_t initialize(int64_t duration);
    virtual void drain(void);
    virtual std::string id() { return std::string("oga"); }
};

vorbisCodec::vorbisCodec(codecSettings settings, bool store) : baseCodec(settings, "audio/ogg;codecs=vorbis", store) {
    pcm.reset();
    pcm = std::make_shared<byteBuffer>();
}

void vorbisCodec::cleanup(void) {
    if (initialized) {
        vorbis_info_clear(&info);
        vorbis_dsp_clear(&dsp);
        vorbis_block_clear(&block);
        ogg_stream_clear(&stream);
    }
}

int64_t vorbisCodec::initialize(int64_t duration) {
    // clean any current decoder 
    cleanup();
    drained = false;

    // initialize vorbis codec
    vorbis_info_init(&info);
    long bitrate = settings.vorbis.bitrate ? settings.vorbis.bitrate * 1000 : 160 * 1000;

    //  assume that only this part can go wrong
    if (vorbis_encode_init(&info, settings.channels, settings.rate, bitrate, bitrate * 1.25, bitrate * 0.75)) {
        vorbis_info_clear(&info);
        return 0;
    }

    initialized = true;
    vorbis_comment comments;
    vorbis_analysis_init(&dsp, &info);
    vorbis_comment_init(&comments);

    // initialize ogg container
    ogg_stream_init(&stream, rand());

    // build headers,put them in pages and write them
    ogg_packet packets[3];
    vorbis_analysis_headerout(&dsp, &comments, packets, packets + 1, packets + 2);
    vorbis_comment_clear(&comments);
    for (size_t i = 0; i < 3; i++) {
        ogg_page page;
        ogg_stream_packetin(&stream, packets + i);
        ogg_stream_pageout(&stream, &page);
        encoded->write(page.header, page.header_len);
        encoded->write(page.body, page.body_len);
    }

    // finally initialize a block structure (once is enough)
    vorbis_block_init(&dsp, &block);

    return -(duration ? ((int64_t)settings.vorbis.bitrate * duration) / 8 : INT64_MAX);
}

void vorbisCodec::process(size_t bytes) {
    while (encoded->space() >= minSpace && pcm->used() > 1024 * settings.channels * settings.size && (ssize_t)bytes > 0) {
        size_t len = 1024 * settings.channels * settings.size;
        // we are always aligned on settings.channels * settings.size;
        int16_t *data = (int16_t*) pcm->readInner(len);
        len /= settings.channels * settings.size;

        float** buffer = vorbis_analysis_buffer(&dsp, len);
        for (size_t i = 0; i < len; i++) {
            buffer[0][i] = *data++ / (float) INT16_MAX;
            buffer[1][i] = *data++ / (float) INT16_MAX;
        }
        pcm->unlock();
        vorbis_analysis_wrote(&dsp, len);

        // encode as many blocks as possible
        while (vorbis_analysis_blockout(&dsp, &block)) {
            // build one packet and submit it to the serializer
            ogg_packet packet;
            
            vorbis_analysis(&block, NULL);
            vorbis_bitrate_addblock(&block);

            while (vorbis_bitrate_flushpacket(&dsp, &packet)) {
                ogg_page page;
                ogg_stream_packetin(&stream, &packet);

                // get as many pages as possible (we assume we won't write more than space here...)
                while (ogg_stream_pageout(&stream, &page)) {
                    encoded->write(page.header, page.header_len);
                    encoded->write(page.body, page.body_len);
                    // don't need to be exact on written bytes
                    bytes -= page.header_len + page.body_len;
                }
            }
        }
    }
}

void vorbisCodec::drain(void) {
    if (drained || encoded->space() < minSpace) return;

    ogg_page page;

    if (ogg_stream_flush(&stream, &page)) {
        encoded->write(page.header, page.header_len);
        encoded->write(page.body, page.body_len);
    }

    drained = true;
}



/****************************************************************************************
 * Interface that will figure out which derived class to create
 */

std::unique_ptr<baseCodec> createCodec(codecSettings::type codec, codecSettings settings, bool store) {
    switch (codec) {
    case codecSettings::PCM: return std::make_unique<pcmCodec>(settings, store);
    case codecSettings::WAV: return std::make_unique<wavCodec>(settings, store);
    case codecSettings::FLAC: return std::make_unique<flacCodec>(settings, store);
    case codecSettings::OPUS: return std::make_unique<opusCodec>(settings, store);
    case codecSettings::VORBIS: return std::make_unique<vorbisCodec>(settings, store);
    case codecSettings::MP3: return std::make_unique<mp3Codec>(settings, store);
    case codecSettings::AAC: return std::make_unique<aacCodec>(settings, store);
    default: return nullptr;
    }
}
