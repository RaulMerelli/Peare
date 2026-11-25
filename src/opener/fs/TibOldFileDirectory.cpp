#include "TibOldFileDirectory.h"

#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool validField(const TibOldField& field, std::uint8_t tag,
                const std::vector<std::int64_t>& capacities) {
    if (field.tag != tag || field.start.volumeIndex >= capacities.size() ||
        field.end.volumeIndex >= capacities.size() || field.start.volumeOffset < 12 ||
        field.end.volumeOffset < 12 ||
        field.start.volumeOffset >= std::uint64_t(capacities[field.start.volumeIndex]) ||
        field.end.volumeOffset > std::uint64_t(capacities[field.end.volumeIndex]) ||
        field.end.volumeIndex < field.start.volumeIndex ||
        (field.end.volumeIndex == field.start.volumeIndex &&
         field.end.volumeOffset <= field.start.volumeOffset) ||
        field.end.logicalOffset <= field.start.logicalOffset) return false;
    return true;
}

} // namespace

std::size_t TibOldFileDirectory::roots() {
    return std::numeric_limits<std::size_t>::max();
}

TibOldDirectoryStatus TibOldFileDirectory::create(
    const TibOldFileIndex& index, std::size_t snapshotIndex,
    const std::vector<ByteStorePtr>& volumes, const TibOldFieldLimits& limits,
    std::shared_ptr<TibOldFileDirectory>* output) {
    if (!output || volumes.empty()) return TibOldDirectoryStatus::InvalidArgument;
    if (snapshotIndex >= index.snapshots.size()) return TibOldDirectoryStatus::InvalidSnapshot;
    const TibOldIndexSnapshot& snapshot = index.snapshots[snapshotIndex];
    if (!snapshot.sectionClosed || snapshot.catalogStatus != TibOldCatalogStatus::Ok)
        return TibOldDirectoryStatus::InvalidSnapshot;
    if (snapshot.scan.hasMinimumOverlaps || snapshot.scan.hasImplausibleSpans ||
        snapshot.scan.candidates.empty() || snapshot.scan.candidates.size() != snapshot.nodes.size())
        return TibOldDirectoryStatus::AmbiguousCatalog;
    try {
        std::shared_ptr<TibOldFileDirectory> result(new TibOldFileDirectory());
        result->volumes_ = volumes;
        result->limits_ = limits;
        result->capacities_.reserve(volumes.size());
        for (const auto& volume : volumes) {
            if (!volume || volume->capacity() < 12 || !volume->validate())
                return TibOldDirectoryStatus::SourceChanged;
            result->capacities_.push_back(volume->capacity());
        }
        if (!validField(snapshot.field, 0x67, result->capacities_))
            return TibOldDirectoryStatus::InvalidSnapshot;
        std::size_t previousEnd = 0;
        for (const auto& candidate : snapshot.scan.candidates) {
            if (candidate.overlapsMinimum || candidate.start < 12 || candidate.start % 2 ||
                candidate.minimumEnd <= candidate.start || candidate.minimumEnd > snapshot.field.decodedBytes ||
                candidate.start < previousEnd || candidate.firstString.empty() ||
                candidate.secondString.empty() || candidate.secondString.find(u'\0') != std::u16string::npos)
                return TibOldDirectoryStatus::AmbiguousCatalog;
            previousEnd = candidate.minimumEnd;
        }
        std::unordered_map<std::uint64_t, std::size_t> records;
        for (std::size_t i = 0; i < index.records.size(); ++i) {
            if (!records.emplace(index.records[i].metadata.tagOffset, i).second)
                return TibOldDirectoryStatus::AmbiguousCatalog;
        }
        result->entries_.reserve(snapshot.nodes.size());
        result->children_.resize(snapshot.nodes.size());
        std::unordered_set<std::uint64_t> usedRecords;
        std::unordered_set<std::size_t> usedCandidates;
        std::map<std::pair<std::size_t, std::u16string>, bool> names;
        std::uint32_t rootDepth = 0;
        bool haveRoot = false;
        for (std::size_t i = 0; i < snapshot.nodes.size(); ++i) {
            const TibOldCatalogNode& node = snapshot.nodes[i];
            if (node.type > 1 || node.candidateIndex >= snapshot.scan.candidates.size() ||
                (node.parent != roots() && (node.parent >= i || !result->entries_[node.parent].info.directory)))
                return TibOldDirectoryStatus::InvalidNode;
            if (node.parent == roots()) {
                if (haveRoot && node.depth != rootDepth) return TibOldDirectoryStatus::InvalidNode;
                rootDepth = node.depth;
                haveRoot = true;
            } else if (std::uint64_t(snapshot.nodes[node.parent].depth) + 1 != node.depth) {
                return TibOldDirectoryStatus::InvalidNode;
            }
            const TibOldCatalogCandidate& candidate = snapshot.scan.candidates[node.candidateIndex];
            if (candidate.overlapsMinimum || candidate.recordOffset != node.recordOffset ||
                candidate.firstString != node.name || node.name.empty() ||
                node.name.find(u'\0') != std::u16string::npos ||
                !usedRecords.insert(node.recordOffset).second ||
                !usedCandidates.insert(node.candidateIndex).second ||
                !names.emplace(std::make_pair(node.parent, node.name), true).second)
                return TibOldDirectoryStatus::AmbiguousCatalog;
            const auto found = records.find(node.recordOffset);
            if (found == records.end()) return TibOldDirectoryStatus::UnresolvedRecord;
            const TibOldIndexRecord& record = index.records[found->second];
            if (record.metadata.type != node.type || record.metadata.depth != node.depth ||
                record.field.start.logicalOffset != node.recordOffset ||
                record.field.start.logicalOffset >= snapshot.field.start.logicalOffset ||
                !validField(record.field, 0x66, result->capacities_))
                return TibOldDirectoryStatus::InvalidNode;
            Entry entry;
            entry.info.name = node.name;
            entry.info.directory = node.type == 1;
            entry.info.deleted = snapshot.scan.hasEntryStates && candidate.state == 3;
            entry.info.hasPayload = record.hasPayload;
            if (record.hasPayload && !entry.info.directory) {
                if (record.primaryLength > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                    record.payload.start.logicalOffset < record.field.end.logicalOffset ||
                    record.payload.end.logicalOffset > snapshot.field.start.logicalOffset ||
                    !validField(record.payload, 0x02, result->capacities_))
                    return TibOldDirectoryStatus::InvalidPayload;
                entry.payload = record.payload;
                entry.info.size = record.primaryLength;
            }
            result->entries_.push_back(std::move(entry));
            if (node.parent == roots()) result->roots_.push_back(i);
            else result->children_[node.parent].push_back(i);
        }
        *output = std::move(result);
        return TibOldDirectoryStatus::Ok;
    } catch (const std::bad_alloc&) {
        return TibOldDirectoryStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldDirectoryStatus::AllocationFailure;
    }
}

