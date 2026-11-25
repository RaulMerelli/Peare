#include "TibRandomAccessVolume.h"
#include "../../common/compression/DeflateDecoder.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace peare {
namespace fs {
namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t adler(const std::uint8_t* p, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    while (n) {
        const std::size_t chunk = std::min<std::size_t>(n, 5552);
        for (std::size_t i = 0; i < chunk; ++i) { a += p[i]; b += a; }
        a %= 65521; b %= 65521;
        p += chunk; n -= chunk;
    }
    return (b << 16) | a;
}

bool exact(const ByteStorePtr& source, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    if (!source || source->capacity() < 0 || offset > std::uint64_t(source->capacity()) ||
        size > std::uint64_t(source->capacity()) - offset) return false;
    while (size) {
        const int count = static_cast<int>(std::min<std::size_t>(size, 1024 * 1024));
        if (!source->readExactlyChecked(static_cast<std::int64_t>(offset), data, count)) return false;
        offset += count; data += count; size -= count;
    }
    return source->validate();
}

} // namespace

TibRandomAccessVolume::TibRandomAccessVolume(ByteStorePtr storage, ByteStorePtr direct,
        std::shared_ptr<TibRandomAccessIndex> index, std::uint32_t pageSize,
        std::uint64_t length, std::uint64_t readableLength, TibRandomAccessGeometry geometry)
    : storage_(std::move(storage)), direct_(std::move(direct)), index_(std::move(index)),
      pageSize_(pageSize), length_(length), readableLength_(std::min(length, readableLength)),
      geometry_(geometry), cachedGroup_(std::numeric_limits<std::uint64_t>::max()) {}

std::int64_t TibRandomAccessVolume::capacity() const {
    return length_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(length_) : -1;
}

bool TibRandomAccessVolume::validate() const {
    return storage_ && direct_ && index_ && index_->valid() && storage_->validate() &&
        direct_->validate() && capacity() >= 0 && pageSize_ && geometry_.sectorBytes &&
        geometry_.sectorsPerBit && geometry_.bitsPerGroup && geometry_.sectorsPerStoredPage &&
        geometry_.indexEntriesPerGroup;
}

bool TibRandomAccessVolume::loadGroup(std::uint64_t group) const {
    if (group == cachedGroup_) return true;
    const std::uint64_t key = group * geometry_.indexEntriesPerGroup;
    if (group && key / group != geometry_.indexEntriesPerGroup) return false;
    if (key > std::numeric_limits<std::uint32_t>::max()) return false;
    std::uint32_t ref = 0;
    const auto result = index_->reference(static_cast<std::uint32_t>(key), &ref);
    const std::uint64_t bitmapSize64 = (std::uint64_t(geometry_.bitsPerGroup) + 7) / 8;
    if (bitmapSize64 > std::vector<std::uint8_t>().max_size()) return false;
    const std::size_t bitmapSize = static_cast<std::size_t>(bitmapSize64);
    if (result == TibRandomAccessIndex::Result::Absent) {
        bitmap_.clear(); bitmapRanks_.clear(); data_.clear(); cachedGroup_ = group; return true;
    }
    if (result != TibRandomAccessIndex::Result::Ok) return false;
    std::vector<std::uint8_t> bitmap(bitmapSize, 0), decoded;
    std::vector<std::uint8_t> input;
    std::size_t dataStart = 0;
    if (ref == 0x7ffffffeU) {
        TibRandomAccessIndex::ExternalReference ext;
        if (index_->external(static_cast<std::uint32_t>(key), &ext) != TibRandomAccessIndex::Result::Ok ||
            ext.length < bitmapSize || ext.length > input.max_size() || direct_->capacity() < 0 ||
            ext.offset > std::uint64_t(direct_->capacity()) ||
            ext.length > std::uint64_t(direct_->capacity()) - ext.offset) return false;
        input.resize(ext.length);
        if (!exact(direct_, ext.offset, input.data(), input.size())) return false;
    } else {
        const std::uint64_t stride = std::uint64_t(geometry_.sectorsPerStoredPage) * geometry_.sectorBytes;
        std::uint64_t total = 0;
        for (std::uint32_t i = 0; i < geometry_.indexEntriesPerGroup; ++i) {
            if (key + i > std::numeric_limits<std::uint32_t>::max()) return false;
            if (i) {
                const auto next = index_->reference(static_cast<std::uint32_t>(key + i), &ref);
                if (next == TibRandomAccessIndex::Result::Absent) break;
                if (next != TibRandomAccessIndex::Result::Ok || ref == 0x7ffffffeU) return false;
            }
            if (total > input.max_size() || pageSize_ > input.max_size() - total) return false;
            const std::uint64_t required = total + pageSize_;
            if (input.size() < required) input.resize(static_cast<std::size_t>(required), 0);
            if (!exact(storage_, std::uint64_t(ref) * pageSize_, input.data() + total, pageSize_)) return false;
            if (stride > std::numeric_limits<std::uint64_t>::max() - total) return false;
            total += stride;
            if (total > input.max_size()) return false;
            if (input.size() < total) input.resize(static_cast<std::size_t>(total), 0);
        }
        if (total < 4 || total > input.size()) return false;
        const std::uint32_t padding = le32(input.data());
        if (padding > total - 4) return false;
        input.resize(static_cast<std::size_t>(total - padding));
        dataStart = 4;
    }
    if (input.size() - dataStart < bitmapSize) return false;
    std::copy(input.begin() + dataStart, input.begin() + dataStart + bitmapSize, bitmap.begin());
    dataStart += bitmapSize;
    if (dataStart == input.size()) {
        std::fill(bitmap.begin(), bitmap.end(), 0);
    } else {
        std::uint64_t selected = 0;
        for (std::uint32_t i = 0; i < geometry_.bitsPerGroup; ++i)
            if (bitmap[i / 8] & (1U << (i % 8))) ++selected;
        const std::uint64_t unit = std::uint64_t(geometry_.sectorsPerBit) * geometry_.sectorBytes;
        if (selected && unit > decoded.max_size() / selected) return false;
        const std::size_t expected = static_cast<std::size_t>(selected * unit);
        const std::size_t packed = input.size() - dataStart;
        if (geometry_.rawCodec) {
            if (packed < 4 || packed - 4 != expected ||
                adler(input.data() + dataStart, expected) != le32(input.data() + input.size() - 4)) return false;
            decoded.assign(input.begin() + dataStart, input.end() - 4);
        } else {
            compression::MemoryInflateInput encoded(input.data() + dataStart, packed);
            if (!compression::inflateZlibStreamExact(encoded, expected, &decoded) || encoded.consumed() != packed)
                return false;
        }
    }
    std::vector<std::uint32_t> ranks(bitmap.size() + 1, 0);
    for (std::size_t byte = 0; byte < bitmap.size(); ++byte) {
        std::uint32_t count = ranks[byte];
        for (unsigned bit = 0; bit < 8; ++bit) {
            if (std::uint64_t(byte) * 8 + bit >= geometry_.bitsPerGroup) break;
            if (bitmap[byte] & (1U << bit)) ++count;
        }
        ranks[byte + 1] = count;
    }
    bitmap_.swap(bitmap); bitmapRanks_.swap(ranks); data_.swap(decoded); cachedGroup_ = group;
    return true;
}

