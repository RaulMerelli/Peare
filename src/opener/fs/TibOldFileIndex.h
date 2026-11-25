#pragma once

#include "TibOldFileCatalog.h"
#include "TibOldFileEnvelope.h"
#include "TibOldFileFields.h"

namespace peare {
namespace fs {

struct TibOldIndexLimits {
    TibOldFieldLimits fields;
    TibOldCatalogLimits catalog;
    std::size_t maxRecords = std::numeric_limits<std::size_t>::max();
    std::size_t maxCatalogs = std::numeric_limits<std::size_t>::max();
    std::size_t maxMetadataBytes = std::numeric_limits<std::size_t>::max();
    std::size_t maxTotalCandidates = std::numeric_limits<std::size_t>::max();
    std::size_t maxSkippedRanges = std::numeric_limits<std::size_t>::max();
    TibOldIndexLimits() {
        catalog.maxInputBytes = catalog.maxRecords = catalog.maxCandidates =
            catalog.maxStringUnits = catalog.maxProbeUnits = catalog.maxCandidateUnits =
                std::numeric_limits<std::size_t>::max();
    }
};

struct TibOldIndexRecord {
    TibOldCatalogRecord metadata;
    TibOldField field;
    bool hasPayload = false;
    TibOldField payload;
    std::uint64_t primaryLength = 0;
};

struct TibOldIndexSnapshot {
    TibOldField field;
    TibOldCatalogScan scan;
    std::vector<TibOldCatalogNode> nodes;
    TibOldCatalogStatus catalogStatus = TibOldCatalogStatus::Ok;
    bool sectionClosed = false;
};

struct TibOldFileIndex {
    std::vector<TibOldIndexRecord> records;
    std::vector<TibOldIndexSnapshot> snapshots;
    std::vector<TibOldSkippedRange> skippedRanges;
    bool trailingSectionIncomplete = false;
};

enum class TibOldIndexStatus {
    Ok,
    IncompleteTail,
    UnverifiedData,
    InvalidArgument,
    FieldError,
    CatalogRecordError,
    DuplicateRecord,
    DuplicateCatalog,
    UnassociatedPayload,
    DuplicatePayload,
    EnvelopeError,
    LimitExceeded,
    AllocationFailure
};

// Output is unchanged on failure. IncompleteTail returns records and snapshots
// through the last closed section containing a catalog, plus all observed
// skipped ranges. Without a closed catalog, only skipped ranges are returned;
// if none exist, output is unchanged. UnverifiedData returns
// verified fields and catalogs together with the unverified physical ranges.
// An incomplete tail is unverified even when the decoder reports physical EOF.
// Field data is committed only after checksum
// verification. Snapshot candidates and trees are heuristic: sectionClosed
// does not certify catalog completeness or the absence of false candidates.
// A record without a payload is not necessarily incomplete.
// EnvelopeError can stop decoding before the outer checksum is checked.
TibOldIndexStatus buildTibOldFileIndex(
    const std::vector<ByteStorePtr>& volumes, const TibOldIndexLimits& limits,
    TibOldFileIndex* output, TibOldFieldStatus* fieldStatus = nullptr,
    TibOldCatalogStatus* recordStatus = nullptr,
    TibOldEnvelopeStatus* envelopeStatus = nullptr);

} // namespace fs
} // namespace peare
