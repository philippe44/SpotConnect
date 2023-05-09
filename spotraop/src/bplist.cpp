/*
 *  bplist - 1-level (no recurse) simplified bplist reader & writer
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow="
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#pragma GCC diagnostic pop

// for htnox and ntohx
#ifdef _WIN32
#include <WinSock2.h>
#define be64toh ntohll
#define htobe64 htonll
#else
#include <arpa/inet.h>
#if defined(sun)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define be64toh(x) __builtin_bswap64(x)
#define htobe64(x) __builtin_bswap64(x)
#else
#define be64toh(x) (x)
#define htobe64(x) (x)
#endif
#elif defined(__FreeBSD__)
#include <sys/endian.h>
#elif defined(__APPLE__)
#define be64toh ntohll
#define htobe64 htonll
#else
#define _BSD_SOURCE
#include <endian.h>
#endif
#endif

#include <bplist.h>

bplist::bplist(void) {
	const char header[] = "bplist00";
	object.insert(object.begin(), header, header + strlen(header));
}

void bplist::init(uint8_t* blob, size_t size) {
	memcpy(&trailer, blob + size - 32, 32);
	uint64_t startOfs = be64toh(trailer.startOfs);

	// copy objects into structure
	object.clear();
	object.insert(object.begin(), blob, blob + startOfs);

	// copy offset
	offset.insert(offset.begin(), blob + startOfs, blob + size - 32);

	// now parse
	size_t pos = 8;
	while (pos < object.size()) {
		uint8_t id = object[pos];

		// look for a dictionary first
		if ((id & 0xf0) != 0xd0) {
			size_t count = readCount(pos);
			pos += count;
			continue;
		}

		// this advances pos to first key offset
		size_t count = readCount(pos);

		// first get all keys
		for (int i = 0; i < count; i++) entries.insert({ readKey(pos), Value {} });
		// the set values
		for (auto& [key, value] : entries) value = readValue(pos);
	}
}

size_t bplist::readCount(size_t &pos) {
	uint8_t id = object[pos++];

	if ((id & 0x0f) != 0x0f) return (id & 0x0f);

	uint8_t bcount = pow(2.0f, object[pos++] & 0x0f);
	size_t count = 0;
	for (int i = 0; i < bcount; i++) count = (count << 8) | object[pos++];

	return count;
}

std::string bplist::readKey(size_t& pos) {
	size_t index = object[pos] * trailer.ofsSize + be64toh(trailer.topOfs), ofs = 0;
	for (int i = 0; i < trailer.ofsSize; i++) ofs = (ofs << 8) | offset[index++];

	if ((object[ofs] & 0xf0) != STRING) printf("Error getting at %zu", ofs);

	// this advances ofs to the first item of the string
	size_t count = readCount(ofs);
	std::string key((char*) object.data() + ofs, count);

	// move to value
	pos++;
	return key;
}

bplist::Value bplist::readValue(size_t& pos) {
	size_t index = object[pos] * trailer.ofsSize + be64toh(trailer.topOfs), ofs = 0;
	for (int i = 0; i < trailer.ofsSize; i++) ofs = (ofs << 8) | offset[index++];

	Value value;
	value.id = (objectId) (object[ofs] & 0xf0);

	// this advances ofs to the first item of the string
	size_t count = readCount(ofs);

	switch (value.id) {
	case STRING:
		value.string.assign((char*)object.data() + ofs, count);
		break;
	case DATA:
		value.data.insert(value.data.begin(), object.data() + ofs, object.data() + ofs + count);
		break;
	case INTEGER:
		value.integer = ntohl(*(uint32_t*) (object.data() + pos));
		break;
	default:
		break;
	}

	// move to next item
	pos++;
	return value;
}

void bplist::insertObject(objectId id, size_t size) {
	if (size < 15) {
		object.push_back(id | (uint8_t)size);	// dictionnary count in objects
	} else {
		object.push_back(id | 0x0f);
		uint8_t blen = log2(size) / 8;
		object.push_back(0x10 | blen);
		for (int i = blen; i >= 0; i--) object.push_back(size >> (8 * i));
	}
}

void bplist::add(std::string key, std::string value) {
	add(1, key.c_str(), STRING, value.c_str());
}

void bplist::add(size_t count, ...) {
	va_list args;
	va_start(args, count);

	// first add the 0xd0 object marker
	ofsIndex.push_back(object.size());
	insertObject(DICTIONARY, count);

	// memorize current offset for later update
	size_t ofs = ofsIndex.size();

	// create offset table indexes for dictonnary are K1..Kn, V1..Vn 
	for (size_t i = 0; i < count * 2; i++) {
		object.push_back(ofsIndex.size());
		// this is a placeholder for now
		ofsIndex.push_back(0);
	}

	// now write the actual Key/Value in object and update offsets (K1..Kn, V1..Vn)
	for (size_t i = 0; i < count; i++) {
		// set Key offset which is end of current object
		ofsIndex[ofs + i] = object.size();

		// process key which is a char*
		auto key = va_arg(args, char*);
		insertObject(STRING, strlen(key));
		object.insert(object.end(), key, key + strlen(key));

		// set Value offset which is the current end of object
		ofsIndex[ofs + count + i] = object.size();

		// then process different values
		objectId id = (objectId) va_arg(args, int);

		switch (id) {
		case STRING: {
			auto item = va_arg(args, char*);
			insertObject(STRING, strlen(item));
			object.insert(object.end(), item, item + strlen(item));
			break;
		}
		case DATA: {
			auto data = va_arg(args, uint8_t*);
			auto size = va_arg(args, size_t);
			insertObject(DATA, size);
			object.insert(object.end(), data, data + size);
			break;
		}
		case INTEGER: {
			auto data = va_arg(args, uint32_t);
			insertObject(INTEGER, sizeof(data));
			object.insert(object.end(), (uint8_t*) &data, (uint8_t*)&data + sizeof(data));
			break;
		}
		default:
			break;
		}
	}

	va_end(args);
}

std::vector<uint8_t> bplist::toData(void) {
	std::vector<uint8_t> data(object);
	 
	trailer.ofsSize = log2(object.size()) / 8 + 1;
	trailer.refSize = 1;
	trailer.startOfs = htobe64(object.size());

	for (auto item : ofsIndex) {
		for (int i = 0; i < trailer.ofsSize; i++) {
			data.push_back(item >> (8 * (trailer.ofsSize - i - 1)));
		}
	}

	trailer.count = htobe64(ofsIndex.size());
	data.insert(data.end(), (uint8_t*)&trailer, (uint8_t*) &trailer + sizeof(trailer));

	/*
	printf("\n\n");
	for (int i = 0; i < data.size(); i++) {
		printf("%02x ", data[i]);
		if (i % 16 == 15) {
			printf("\t");
			for (int j = i-15; j <= i; j++) {
				putc(isalnum(data[j]) ? data[j] : '.', stdout);
			}
			printf("\n");
		}
	}
	printf("\n\n");
	*/

	return data;
}
