#pragma once

#include "DiscStore.h"
#include "WimReader.h"

#include <functional>
#include <mutex>

namespace peare {
namespace fs {

class AcronisRestoredStore final : public IByteStore, public IWofResourceResolver {
public:
    typedef std::function<bool(ByteStorePtr*)> FilterLoader;
    AcronisRestoredStore(ByteStorePtr source, FilterLoader loadFilter,
                         bool reconstructWithoutFilter = false);
    std::int64_t capacity() const override;
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;
    bool validate() const override;
    bool cheapRandomAccess() const override { return false; }
    const ByteStorePtr& original() const { return source_; }
    ByteStorePtr resolveExternalWofResource(const std::string& path,
                                             const WofWimReference& reference) const override;
private:
    void initialize() const;
    ByteStorePtr source_;
    FilterLoader loadFilter_;
    bool reconstructWithoutFilter_;
    mutable std::once_flag once_;
    mutable ByteStorePtr view_;
};

} // namespace fs
} // namespace peare
