#pragma once

#include "DiscStore.h"

namespace peare {
namespace fs {

constexpr std::int64_t tibCbcCompleteSize(std::int64_t size) {
    return size < 0 ? 0 : size - size % 16;
}

constexpr bool tibCbcReadableOffset(std::int64_t pos, std::int64_t size) {
    return pos >= 16 && pos < tibCbcCompleteSize(size);
}

static_assert(tibCbcCompleteSize(-1) == 0 && tibCbcCompleteSize(15) == 0,
              "Incomplete CBC blocks are not readable");
static_assert(tibCbcCompleteSize(16) == 16 && tibCbcCompleteSize(85) == 80,
              "CBC capacity must end at a complete block");
static_assert(!tibCbcReadableOffset(15, 85) && tibCbcReadableOffset(16, 85) &&
              tibCbcReadableOffset(79, 85) && !tibCbcReadableOffset(80, 85),
              "CBC readable offsets exclude the IV and incomplete tail");

// CBC plaintext is available only after the IV and through complete blocks.
class TibCbcBounds final : public IByteStore {
public:
    explicit TibCbcBounds(ByteStorePtr source)
        : source_(std::move(source)), sourceSize_(source_ ? source_->capacity() : -1),
          size_(tibCbcCompleteSize(sourceSize_)) {}

    std::int64_t capacity() const override { return size_; }
    bool validate() const override {
        return source_ && sourceSize_ >= 0 && source_->capacity() == sourceSize_ && source_->validate();
    }
    bool cheapRandomAccess() const override { return source_ && source_->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || (count > 0 && !dst)) return -1;
        if (pos < 0 || count <= 0 || pos >= size_) return 0;
        if (!tibCbcReadableOffset(pos, sourceSize_)) return -1;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
        return source_->read(pos, dst, amount);
    }

private:
    ByteStorePtr source_;
    std::int64_t sourceSize_;
    std::int64_t size_;
};

}  // namespace fs
}  // namespace peare
