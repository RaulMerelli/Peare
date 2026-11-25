#include "TibOldFileCatalog.h"

#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace peare {
namespace fs {
namespace {

std::uint32_t read32(const std::vector<std::uint8_t>& bytes, std::size_t pos)
{
    return std::uint32_t(bytes[pos]) | (std::uint32_t(bytes[pos + 1]) << 8) |
           (std::uint32_t(bytes[pos + 2]) << 16) |
           (std::uint32_t(bytes[pos + 3]) << 24);
}

std::uint64_t read64(const std::vector<std::uint8_t>& bytes, std::size_t pos)
{
    return std::uint64_t(read32(bytes, pos)) |
           (std::uint64_t(read32(bytes, pos + 4)) << 32);
}

std::u16string readString(const std::vector<std::uint8_t>& bytes,
                          std::size_t pos, std::size_t units)
{
    std::u16string result;
    result.reserve(units);
    for (std::size_t i = 0; i < units; ++i) {
        result.push_back(static_cast<char16_t>(std::uint16_t(bytes[pos]) |
                         (std::uint16_t(bytes[pos + 1]) << 8)));
        pos += 2;
    }
    return result;
}

enum class Probe { Valid, Invalid, Limit };

Probe probeString(const std::vector<std::uint8_t>& bytes, std::size_t* pos,
                  const TibOldCatalogLimits& limits, std::size_t* budget,
                  std::size_t* stringPos, std::size_t* units)
{
    if (bytes.size() - *pos < 4) return Probe::Invalid;
    const std::uint32_t length = read32(bytes, *pos);
    *pos += 4;
    if (length == 0 || std::uint64_t(length) > (bytes.size() - *pos) / 2)
        return Probe::Invalid;
    if (std::uint64_t(length) > limits.maxStringUnits) return Probe::Limit;
    if (std::uint64_t(length) > *budget) return Probe::Limit;
    *budget -= static_cast<std::size_t>(length);
    *stringPos = *pos;
    *units = static_cast<std::size_t>(length);
    for (std::size_t i = 0; i < *units; ++i) {
        if (bytes[*pos] == 0 && bytes[*pos + 1] == 0) return Probe::Invalid;
        *pos += 2;
    }
    return Probe::Valid;
}

TibOldCatalogStatus makeRecordIndex(const std::vector<TibOldCatalogRecord>& records,
                                    TibOldCatalogRecordIndex* index)
{
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].type > 1) return TibOldCatalogStatus::InvalidRecord;
        if (!index->emplace(records[i].tagOffset, i).second)
            return TibOldCatalogStatus::DuplicateRecordOffset;
    }
    return TibOldCatalogStatus::Ok;
}

TibOldCatalogStatus checkRecord(const std::vector<TibOldCatalogRecord>& records,
                                TibOldCatalogRecordIndex::const_iterator entry)
{
    if (entry->second >= records.size() || records[entry->second].tagOffset != entry->first)
        return TibOldCatalogStatus::InvalidIndex;
    if (records[entry->second].type > 1) return TibOldCatalogStatus::InvalidRecord;
    return TibOldCatalogStatus::Ok;
}

} // namespace

TibOldCatalogStatus parseTibOldCatalogRecord(
    const std::vector<std::uint8_t>& decoded, std::uint64_t tagOffset,
    const TibOldCatalogLimits& limits, TibOldCatalogRecord* output)
{
    if (!output) return TibOldCatalogStatus::InvalidArgument;
    if (decoded.size() > limits.maxInputBytes) return TibOldCatalogStatus::LimitExceeded;
    if (decoded.size() < 9) return TibOldCatalogStatus::InvalidRecord;
    const std::uint32_t units = read32(decoded, 0);
    if (std::uint64_t(units) != (decoded.size() - 9) / 2 ||
        (decoded.size() - 9) % 2 != 0)
        return TibOldCatalogStatus::InvalidRecord;
    if (std::uint64_t(units) > limits.maxStringUnits)
        return TibOldCatalogStatus::LimitExceeded;
    const std::size_t typePos = decoded.size() - 5;
    if (decoded[typePos] > 1) return TibOldCatalogStatus::InvalidRecord;
    TibOldCatalogRecord record;
    record.tagOffset = tagOffset;
    record.name = readString(decoded, 4, static_cast<std::size_t>(units));
    record.type = decoded[typePos];
    record.depth = read32(decoded, typePos + 1);
    *output = std::move(record);
    return TibOldCatalogStatus::Ok;
}

