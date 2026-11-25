#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <utility>
#include <vector>

namespace peare {
namespace fs {

class TibRandomAccessIndex {
public:
    // Each resolved store starts at the first byte of the requested page.
    typedef std::function<ByteStorePtr(std::uint32_t)> PageResolver;

    enum class Result { Ok, Absent, Invalid, IoError, ChecksumError, ResourceError };

    struct ExternalReference {
        std::uint64_t offset;
        std::uint32_t length;
    };

    TibRandomAccessIndex(std::uint32_t pageSize, std::uint32_t tag06,
                         std::uint32_t tag14, bool hasRootD2, std::uint32_t rootD2,
                         bool hasRoot9C, std::uint32_t root9C, PageResolver resolver);

    bool valid() const { return valid_; }
    std::uint32_t count() const { return count_; }
    std::uint32_t fanout() const { return fanout_; }

    // Output arguments are changed only on success.
    Result raw(std::uint32_t index, std::uint32_t* value) const;
    Result reference(std::uint32_t index, std::uint32_t* value) const;
    // index identifies the marker; the following three logical entries are raw.
    Result external(std::uint32_t index, ExternalReference* value) const;

private:
    Result readPage(std::uint32_t reference, std::vector<std::uint8_t>* page) const;
    Result leaf(std::uint32_t index, std::vector<std::uint8_t>* page) const;

    std::uint32_t pageSize_, count_, fanout_, root_;
    PageResolver resolver_;
    bool valid_;
    mutable std::mutex cacheMutex_;
    mutable std::list<std::pair<std::uint32_t, std::vector<std::uint8_t> > > cache_;
    mutable std::size_t cacheBytes_;
};

} // namespace fs
} // namespace peare
