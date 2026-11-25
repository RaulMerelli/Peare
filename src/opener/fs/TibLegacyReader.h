#pragma once

#include "DiscStore.h"
#include "WimReader.h"

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

class TibDedupProvider;

struct TibLegacyPartition {
    unsigned int index;
    ByteStorePtr content;
    ByteStorePtr fatClusterFilter;
};

class TibLegacyFilteredVolume final : public IByteStore, public IWofResourceResolver {
public:
    TibLegacyFilteredVolume(ByteStorePtr source, ByteStorePtr filter)
        : source_(std::move(source)), filter_(std::move(filter)) {}
    std::int64_t capacity() const override { return source_->capacity(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return source_->read(pos, dst, count);
    }
    bool validate() const override { return source_->validate() && filter_->validate(); }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }
    const ByteStorePtr& fatClusterFilter() const { return filter_; }
    ByteStorePtr resolveExternalWofResource(const std::string& path,
                                             const WofWimReference& reference) const override;
private:
    ByteStorePtr source_;
    ByteStorePtr filter_;
};

bool openTibLegacyDisk(const ByteStorePtr& source,
                       std::vector<TibLegacyPartition>* partitions,
                       std::string* error,
                       const std::shared_ptr<TibDedupProvider>& provider = std::shared_ptr<TibDedupProvider>(),
                       bool* needsProviderDuringProbe = nullptr);
void attachTibLegacyDedupProvider(std::vector<TibLegacyPartition>* partitions,
                                  const std::shared_ptr<TibDedupProvider>& provider);
bool tibLegacyNeedsDedupProvider(const std::vector<TibLegacyPartition>& partitions);
std::vector<ByteStorePtr> makeTibLegacyPartitionStores(
    const std::vector<TibLegacyPartition>& partitions);

} // namespace fs
} // namespace peare
