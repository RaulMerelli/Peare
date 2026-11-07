#include "QuantumCabDecoder.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace peare {
namespace compression {
namespace {

class RangeDecoder {
public:
    bool initialize(const std::uint8_t* data, std::size_t size)
    {
        if (!data || size < 2) return false;
        data_ = data;
        size_ = size;
        code_ = (std::uint32_t(data[0]) << 8) | data[1];
        bitPosition_ = 0;
        low_ = 0;
        range_ = 0x10000U;
        return true;
    }

    bool readBits(unsigned count, std::uint32_t* value)
    {
        if (!value || count > 32) return false;
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t byteOffset = 2 + bitPosition_ / 8;
            if (byteOffset >= size_) return false;
            const unsigned shift = 7U - unsigned(bitPosition_ & 7U);
            result = (result << 1) | ((data_[byteOffset] >> shift) & 1U);
            ++bitPosition_;
        }
        *value = result;
        return true;
    }

    bool threshold(std::uint32_t total, std::uint32_t* value) const
    {
        if (!total || !range_ || !value) return false;
        *value = ((code_ + 1U) * total - 1U) / range_;
        return *value < total;
    }

    bool decode(std::uint32_t start, std::uint32_t end, std::uint32_t total)
    {
        if (!total || start >= end || end > total || !range_) return false;
        std::uint32_t high = 0U - (low_ + (end * range_) / total);
        const std::uint32_t offset = (start * range_) / total;
        std::uint32_t low = low_ + offset;
        code_ -= offset;
        unsigned bits = 0;
        low ^= high;
        while (low & 0x8000U) {
            low <<= 1;
            high <<= 1;
            ++bits;
        }
        low ^= high;
        std::uint32_t common = low & high;
        while (common & 0x4000U) {
            common <<= 1;
            low <<= 1;
            high <<= 1;
            ++bits;
        }
        low_ = low;
        range_ = ((~high - low) & 0xffffU) + 1U;
        if (bits) {
            std::uint32_t next = 0;
            if (!readBits(bits, &next)) return false;
            code_ = (code_ << bits) + next;
        }
        return true;
    }

    bool finish()
    {
        const unsigned offset = unsigned(bitPosition_ & 7U);
        const unsigned count = 2U + ((14U - offset) & 7U);
        std::uint32_t padding = 0;
        if (!readBits(count, &padding) || padding) return false;
        return 2 + (bitPosition_ + 7) / 8 == size_;
    }

private:
    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t bitPosition_ = 0;
    std::uint32_t low_ = 0;
    std::uint32_t range_ = 0;
    std::uint32_t code_ = 0;
};

}  // namespace

class QuantumCabDecoder::Model {
public:
    void initialize(unsigned count, unsigned firstValue)
    {
        count_ = count;
        reorder_ = 4;
        cumulative_[count] = 0;
        for (unsigned i = 0; i < count; ++i) {
            cumulative_[i] = std::uint16_t(count - i);
            values_[i] = std::uint8_t(firstValue + i);
        }
    }

    bool decode(RangeDecoder* range, unsigned* value)
    {
        if (!range || !value || !count_) return false;
        if (cumulative_[0] > 3800) rescale();
        const unsigned total = cumulative_[0];
        std::uint32_t threshold = 0;
        if (!range->threshold(total, &threshold)) return false;
        unsigned symbol = 0;
        while (symbol + 1 < count_ && cumulative_[symbol + 1] > threshold)
            ++symbol;
        const unsigned lower = cumulative_[symbol + 1];
        const unsigned upper = cumulative_[symbol];
        for (unsigned i = 0; i <= symbol; ++i)
            cumulative_[i] = std::uint16_t(cumulative_[i] + 8);
        *value = values_[symbol];
        return range->decode(lower, upper, total);
    }

private:
    void rescale()
    {
        if (--reorder_ == 0) {
            reorder_ = 50;
            std::array<unsigned, 64> weights = {};
            unsigned next = 0;
            for (unsigned i = count_; i > 0; --i) {
                const unsigned current = cumulative_[i - 1];
                weights[i - 1] = (current - next + 1) >> 1;
                next = current;
            }
            for (unsigned i = 0; i + 1 < count_; ++i) {
                for (unsigned j = i + 1; j < count_; ++j) {
                    if (weights[i] < weights[j]) {
                        std::swap(weights[i], weights[j]);
                        std::swap(values_[i], values_[j]);
                    }
                }
            }
            unsigned sum = 0;
            cumulative_[count_] = 0;
            for (unsigned i = count_; i > 0; --i) {
                sum += weights[i - 1];
                cumulative_[i - 1] = std::uint16_t(sum);
            }
            return;
        }
        unsigned next = 1;
        for (unsigned i = count_; i > 0; --i) {
            unsigned frequency = cumulative_[i - 1] >> 1;
            if (frequency < next) frequency = next;
            cumulative_[i - 1] = std::uint16_t(frequency);
            next = frequency + 1;
        }
    }

