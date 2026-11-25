#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace peare {
namespace fs {

struct TibMacInspection {
    std::uint32_t blockSize;
    std::uint64_t recordOffset;
    std::uint32_t companionRecordSize;
    std::uint32_t trailerSize;
    std::uint32_t firstChunkSize;
    std::uint32_t firstBoxSize;
    std::uint16_t firstBoxType;
    std::uint8_t firstBoxMagic;
};

struct TibMacHandle {
    std::uint64_t recordStart;
    std::uint32_t start;
    std::uint32_t size;
    std::uint32_t recordSize;
};

struct TibMacEntryName {
    std::u16string name;
    std::uint8_t type;
    std::uint64_t size;
    std::uint64_t mtime;
    std::uint64_t atime;
};

struct TibMacTreeEntry {
    TibMacEntryName item;
    std::uint64_t numEntries;
    std::int64_t parent;
    std::vector<TibMacHandle> handles;
};

struct TibMacSlice {
    std::array<std::uint8_t, 16> id;
    std::uint64_t timestamp;
    std::int64_t parent;
    std::vector<TibMacTreeEntry> entries;
};

bool linkTibMacSlices(std::vector<TibMacSlice>* slices, std::string* error);

// Directory counts are interpreted as the number of following descendants.
bool buildTibMacTree(std::vector<TibMacTreeEntry>* entries, std::string* error);

// Each span contains complete serialized groups, excluding the handlesSize field.
bool parseTibMacHandleGroups(const std::uint8_t* bytes, std::size_t size,
                             std::vector<TibMacHandle>* dataHandles, std::string* error);

ByteStorePtr openTibMacFile(const ByteStorePtr& data, std::uint32_t blockSize,
                            const std::vector<TibMacHandle>& handles,
                            std::uint64_t fileSize, std::string* error);

// Span starts at filenameSize and ends immediately after the UTF-16 name.
bool parseTibMacEntryName(const std::uint8_t* bytes, std::size_t size,
                          TibMacEntryName* entry, std::string* error);

bool parseTibMacHandleDescriptors(const std::uint8_t* bytes, std::size_t size,
                                  std::vector<TibMacHandle>* handles, std::string* error);

// The start-5 relation is a format hypothesis and must be validated for each
// handle. recordSize is retained as metadata until its semantics are known.
ByteStorePtr openTibMacHandle(const ByteStorePtr& data, std::uint32_t blockSize,
                              const TibMacHandle& handle, std::string* error);

bool inspectTibMac(const ByteStorePtr& data, const ByteStorePtr& companion,
                   TibMacInspection* result, std::string* error);

bool readTibMacIndex(const ByteStorePtr& data, const ByteStorePtr& companion,
                     std::uint32_t* blockSize, std::vector<TibMacSlice>* slices,
                     std::string* error);

} // namespace fs
} // namespace peare
