#pragma once

#include "TibOldFileFields.h"

#include <cstdint>
#include <vector>

namespace peare {
namespace fs {

// Positioned reads verify the complete field and envelope before copying data.
// Each read decodes the field again and returns at most 1 MiB. The caller must
// verify indexed fields, including empty files, before constructing the store.
// validate() checks volumes and capacities, not same-size content changes.
class TibOldFileStore final : public IByteStore {
public:
    TibOldFileStore(const std::vector<ByteStorePtr>& volumes,
                    const TibOldFieldPosition& fieldPosition,
                    std::uint64_t primaryLength,
                    const TibOldFieldLimits& limits = TibOldFieldLimits());

    std::int64_t capacity() const override;
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override;

private:
    std::vector<ByteStorePtr> volumes_;
    std::vector<std::int64_t> capacities_;
    TibOldFieldPosition fieldPosition_;
    std::uint64_t primaryLength_;
    TibOldFieldLimits limits_;
    bool valid_;
};

} // namespace fs
} // namespace peare