    unsigned count_ = 0;
    unsigned reorder_ = 4;
    std::array<std::uint16_t, 65> cumulative_ = {};
    std::array<std::uint8_t, 64> values_ = {};
};

QuantumCabDecoder::QuantumCabDecoder() = default;
QuantumCabDecoder::~QuantumCabDecoder() = default;

void QuantumCabDecoder::resetModels()
{
    selector_->initialize(7, 0);
    for (unsigned i = 0; i < 4; ++i)
        literals_[i].initialize(64, i * 64);
    const unsigned dictionarySlots = windowBits_ ? windowBits_ * 2 : 1;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned slots = 24 + i * 6 + (((i + 1) & 2) * 3);
        positions_[i].initialize(std::min(dictionarySlots, slots), 0);
    }
    lengths_->initialize(27, 5);
}

bool QuantumCabDecoder::configure(unsigned windowBits)
{
    if (windowBits < 10 || windowBits > 21) return false;
    try {
        windowBits_ = windowBits;
        windowSize_ = std::size_t(1) << std::max(windowBits, 15U);
        window_.assign(windowSize_, 0);
        std::unique_ptr<Model> selector(new Model);
        std::unique_ptr<Model[]> literals(new Model[4]);
        std::unique_ptr<Model[]> positions(new Model[3]);
        std::unique_ptr<Model> lengths(new Model);
        selector_ = std::move(selector);
        literals_ = std::move(literals);
        positions_ = std::move(positions);
        lengths_ = std::move(lengths);
        windowPosition_ = 0;
        historySize_ = 0;
        ready_ = true;
        return true;
    } catch (...) {
        ready_ = false;
        return false;
    }
}

bool QuantumCabDecoder::decodeBlock(const std::uint8_t* input,
                                    std::size_t inputSize,
                                    std::size_t outputSize,
                                    bool keepHistory,
                                    std::vector<std::uint8_t>* output)
{
    if (!ready_ || !output || (!input && inputSize) || outputSize > 32768)
        return false;
    try {
        if (!keepHistory) {
            windowPosition_ = 0;
            historySize_ = 0;
            resetModels();
        }
        RangeDecoder range;
        if (!range.initialize(input, inputSize)) return false;
        output->clear();
        output->reserve(outputSize);
        while (output->size() < outputSize) {
            unsigned selector = 0;
            if (!selector_->decode(&range, &selector) || selector > 6) return false;
            if (selector < 4) {
                unsigned literal = 0;
                if (!literals_[selector].decode(&range, &literal)) return false;
                const std::uint8_t byte = std::uint8_t(literal);
                output->push_back(byte);
                window_[windowPosition_] = byte;
                windowPosition_ = (windowPosition_ + 1) % windowSize_;
                historySize_ = std::min(windowSize_, historySize_ + 1);
                continue;
            }

            std::size_t length = selector - 4 + 3;
            if (selector == 6) {
                unsigned slot = 0;
                if (!lengths_->decode(&range, &slot) || slot < 5) return false;
                length = slot;
                if (slot >= 11) {
                    const unsigned code = slot - 7;
                    const unsigned directBits = code >> 2;
                    if (directBits > 24) return false;
                    const unsigned prefix = 4U | (code & 3U);
                    std::uint64_t expanded =
                        (std::uint64_t(prefix) << directBits) - 8 + 11;
                    if (directBits < 6) {
                        std::uint32_t extra = 0;
                        if (!range.readBits(directBits, &extra)) return false;
                        expanded += extra;
                    }
                    if (expanded > outputSize) return false;
                    length = std::size_t(expanded);
                }
            }
            if (length > outputSize - output->size()) return false;

            unsigned distanceSlot = 0;
            if (!positions_[selector - 4].decode(&range, &distanceSlot)) return false;
            std::uint64_t distance = distanceSlot;
            if (distanceSlot >= 4) {
                const unsigned directBits = (distanceSlot >> 1) - 1;
                if (directBits > 20) return false;
                std::uint32_t extra = 0;
                if (!range.readBits(directBits, &extra)) return false;
                distance = ((2U | (distanceSlot & 1U)) << directBits) + extra;
            }
            if (distance >= historySize_) return false;

            for (std::size_t i = 0; i < length; ++i) {
                const std::size_t source = (windowPosition_ + windowSize_ -
                                            std::size_t(distance) - 1) % windowSize_;
                const std::uint8_t byte = window_[source];
                output->push_back(byte);
                window_[windowPosition_] = byte;
                windowPosition_ = (windowPosition_ + 1) % windowSize_;
                historySize_ = std::min(windowSize_, historySize_ + 1);
            }
        }
        return range.finish();
    } catch (...) {
        output->clear();
        return false;
    }
}

}  // namespace compression
}  // namespace peare