TibOldCatalogStatus scanTibOldCatalog(
    const std::vector<std::uint8_t>& decoded,
    const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogLimits& limits, TibOldCatalogScan* output)
{
    if (!output) return TibOldCatalogStatus::InvalidArgument;
    if (decoded.size() > limits.maxInputBytes || records.size() > limits.maxRecords)
        return TibOldCatalogStatus::LimitExceeded;
    if (decoded.size() < 12 || read32(decoded, 0) != 1 ||
        (read32(decoded, 4) != 1 && (decoded.size() < 32 ||
         read32(decoded, 4) != 2 || read32(decoded, 8) != 1 || read32(decoded, 12) != 1)))
        return TibOldCatalogStatus::InvalidHeader;
    TibOldCatalogRecordIndex index;
    const TibOldCatalogStatus status = makeRecordIndex(records, &index);
    if (status != TibOldCatalogStatus::Ok) return status;
    return scanTibOldCatalog(decoded, records, index, limits, output);
}

TibOldCatalogStatus scanTibOldCatalog(
    const std::vector<std::uint8_t>& decoded,
    const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogRecordIndex& recordIndex,
    const TibOldCatalogLimits& limits, TibOldCatalogScan* output)
{
    if (!output) return TibOldCatalogStatus::InvalidArgument;
    if (decoded.size() > limits.maxInputBytes || records.size() > limits.maxRecords)
        return TibOldCatalogStatus::LimitExceeded;
    if (decoded.size() < 12 || read32(decoded, 0) != 1 ||
        (read32(decoded, 4) != 1 && (decoded.size() < 32 ||
         read32(decoded, 4) != 2 || read32(decoded, 8) != 1 || read32(decoded, 12) != 1)))
        return TibOldCatalogStatus::InvalidHeader;
    if (recordIndex.size() != records.size()) return TibOldCatalogStatus::InvalidIndex;
    TibOldCatalogScan result;
    result.hasEntryStates = read32(decoded, 4) == 1;
    std::size_t budget = limits.maxProbeUnits;
    std::size_t candidateUnits = 0;
    std::size_t furthestEnd = 0;
    std::size_t furthestIndex = 0;
    for (std::size_t start = read32(decoded, 4) == 1 ? 12 : 32; start < decoded.size();) {
        std::size_t pos = start, firstPos = 0, secondPos = 0;
        std::size_t firstUnits = 0, secondUnits = 0;
        Probe probe = probeString(decoded, &pos, limits, &budget, &firstPos, &firstUnits);
        if (probe == Probe::Limit) return TibOldCatalogStatus::LimitExceeded;
        if (probe == Probe::Valid)
            probe = probeString(decoded, &pos, limits, &budget, &secondPos, &secondUnits);
        if (probe == Probe::Limit) return TibOldCatalogStatus::LimitExceeded;
        if (probe == Probe::Valid && decoded.size() - pos >= 36) {
            const auto entry = recordIndex.find(read64(decoded, pos + 12));
            if (entry == recordIndex.end()) {
                if (decoded.size() - start <= 2) break;
                start += 2;
                continue;
            }
            const TibOldCatalogStatus status = checkRecord(records, entry);
            if (status != TibOldCatalogStatus::Ok) return status;
            if (result.candidates.size() >= limits.maxCandidates ||
                firstUnits > limits.maxCandidateUnits - candidateUnits ||
                secondUnits > limits.maxCandidateUnits - candidateUnits - firstUnits)
                return TibOldCatalogStatus::LimitExceeded;
            candidateUnits += firstUnits + secondUnits;
            TibOldCatalogCandidate candidate;
            candidate.start = start;
            candidate.minimumEnd = pos + 36;
            candidate.firstString = readString(decoded, firstPos, firstUnits);
            candidate.secondString = readString(decoded, secondPos, secondUnits);
            candidate.timestamp = read64(decoded, pos);
            candidate.opaque = read32(decoded, pos + 8);
            candidate.recordOffset = read64(decoded, pos + 12);
            candidate.state = read32(decoded, pos + 32);
            if (!result.candidates.empty() && start < furthestEnd) {
                candidate.overlapsMinimum = true;
                result.candidates[furthestIndex].overlapsMinimum = true;
                result.hasMinimumOverlaps = true;
            }
            if (candidate.minimumEnd > furthestEnd) {
                furthestEnd = candidate.minimumEnd;
                furthestIndex = result.candidates.size();
            }
            result.candidates.push_back(std::move(candidate));
        }
        if (decoded.size() - start <= 2) break;
        start += 2;
    }
    for (std::size_t i = 0; i < result.candidates.size(); ++i) {
        const std::size_t end = i + 1 < result.candidates.size()
            ? result.candidates[i + 1].start : decoded.size();
        const std::size_t minimumEnd = result.candidates[i].minimumEnd;
        if (end >= minimumEnd && end - minimumEnd > 4096U - 36U)
            result.hasImplausibleSpans = true;
    }
    *output = std::move(result);
    return TibOldCatalogStatus::Ok;
}

