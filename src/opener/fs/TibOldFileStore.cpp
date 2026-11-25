#include "TibOldFileStore.h"

#include "TibOldFileEnvelope.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace peare {
namespace fs {
namespace {

class WindowSink final : public TibOldFieldSink, public TibOldEnvelopeSink {
public:
    WindowSink(std::uint64_t length, std::uint64_t start,
               std::vector<std::uint8_t>& window)
        : envelope_(*this), length_(length), start_(start), window_(window),
          primaryOffset_(0), copied_(0), lengthSeen_(false), verified_(false) {}

    bool beginField(const TibOldField& field) override { return field.tag == 0x02; }

    bool fieldData(const std::uint8_t* data, std::size_t size) override {
        const TibOldEnvelopeStatus status = envelope_.push(data, size);
        return status == TibOldEnvelopeStatus::NeedMore ||
               status == TibOldEnvelopeStatus::Complete;
    }

    void endField(const TibOldField&, bool verified) override { verified_ = verified; }

    bool primaryLength(std::uint64_t length) override {
        if (lengthSeen_ || length != length_) return false;
        lengthSeen_ = true;
        return true;
    }

    bool primaryData(const std::uint8_t* data, std::size_t size) override {
        if (!lengthSeen_ || primaryOffset_ > length_ || size > length_ - primaryOffset_)
            return false;
        const std::uint64_t end = primaryOffset_ + size;
        const std::uint64_t windowEnd = start_ + window_.size();
        const std::uint64_t first = std::max(primaryOffset_, start_);
        const std::uint64_t last = std::min(end, windowEnd);
        if (first < last) {
            const std::size_t source = static_cast<std::size_t>(first - primaryOffset_);
            const std::size_t target = static_cast<std::size_t>(first - start_);
            const std::size_t count = static_cast<std::size_t>(last - first);
            std::memcpy(window_.data() + target, data + source, count);
            copied_ += count;
        }
        primaryOffset_ = end;
        return true;
    }

    bool complete() {
        return verified_ && lengthSeen_ && primaryOffset_ == length_ &&
               copied_ == window_.size() &&
               envelope_.finish() == TibOldEnvelopeStatus::Complete;
    }

private:
    TibOldFileEnvelope envelope_;
    std::uint64_t length_;
    std::uint64_t start_;
    std::vector<std::uint8_t>& window_;
    std::uint64_t primaryOffset_;
    std::size_t copied_;
    bool lengthSeen_;
    bool verified_;
};

} // namespace

TibOldFileStore::TibOldFileStore(const std::vector<ByteStorePtr>& volumes,
                                 const TibOldFieldPosition& fieldPosition,
                                 std::uint64_t primaryLength,
                                 const TibOldFieldLimits& limits)
    : volumes_(volumes), fieldPosition_(fieldPosition), primaryLength_(primaryLength),
      limits_(limits), valid_(true) {
    if (volumes_.empty() || fieldPosition_.volumeIndex >= volumes_.size() ||
        primaryLength_ > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        valid_ = false;
    capacities_.reserve(volumes_.size());
    for (std::size_t i = 0; i < volumes_.size(); ++i) {
        const std::int64_t size = volumes_[i] ? volumes_[i]->capacity() : -1;
        capacities_.push_back(size);
        if (size < 12) valid_ = false;
    }
    if (valid_ && (fieldPosition_.volumeOffset < 12 ||
                   fieldPosition_.volumeOffset >= static_cast<std::uint64_t>(
                       capacities_[fieldPosition_.volumeIndex])))
        valid_ = false;
}

std::int64_t TibOldFileStore::capacity() const {
    return valid_ ? static_cast<std::int64_t>(primaryLength_) : -1;
}

bool TibOldFileStore::validate() const {
    if (!valid_) return false;
    for (std::size_t i = 0; i < volumes_.size(); ++i) {
        if (!volumes_[i] || volumes_[i]->capacity() != capacities_[i] ||
            !volumes_[i]->validate()) return false;
    }
    return true;
}

int TibOldFileStore::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (count <= 0) return 0;
    if (pos < 0 || !dst || !validate()) return -1;
    if (static_cast<std::uint64_t>(pos) >= primaryLength_) return 0;
    const std::uint64_t available = primaryLength_ - static_cast<std::uint64_t>(pos);
    const std::size_t wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
        available, std::min<std::uint64_t>(static_cast<unsigned>(count), 1024U * 1024U)));
    try {
        std::vector<std::uint8_t> window(wanted);
        WindowSink sink(primaryLength_, static_cast<std::uint64_t>(pos), window);
        if (readTibOldFileFieldAt(volumes_, fieldPosition_, limits_, &sink) != TibOldFieldStatus::Ok ||
            !sink.complete() || !validate()) return -1;
        std::memcpy(dst, window.data(), wanted);
        return static_cast<int>(wanted);
    } catch (const std::bad_alloc&) {
        return -1;
    } catch (const std::length_error&) {
        return -1;
    }
}

} // namespace fs
} // namespace peare
