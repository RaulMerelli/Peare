#pragma once

#include "TibFileIndex.h"

namespace peare {
namespace fs {

struct TibLegacyFileNode {
    // SIZE_MAX denotes the root, which has no metadata of its own.
    std::size_t parent;
    std::u16string name;
    std::u16string extendedName;
    std::u16string path;
    std::uint64_t fieldA;
    std::uint32_t fieldB;
    std::uint64_t recordPosition;
    std::uint64_t fieldD;
    std::uint32_t fieldE;
    std::uint32_t fieldF;
    std::uint8_t typeCode;
    std::uint32_t fieldG;
    bool hasField13;
    std::uint64_t field13;
    bool hasField14;
    std::uint32_t field14;
    std::vector<std::uint8_t> string14;
    bool hasField31;
    std::uint8_t field31;
};

// Parses the tree prefix. Remaining decoded bytes are not interpreted.
bool parseTibLegacyFileTree(const std::vector<std::uint8_t>& decoded,
                            TibFileMetadataVersion version,
                            std::vector<TibLegacyFileNode>* nodes,
                            std::size_t* consumed, std::string* error = nullptr);

struct TibLegacyChunkHash {
    std::uint8_t algorithm;
    std::vector<std::uint8_t> bytes;
};

// These parse decoded prefixes only; they do not verify or associate the hash.
bool parseTibLegacyHashChunk(const std::vector<std::uint8_t>& decoded,
                             TibLegacyChunkHash* hash, std::size_t* consumed,
                             std::string* error = nullptr);
bool parseTibLegacyChunk5(const std::vector<std::uint8_t>& decoded,
                          std::vector<std::uint8_t>* value, std::size_t* consumed,
                          std::string* error = nullptr);

}  // namespace fs
}  // namespace peare
