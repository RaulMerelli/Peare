#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

struct TibFileIndexEntry {
    std::u16string path;
    std::vector<std::uint8_t> metadata;
};

struct TibFileMetadataVersion {
    std::uint32_t major;
    std::uint32_t minor;
};

struct TibFileMetadata {
    TibFileMetadataVersion version;
    // Presence depends on the version; zero is still a recorded value.
    bool hasPositionA;
    std::uint64_t positionA;
    bool hasPositionB;
    std::uint64_t positionB;
    bool hasEncryptionVector;
    std::vector<std::uint8_t> encryptionVector;
    std::vector<std::uint64_t> versionFields;
    std::vector<std::uint8_t> decoded;
};

struct TibFileIndexFallback {
    bool metadataAvailable;
    bool legacyTreeAvailable;
    TibFileMetadataVersion legacyVersion;
    std::vector<std::uint8_t> legacyTree;
    TibFileMetadata metadata;
    std::string metadataError;
    // Source coordinates; metadataEnd is exclusive.
    std::uint64_t dataOffset;
    std::uint64_t metadataStart;
    std::uint64_t metadataEnd;
    std::uint64_t trailerStart;
};

// Preserves the decoded e record and its checked vector, including processor and CRC.
bool parseTibFileMetadata(const std::vector<std::uint8_t>& decoded,
                          TibFileMetadata* metadata, std::string* error = nullptr);

// Reads e without requiring a readable g index; dataOffset is the archive base.
bool readTibFileMetadata(const ByteStorePtr& source, TibFileMetadata* metadata,
                         std::uint64_t* dataOffset, std::string* error = nullptr);

inline bool supportsTibFileMetadataVersion(const TibFileMetadataVersion& version) {
    return version.major == 5 && version.minor <= 2;
}

// Reads and checks g within [start, limit); recordEnd is after its CRC.
// fatal distinguishes source/resource failures from an invalid candidate.
bool readTibFileGRecord(const ByteStorePtr& source, std::int64_t start,
                        std::int64_t limit, std::vector<std::uint8_t>* decoded,
                        std::int64_t* recordEnd, bool* fatal, std::string* error = nullptr);

// The caller must select a supported metadata version before parsing g.
bool parseTibFileGIndex(const std::vector<std::uint8_t>& decoded,
                        std::vector<TibFileIndexEntry>* entries, std::string* error = nullptr);

// With fallback, a checked encryption vector stops the search before reading g.
// Unencrypted candidates retain the normal e/g index search.
bool readTibFileIndex(const ByteStorePtr& source,
                      std::vector<TibFileIndexEntry>* entries,
                      std::uint64_t* dataOffset,
                      TibFileMetadataVersion* metadataVersion,
                      std::string* error = nullptr,
                      TibFileIndexFallback* fallback = nullptr);

} // namespace fs
} // namespace peare
