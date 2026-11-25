#pragma once

#include "TibOldFileIndex.h"
#include "TibOldFileStore.h"

#include <memory>
#include <string>
#include <vector>

namespace peare {
namespace fs {

enum class TibOldDirectoryStatus {
    Ok,
    InvalidArgument,
    InvalidSnapshot,
    AmbiguousCatalog,
    InvalidNode,
    UnresolvedRecord,
    MissingPayload,
    InvalidPayload,
    NotDirectory,
    NotFile,
    SourceChanged,
    AllocationFailure
};

struct TibOldDirectoryEntry {
    std::u16string name;
    bool directory = false;
    bool deleted = false;
    bool hasPayload = false;
    std::uint64_t size = 0;
};

class TibOldFileDirectory {
public:
    // SIZE_MAX identifies the virtual list of roots, not a catalog node.
    static std::size_t roots();

    // The index must have been built from these same sources, which must stay
    // immutable. Catalog scanning is heuristic, even when creation succeeds.
    static TibOldDirectoryStatus create(
        const TibOldFileIndex& index, std::size_t snapshotIndex,
        const std::vector<ByteStorePtr>& volumes, const TibOldFieldLimits& limits,
        std::shared_ptr<TibOldFileDirectory>* output);

    bool catalogIsHeuristic() const { return true; }
    std::size_t nodeCount() const { return entries_.size(); }
    TibOldDirectoryStatus entry(std::size_t node, TibOldDirectoryEntry* output) const;
    TibOldDirectoryStatus children(std::size_t node, std::vector<std::size_t>* output) const;
    TibOldDirectoryStatus openFile(std::size_t node, ByteStorePtr* output) const;

private:
    struct Entry {
        TibOldDirectoryEntry info;
        TibOldField payload;
    };

    bool sourcesValid() const;
    std::vector<ByteStorePtr> volumes_;
    std::vector<std::int64_t> capacities_;
    TibOldFieldLimits limits_;
    std::vector<Entry> entries_;
    std::vector<std::vector<std::size_t> > children_;
    std::vector<std::size_t> roots_;
};

} // namespace fs
} // namespace peare
