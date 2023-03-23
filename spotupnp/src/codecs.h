#pragma once

#include <vector>
#include <inttypes.h>
#include <mutex>

/****************************************************************************************
 * Ring buffer
 */
class byteBuffer {
private:
    uint8_t* buffer;
    uint8_t* read_p, * write_p, * wrap_p;
    size_t size;
    std::mutex mutex;
    FILE* storage;

    size_t _space(void) { return size - _used() - 1; }
    size_t _used(void) { return write_p >= read_p ? write_p - read_p : size - (read_p - write_p); }

public:
    byteBuffer(FILE* storage = NULL, size_t size = 1024 * 1024);
    ~byteBuffer(void);
    size_t read(uint8_t* dst, size_t max, size_t min = 0);
    uint8_t* readInner(size_t& size);
    bool write(const uint8_t* src, size_t size);
    size_t space(void) { std::scoped_lock lock(mutex); return _space(); }
    size_t used(void) { std::scoped_lock lock(mutex); return _used(); }
    void flush(void) { std::scoped_lock lock(mutex); read_p = write_p = buffer; }
    void lock(void) { mutex.lock(); }
    void unlock(void) { mutex.unlock(); }
};

/* 
 Note that the whole implementation assumes that every buffer of samples contains 
 a set of full frames (i.e. a multiply of 16 bits L+R = 4 bytes
 */
class baseCodec {
private:
    static uint32_t index;

protected:
    std::shared_ptr<byteBuffer> pcm, encoded;
    int total = 0;

public:
    std::string mimeType;

    baseCodec(std::string mimeType, bool store = false);
    virtual ~baseCodec(void) { };
    virtual bool pcmWrite(const uint8_t* data, size_t size) { return pcm->write(data, size); }
    void unlock(void) { encoded->unlock(); }
    bool isEmpty(void) { return encoded->used(); }
    virtual void flush(void) { total = 0;  pcm->flush(); encoded->flush(); }
    virtual uint64_t initialize(int64_t duration = 0) { return 0; }
    virtual size_t read(uint8_t* dst, size_t size, size_t min = 0) { return encoded->read(dst, size, min); }
    virtual uint8_t* readInner(size_t& size) { return encoded->readInner(size); }
    virtual void drain(void) { }
};

class pcmCodec : public::baseCodec {
public:
    pcmCodec(void) : baseCodec("audio/L16;rate=44100;channels=2") { }
    virtual uint64_t initialize(int64_t duration);
    virtual size_t read(uint8_t* dst, size_t size, size_t min);
    virtual uint8_t* readInner(size_t& size);
};

class wavCodec : public::baseCodec {
private:
    size_t position = 0;

public:
    wavCodec(void) : baseCodec("audio/wav") { /*header.reserve(128);*/ }
    virtual uint64_t initialize(int64_t duration);
};

class flacCodec : public::baseCodec {
private:
    void* flac = NULL;
    int level;
    bool drained = false;

public:
    flacCodec(int level = 5) : baseCodec("audio/flac"), level(level) { }
    virtual ~flacCodec(void);
    virtual uint64_t initialize(int64_t duration);
    virtual bool pcmWrite(const uint8_t* data, size_t size);
    virtual void drain(void);
 };

class mp3Codec : public::baseCodec {
private:
    void* mp3 = NULL;
    bool drained = false;
    int blockSize, rate;
    int16_t* scratch;

public:
    mp3Codec(int rate = 160);
    virtual ~mp3Codec(void);
    virtual uint64_t initialize(int64_t duration);
    virtual bool pcmWrite(const uint8_t* data, size_t size);
    virtual void drain(void);
};
