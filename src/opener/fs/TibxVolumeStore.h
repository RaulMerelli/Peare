#pragma once

#include "TibxPageStore.h"
#include "TibxItemMetadata.h"
#include "PartitionTable.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>

namespace peare {
namespace fs {

class TibxVolumeContext {
public:
    TibxVolumeContext(ByteStorePtr source, TibxPageStore::CommittedHeader header,
                      TibxPageStore::Superblock dataMap, TibxPageStore::Superblock segmentMap);
    std::shared_ptr<const std::vector<TibxPageStore::Extent> > loadExtents(std::uint64_t volumeId) const;
    bool validate() const;

    TibxPageStore pages;
    TibxPageStore::CommittedHeader header;
    TibxPageStore::Superblock dataMap;
    TibxPageStore::Superblock segmentMap;
private:
    ByteStorePtr source_;
    std::int64_t sourceSize_;
    mutable std::mutex mutex_;
    mutable std::map<std::uint64_t, std::shared_ptr<const std::vector<TibxPageStore::Extent> > > extents_;
};

class TibxRecoverySnapshot {
public:
    const std::vector<TibxPageStore::ResolvedItem>& items() const { return items_; }
private:
    friend class TibxRecoveryItems;
    std::shared_ptr<TibxVolumeContext> context_;
    std::uint32_t fullId_, sliceId_;
    std::vector<TibxPageStore::ResolvedItem> items_;
};

class TibxRecoveryItems {
public:
    TibxRecoveryItems(std::shared_ptr<TibxVolumeContext> context,
                      TibxPageStore::Superblock itemMap, std::uint32_t fullId,
                      std::uint32_t sliceId);
    std::shared_ptr<const std::vector<TibxPageStore::ResolvedItem> > load(std::string* error = nullptr) const;
    std::shared_ptr<const TibxRecoverySnapshot> snapshot(std::string* error = nullptr) const;
    bool matches(const TibxRecoverySnapshot& snapshot) const;
private:
    std::shared_ptr<TibxVolumeContext> context_;
    TibxPageStore::Superblock itemMap_;
    std::uint32_t fullId_, sliceId_;
};

class TibxVolumeStore final : public IByteStore {
public:
    enum class StreamResult { Found, NotFound, Ambiguous, Invalid, Unavailable };
    TibxVolumeStore(std::shared_ptr<TibxVolumeContext> context, std::uint64_t volumeId,
                    std::int64_t length, std::uint32_t itemSlice,
                    std::shared_ptr<const std::vector<TibxPageStore::Volume> > recoveryVolumes = nullptr,
                    std::shared_ptr<const TibxRecoveryItems> recoveryItems = nullptr,
                    std::string itemName = std::string());
    std::int64_t capacity() const override;
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override;
    // These are streams from the same recovery point, not verified disk identities.
    std::shared_ptr<const std::vector<TibxPageStore::Volume> > recoveryVolumes() const;
    ByteStorePtr openRecoveryVolume(std::size_t index) const;
    std::shared_ptr<const std::vector<TibxPageStore::ResolvedItem> > recoveryItems(std::string* error = nullptr) const;
    std::shared_ptr<const TibxRecoverySnapshot> recoverySnapshot(std::string* error = nullptr) const;
    // Selector is the serialized descriptor value; absence does not invalidate the item.
    StreamResult openRecoveryItemStream(const std::string& itemName, std::uint32_t itemSlice,
                                        std::uint8_t selector, ByteStorePtr* stream,
                                        std::string* error = nullptr) const;
    StreamResult openRecoveryItemStream(const std::shared_ptr<const TibxRecoverySnapshot>& snapshot,
                                        std::size_t itemIndex, std::uint8_t selector,
                                        ByteStorePtr* stream, std::string* error = nullptr) const;
    StreamResult readRecoveryItemMetadata(const std::string& itemName, std::uint32_t itemSlice,
                                          TibxMetadataDomain domain,
                                          std::vector<TibxMetadataField>* fields,
                                          std::string* error = nullptr) const;
    StreamResult readRecoveryItemMetadata(const std::shared_ptr<const TibxRecoverySnapshot>& snapshot,
                                          std::size_t itemIndex, TibxMetadataDomain domain,
                                          std::vector<TibxMetadataField>* fields,
                                          std::string* error = nullptr) const;
    StreamResult openFatClusterFilter(ByteStorePtr* stream) const;

    PartitionResolveResult openWofSibling(const PartitionIdentity& target,
                                          ByteStorePtr* stream) const;

private:
    std::shared_ptr<TibxVolumeContext> context_;
    std::uint64_t volumeId_;
    std::int64_t length_;
    std::uint32_t itemSlice_;
    std::shared_ptr<const std::vector<TibxPageStore::Volume> > recoveryVolumes_;
    std::shared_ptr<const TibxRecoveryItems> recoveryItems_;
    std::string itemName_;
    mutable std::atomic<bool> failed_;
};

}  // namespace fs
}  // namespace peare
