#pragma once

// UDIF/DMG run-backed byte store. The DMG module parses the XML resource fork
// and passes blkx run tables here; this store presents the uncompressed sector
// range lazily.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "DiscStore.h"

namespace peare {
namespace fs {

struct DmgRun {
    std::uint32_t type = 0;
    std::int64_t sectorStart = 0;
    std::int64_t sectorCount = 0;
    std::int64_t compOffset = 0;
    std::int64_t compLength = 0;
};

class DmgRunStore final : public IByteStore {
public:
    DmgRunStore(ByteStorePtr source, std::int64_t sectorCount,
                std::vector<DmgRun> runs);

    std::int64_t capacity() const override { return sectorCount_ * 512; }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;
    bool valid() const { return valid_; }
    std::string error() const { return error_; }

private:
    bool loadRun(std::size_t index) const;

    ByteStorePtr source_;
    std::int64_t sectorCount_ = 0;
    std::vector<DmgRun> runs_;
    bool valid_ = false;
    std::string error_;

    mutable std::map<std::size_t, std::vector<std::uint8_t> > cache_;
};

}  // namespace fs
}  // namespace peare