int TibRandomAccessVolume::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (!dst || pos < 0 || count <= 0 || std::uint64_t(pos) >= length_ || !validate()) return 0;
    const std::size_t amount = static_cast<std::size_t>(std::min<std::uint64_t>(
        std::min<std::uint64_t>(count, length_ - std::uint64_t(pos)), 1024 * 1024));
    if (std::uint64_t(pos) >= readableLength_ || amount > readableLength_ - std::uint64_t(pos)) return -1;
    const std::uint64_t unit = std::uint64_t(geometry_.sectorBytes) * geometry_.sectorsPerBit;
    if (geometry_.bitsPerGroup > std::numeric_limits<std::uint64_t>::max() / unit ||
        geometry_.sectorPrefix > std::numeric_limits<std::uint64_t>::max() / geometry_.sectorBytes) return -1;
    const std::uint64_t groupSize = unit * geometry_.bitsPerGroup;
    const std::uint64_t prefix = geometry_.sectorPrefix * geometry_.sectorBytes;
    if (prefix > std::numeric_limits<std::uint64_t>::max() - amount ||
        std::uint64_t(pos) > std::numeric_limits<std::uint64_t>::max() - prefix - amount) return -1;
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        std::vector<std::uint8_t> output(amount, 0);
        std::size_t done = 0;
        while (done < amount) {
            const std::uint64_t logical = std::uint64_t(pos) + prefix + done;
            const std::uint64_t group = logical / groupSize, within = logical % groupSize;
            if (!loadGroup(group)) return -1;
            const std::uint64_t bit = within / unit, offset = within % unit;
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(amount - done, unit - offset));
            if (!bitmap_.empty() && (bitmap_[bit / 8] & (1U << (bit % 8)))) {
                std::uint64_t rank = bitmapRanks_[bit / 8];
                for (unsigned i = 0; i < bit % 8; ++i)
                    if (bitmap_[bit / 8] & (1U << i)) ++rank;
                const std::uint64_t source = rank * unit + offset;
                if (source > data_.size() || n > data_.size() - source) return -1;
                std::memcpy(output.data() + done, data_.data() + source, n);
            }
            done += n;
        }
        if (!validate()) return -1;
        std::memcpy(dst, output.data(), amount);
        return static_cast<int>(amount);
    } catch (const std::exception&) { return -1; }
}

} // namespace fs
} // namespace peare
