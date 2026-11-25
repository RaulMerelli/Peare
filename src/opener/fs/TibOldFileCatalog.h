#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace peare {
namespace fs {

struct TibOldCatalogLimits {
    std::size_t maxInputBytes = 64U * 1024 * 1024;
    std::size_t maxRecords = 100000;
    std::size_t maxCandidates = 100000;
    std::size_t maxStringUnits = 32768;
    std::size_t maxProbeUnits = 16U * 1024 * 1024;
    std::size_t maxCandidateUnits = 4U * 1024 * 1024;
};

enum class TibOldCatalogStatus {
    Ok,
    InvalidArgument,
    InvalidRecord,
    InvalidHeader,
    DuplicateRecordOffset,
    MissingRecord,
    DuplicateReference,
    DuplicateName,
    CandidateCollision,
    InvalidCandidate,
    InvalidDepth,
    LimitExceeded,
    InvalidIndex
};

struct TibOldCatalogRecord {
    std::uint64_t tagOffset = 0;
    std::u16string name;
    std::uint8_t type = 0;
    std::uint32_t depth = 0;
};

typedef std::unordered_map<std::uint64_t, std::size_t> TibOldCatalogRecordIndex;

struct TibOldCatalogCandidate {
    std::size_t start = 0;
    // The actual end is unknown; this is only the required minimum.
    std::size_t minimumEnd = 0;
    std::u16string firstString;
    std::u16string secondString;
    std::uint64_t timestamp = 0;
    std::uint32_t opaque = 0;
    std::uint64_t recordOffset = 0;
    std::uint32_t state = 0;
    bool overlapsMinimum = false;
};

struct TibOldCatalogScan {
    std::vector<TibOldCatalogCandidate> candidates;
    bool hasEntryStates = false;
    bool hasMinimumOverlaps = false;
    bool hasImplausibleSpans = false;
};

struct TibOldCatalogNode {
    // SIZE_MAX denotes a root; all other values index the returned node vector.
    std::size_t parent = static_cast<std::size_t>(-1);
    std::u16string name;
    std::uint64_t recordOffset = 0;
    std::size_t candidateIndex = 0;
    std::uint32_t depth = 0;
    std::uint8_t type = 0;
};

// On failure, output is unchanged. Names are preserved as UTF-16 code units.
TibOldCatalogStatus parseTibOldCatalogRecord(
    const std::vector<std::uint8_t>& decoded, std::uint64_t tagOffset,
    const TibOldCatalogLimits& limits, TibOldCatalogRecord* output);

// Candidates are heuristic, not a grammar or a verified tree. Even without
// minimum overlaps, unknown entry lengths can make the result ambiguous.
TibOldCatalogStatus scanTibOldCatalog(
    const std::vector<std::uint8_t>& decoded,
    const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogLimits& limits, TibOldCatalogScan* output);

// The caller maintains a complete one-to-one index for the current records,
// including unique offsets and valid record types, without concurrent mutation.
// Size and accessed entries are checked; other entries are not revalidated.
TibOldCatalogStatus scanTibOldCatalog(
    const std::vector<std::uint8_t>& decoded,
    const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogRecordIndex& recordIndex,
    const TibOldCatalogLimits& limits, TibOldCatalogScan* output);

// Roots must have depth zero. Names compare as exact code units. Output is
// unchanged on failure; success does not verify the heuristic.
TibOldCatalogStatus buildTibOldCatalogTree(
    const TibOldCatalogScan& scan, const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogLimits& limits, std::vector<TibOldCatalogNode>* output);

// The same index invariants and per-access checks apply as for the scan.
TibOldCatalogStatus buildTibOldCatalogTree(
    const TibOldCatalogScan& scan, const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogRecordIndex& recordIndex,
    const TibOldCatalogLimits& limits, std::vector<TibOldCatalogNode>* output);

} // namespace fs
} // namespace peare
