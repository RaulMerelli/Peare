#pragma once

#include "DiscStore.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace peare {
namespace fs {

struct TibOldFieldLimits {
    std::size_t maxVolumes = std::numeric_limits<std::size_t>::max();
    std::size_t maxFields = std::numeric_limits<std::size_t>::max();
    std::uint64_t maxInputBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxFieldBytes = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t maxTotalBytes = std::numeric_limits<std::uint64_t>::max();
};

struct TibOldFieldPosition {
    std::size_t volumeIndex = 0;
    std::uint64_t volumeOffset = 0;
    std::uint64_t logicalOffset = 0;
};

struct TibOldField {
    std::uint8_t tag = 0;
    TibOldFieldPosition start;
    TibOldFieldPosition end;
    std::uint64_t compressedBytes = 0;
    std::uint64_t decodedBytes = 0;
};

struct TibOldSkippedRange {
    std::size_t volumeIndex = 0;
    std::uint64_t volumeOffset = 0;
    std::uint64_t length = 0;
};

enum class TibOldFieldStatus {
    Ok,
    InvalidArgument,
    LimitExceeded,
    InvalidVolume,
    InvalidSequence,
    ReadError,
    InvalidDeflate,
    TruncatedField,
    TruncatedChecksum,
    ChecksumMismatch,
    CallbackStopped,
    AllocationFailure,
    OkWithSkippedData
};

class TibOldFieldSink {
public:
    virtual ~TibOldFieldSink() {}
    virtual bool beginField(const TibOldField& field) = 0;
    virtual bool fieldData(const std::uint8_t* data, std::size_t size) = 0;
    virtual void endField(const TibOldField& field, bool verified) = 0;
    virtual void skippedSectionTail(const TibOldSkippedRange&) {}
};

// Callbacks must not throw; field data is tentative until endField(..., true).
// Volume signatures, identifiers and sequences are checked, not the variant.
// OkWithSkippedData reports unverified ranges after section-end fields.
TibOldFieldStatus scanTibOldFileFields(const std::vector<ByteStorePtr>& volumes,
                                       const TibOldFieldLimits& limits,
                                       TibOldFieldSink* sink);

// TruncatedField does not establish that the compressed prefix was valid.
// Reads one field after validating all volume headers and aggregate size.
TibOldFieldStatus readTibOldFileFieldAt(const std::vector<ByteStorePtr>& volumes,
                                        const TibOldFieldPosition& position,
                                        const TibOldFieldLimits& limits,
                                        TibOldFieldSink* sink);

} // namespace fs
} // namespace peare