TibOldCatalogStatus buildTibOldCatalogTree(
    const TibOldCatalogScan& scan, const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogLimits& limits, std::vector<TibOldCatalogNode>* output)
{
    if (!output) return TibOldCatalogStatus::InvalidArgument;
    if (records.size() > limits.maxRecords || scan.candidates.size() > limits.maxCandidates)
        return TibOldCatalogStatus::LimitExceeded;
    if (scan.candidates.empty()) return TibOldCatalogStatus::InvalidCandidate;
    if (scan.hasMinimumOverlaps) return TibOldCatalogStatus::CandidateCollision;
    if (scan.hasImplausibleSpans) return TibOldCatalogStatus::InvalidCandidate;
    TibOldCatalogRecordIndex index;
    const TibOldCatalogStatus status = makeRecordIndex(records, &index);
    if (status != TibOldCatalogStatus::Ok) return status;
    return buildTibOldCatalogTree(scan, records, index, limits, output);
}

TibOldCatalogStatus buildTibOldCatalogTree(
    const TibOldCatalogScan& scan, const std::vector<TibOldCatalogRecord>& records,
    const TibOldCatalogRecordIndex& recordIndex,
    const TibOldCatalogLimits& limits, std::vector<TibOldCatalogNode>* output)
{
    if (!output) return TibOldCatalogStatus::InvalidArgument;
    if (records.size() > limits.maxRecords || scan.candidates.size() > limits.maxCandidates)
        return TibOldCatalogStatus::LimitExceeded;
    if (scan.candidates.empty()) return TibOldCatalogStatus::InvalidCandidate;
    if (scan.hasMinimumOverlaps) return TibOldCatalogStatus::CandidateCollision;
    if (scan.hasImplausibleSpans) return TibOldCatalogStatus::InvalidCandidate;
    if (recordIndex.size() != records.size()) return TibOldCatalogStatus::InvalidIndex;
    std::size_t previousEnd = 0;
    std::size_t totalUnits = 0;
    for (std::size_t i = 0; i < scan.candidates.size(); ++i) {
        const TibOldCatalogCandidate& candidate = scan.candidates[i];
        if (candidate.overlapsMinimum) return TibOldCatalogStatus::CandidateCollision;
        if (candidate.start < 12 || candidate.start % 2 != 0 ||
            candidate.minimumEnd <= candidate.start || candidate.firstString.empty() ||
            candidate.secondString.empty() ||
            candidate.firstString.find(u'\0') != std::u16string::npos ||
            candidate.secondString.find(u'\0') != std::u16string::npos)
            return TibOldCatalogStatus::InvalidCandidate;
        if (i != 0 && candidate.start < previousEnd)
            return TibOldCatalogStatus::CandidateCollision;
        previousEnd = candidate.minimumEnd;
        if (candidate.firstString.size() > limits.maxStringUnits ||
            candidate.secondString.size() > limits.maxStringUnits ||
            candidate.firstString.size() > limits.maxCandidateUnits - totalUnits)
            return TibOldCatalogStatus::LimitExceeded;
        totalUnits += candidate.firstString.size();
        if (candidate.secondString.size() > limits.maxCandidateUnits - totalUnits)
            return TibOldCatalogStatus::LimitExceeded;
        totalUnits += candidate.secondString.size();
    }
    std::vector<TibOldCatalogNode> nodes;
    std::vector<std::size_t> ancestors;
    std::unordered_set<std::uint64_t> usedReferences;
    std::map<std::pair<std::size_t, std::u16string>, bool> names;
    const std::size_t root = static_cast<std::size_t>(-1);
    for (std::size_t i = scan.candidates.size(); i > 0; --i) {
        const TibOldCatalogCandidate& candidate = scan.candidates[i - 1];
        const auto record = recordIndex.find(candidate.recordOffset);
        if (record == recordIndex.end()) return TibOldCatalogStatus::MissingRecord;
        const TibOldCatalogStatus status = checkRecord(records, record);
        if (status != TibOldCatalogStatus::Ok) return status;
        if (!usedReferences.insert(candidate.recordOffset).second)
            return TibOldCatalogStatus::DuplicateReference;
        const TibOldCatalogRecord& metadata = records[record->second];
        if (nodes.empty() && metadata.depth != 0) return TibOldCatalogStatus::InvalidDepth;
        const std::uint64_t level = metadata.depth;
        if (level > ancestors.size()) return TibOldCatalogStatus::InvalidDepth;
        ancestors.resize(static_cast<std::size_t>(level));
        const std::size_t parent = ancestors.empty() ? root : ancestors.back();
        if (parent != root && nodes[parent].type != 1)
            return TibOldCatalogStatus::InvalidDepth;
        if (!names.emplace(std::make_pair(parent, candidate.firstString), true).second)
            return TibOldCatalogStatus::DuplicateName;
        TibOldCatalogNode node;
        node.parent = parent;
        node.name = candidate.firstString;
        node.recordOffset = candidate.recordOffset;
        node.candidateIndex = i - 1;
        node.depth = metadata.depth;
        node.type = metadata.type;
        nodes.push_back(std::move(node));
        ancestors.push_back(nodes.size() - 1);
    }
    *output = std::move(nodes);
    return TibOldCatalogStatus::Ok;
}

} // namespace fs
} // namespace peare
