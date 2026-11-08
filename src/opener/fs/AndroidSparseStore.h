#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace fs {

class AndroidSparseStore final : public IByteStore {
public:
    static std::shared_ptr<AndroidSparseStore> open(
        const ByteStorePtr& source, std::string* error = nullptr);

    std::int64_t capacity() const override { return outputSize_; }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override;
    bool validate() const override;

private:
    enum class ChunkKind { Raw, Fill, Zero };
    struct Region {
        ChunkKind kind;
        std::int64_t outputOffset;
        std::int64_t length;
        std::int64_t sourceOffset;
        std::uint32_t fillValue;
    };
    struct Checksum {
        std::int64_t outputOffset;
        std::uint32_t value;
    };

    explicit AndroidSparseStore(ByteStorePtr source) : source_(std::move(source)) {}
    bool parse(std::string* error);

    ByteStorePtr source_;
    std::vector<Region> regions_;
    std::vector<Checksum> checksums_;
    std::int64_t outputSize_ = 0;
    std::uint32_t imageChecksum_ = 0;
};

}  // namespace fs
}  // namespace peare