bool TibOldFileDirectory::sourcesValid() const {
    for (std::size_t i = 0; i < volumes_.size(); ++i)
        if (!volumes_[i] || volumes_[i]->capacity() != capacities_[i] || !volumes_[i]->validate())
            return false;
    return true;
}

TibOldDirectoryStatus TibOldFileDirectory::entry(std::size_t node, TibOldDirectoryEntry* output) const {
    if (!output) return TibOldDirectoryStatus::InvalidArgument;
    if (node >= entries_.size()) return TibOldDirectoryStatus::InvalidNode;
    try {
        *output = entries_[node].info;
        return TibOldDirectoryStatus::Ok;
    } catch (const std::bad_alloc&) {
        return TibOldDirectoryStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldDirectoryStatus::AllocationFailure;
    }
}

TibOldDirectoryStatus TibOldFileDirectory::children(std::size_t node, std::vector<std::size_t>* output) const {
    if (!output) return TibOldDirectoryStatus::InvalidArgument;
    if (node != roots() && node >= entries_.size()) return TibOldDirectoryStatus::InvalidNode;
    if (node != roots() && !entries_[node].info.directory) return TibOldDirectoryStatus::NotDirectory;
    try {
        *output = node == roots() ? roots_ : children_[node];
        return TibOldDirectoryStatus::Ok;
    } catch (const std::bad_alloc&) {
        return TibOldDirectoryStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldDirectoryStatus::AllocationFailure;
    }
}

TibOldDirectoryStatus TibOldFileDirectory::openFile(std::size_t node, ByteStorePtr* output) const {
    if (!output) return TibOldDirectoryStatus::InvalidArgument;
    if (node >= entries_.size()) return TibOldDirectoryStatus::InvalidNode;
    const Entry& entry = entries_[node];
    if (entry.info.directory) return TibOldDirectoryStatus::NotFile;
    if (!entry.info.hasPayload) return TibOldDirectoryStatus::MissingPayload;
    if (!sourcesValid()) return TibOldDirectoryStatus::SourceChanged;
    try {
        ByteStorePtr store = std::make_shared<TibOldFileStore>(volumes_, entry.payload.start,
                                                              entry.info.size, limits_);
        if (!store->validate() || store->capacity() < 0) return TibOldDirectoryStatus::SourceChanged;
        *output = std::move(store);
        return TibOldDirectoryStatus::Ok;
    } catch (const std::bad_alloc&) {
        return TibOldDirectoryStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldDirectoryStatus::AllocationFailure;
    }
}

} // namespace fs
} // namespace peare
