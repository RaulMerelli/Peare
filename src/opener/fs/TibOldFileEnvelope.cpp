#include "TibOldFileEnvelope.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace peare {
namespace fs {
namespace {

std::uint32_t readLe32(const std::uint8_t* data) {
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* data) {
    return std::uint64_t(readLe32(data)) | (std::uint64_t(readLe32(data + 4)) << 32);
}

} // namespace

TibOldFileEnvelope::TibOldFileEnvelope(TibOldEnvelopeSink& sink)
    : sink_(sink), state_(State::EnvelopeHeader), failure_(TibOldEnvelopeStatus::NeedMore),
      headerSize_(0), remaining_(0), offset_(0) {}

TibOldEnvelopeStatus TibOldFileEnvelope::fail(TibOldEnvelopeStatus status) {
    state_ = State::Failed;
    failure_ = status;
    return status;
}

TibOldEnvelopeStatus TibOldFileEnvelope::push(const std::uint8_t* data, std::size_t size) {
    if (state_ == State::Failed) return failure_;
    if (state_ == State::Done) return TibOldEnvelopeStatus::Complete;
    if (!data && size != 0) return fail(TibOldEnvelopeStatus::InvalidArgument);

    while (size != 0) {
        if (state_ == State::EnvelopeHeader || state_ == State::StreamHeader) {
            const std::size_t count = std::min(size, sizeof(header_) - headerSize_);
            if (count > std::numeric_limits<std::uint64_t>::max() - offset_)
                return fail(TibOldEnvelopeStatus::Overflow);
            std::memcpy(header_ + headerSize_, data, count);
            headerSize_ += count;
            offset_ += count;
            data += count;
            size -= count;
            if (headerSize_ != sizeof(header_)) continue;

            headerSize_ = 0;
            if (state_ == State::EnvelopeHeader) {
                if (readLe32(header_) != 3 || readLe32(header_ + 4) != 2)
                    return fail(TibOldEnvelopeStatus::InvalidVersion);
                remaining_ = readLe64(header_ + 8);
                if (remaining_ > std::numeric_limits<std::uint64_t>::max() - offset_)
                    return fail(TibOldEnvelopeStatus::Overflow);
                state_ = remaining_ ? State::Metadata : State::StreamHeader;
            } else {
                const std::uint32_t type = readLe32(header_);
                remaining_ = readLe64(header_ + 8);
                if (remaining_ > std::numeric_limits<std::uint64_t>::max() - offset_)
                    return fail(TibOldEnvelopeStatus::Overflow);
                if (type == 1) {
                    if (!sink_.primaryLength(remaining_))
                        return fail(TibOldEnvelopeStatus::CallbackStopped);
                    if (remaining_ == 0) {
                        state_ = State::Done;
                        return TibOldEnvelopeStatus::Complete;
                    }
                    state_ = State::Primary;
                } else {
                    state_ = remaining_ ? State::SkipStream : State::StreamHeader;
                }
            }
            continue;
        }

        const std::size_t count = remaining_ < size
            ? static_cast<std::size_t>(remaining_) : size;
        if (state_ == State::Primary && !sink_.primaryData(data, count))
            return fail(TibOldEnvelopeStatus::CallbackStopped);
        remaining_ -= count;
        offset_ += count;
        data += count;
        size -= count;
        if (remaining_ == 0) {
            if (state_ == State::Primary) {
                state_ = State::Done;
                return TibOldEnvelopeStatus::Complete;
            }
            state_ = State::StreamHeader;
        }
    }
    return TibOldEnvelopeStatus::NeedMore;
}

TibOldEnvelopeStatus TibOldFileEnvelope::finish() {
    if (state_ == State::Done) return TibOldEnvelopeStatus::Complete;
    if (state_ == State::Failed) return failure_;
    if (state_ == State::StreamHeader && headerSize_ == 0)
        return fail(TibOldEnvelopeStatus::MissingPrimary);
    return fail(TibOldEnvelopeStatus::Truncated);
}

} // namespace fs
} // namespace peare
