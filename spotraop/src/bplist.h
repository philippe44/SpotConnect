/*
 *  bplist - 1-level (no recurse) simplified bplist reader & writer
 *
 *  (c) Philippe, philippe_44@outlook.com
 *
 *  See LICENSE
 *
 */
 
#pragma once

#include <string>
#include <map>
#include <vector>

class bplist {
public:
	typedef enum { DICTIONARY = 0xd0,  STRING = 0x50, INTEGER = 0x10, ARRAY = 0xa0, DATA = 0x40 } objectId;

private:
	typedef struct {
		objectId id;
		// don't want to bother with a variant and can't be an union
		uint32_t integer;
		std::string string;
		std::vector<uint8_t> data;
	} Value;

	struct {
		uint8_t unused[5];
		uint8_t sort;
		uint8_t ofsSize;
		uint8_t refSize;
		uint64_t count;
		uint64_t topOfs;
		uint64_t startOfs;
	} trailer = { };

	std::vector<size_t> offset, ofsIndex;
	std::vector<uint8_t> object;
	std::map<std::string, Value> entries;

	void insertObject(objectId id, size_t size);
	std::string readKey(size_t& pos);
	size_t readCount(size_t& pos);
	Value readValue(size_t& pos);
	void init(uint8_t* blob, size_t size);
	
public:
	bplist();
	bplist(std::vector<uint8_t> blob) {	init(blob.data(), blob.size()); }
	bplist(uint8_t* blob, size_t size) { init(blob, size); }
	void add(size_t count, ...);
	void add(std::string key, std::string value);
	std::vector<uint8_t> toData(void);
	std::string getValueString(std::string key) { return entries[key].string; }
	std::vector<uint8_t> getValueData(std::string key) { return entries[key].data; }
};
