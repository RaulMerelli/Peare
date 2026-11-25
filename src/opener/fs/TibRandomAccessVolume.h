#pragma once

#include "DiscStore.h"
#include "TibRandomAccessIndex.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace peare {
namespace fs {

struct TibRandomAccessGeometry {
    std::uint32_t sectorBytes;
    std::uint32_t sectorsPerBit;
    std::uint32_t bitsPerGroup;
    std::uint32_t sectorsPerStoredPage;
    std::uint32_t indexEntriesPerGroup;
    std::uint64_t sectorPrefix;
    bool rawCodec;
};

class TibRandomAccessVolume final : public IByteStore {
public:
    TibRandomAccessVolume(ByteStorePtr storage, ByteStorePtr direct,
                          std::shared_ptr<TibRandomAccessIndex> index,
                          std::uint32_t pageSize, std::uint64_t length,
                          std::uint64_t readableLength, TibRandomAccessGeometry geometry);
    std::int64_t capacity() const override;
    bool validate() const override;
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;

private:
    bool loadGroup(std::uint64_t group) const;

    ByteStorePtr storage_, direct_;
    std::shared_ptr<TibRandomAccessIndex> index_;
    std::uint32_t pageSize_;
    std::uint64_t length_, readableLength_;
    TibRandomAccessGeometry geometry_;
    mutable std::mutex mutex_;
    mutable std::uint64_t cachedGroup_;
    mutable std::vector<std::uint8_t> bitmap_, data_;
    mutable std::vector<std::uint32_t> bitmapRanks_;
};

} // namespace fs
} // namespace peare
