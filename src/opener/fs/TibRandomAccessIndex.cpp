#include "TibRandomAccessIndex.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kAbsent = 0x7fffffffU;
const std::uint32_t kExternal = 0x7ffffffeU;

std::uint32_t word(const std::vector<std::uint8_t>& page, std::size_t index) {
    const std::size_t p = index * 4;
    return static_cast<std::uint32_t>(page[p]) |
        (static_cast<std::uint32_t>(page[p + 1]) << 8) |
        (static_cast<std::uint32_t>(page[p + 2]) << 16) |
        (static_cast<std::uint32_t>(page[p + 3]) << 24);
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t a = 1, b = 0;
    while (size) {
        const std::size_t count = std::min<std::size_t>(size, 5552);
        for (std::size_t i = 0; i < count; ++i) {
            a += data[i];
            b += a;
        }
        a %= 65521;
        b %= 65521;
        data += count;
        size -= count;
    }
    return (b << 16) | a;
}

} // namespace

TibRandomAccessIndex::TibRandomAccessIndex(std::uint32_t pageSize, std::uint32_t tag06,
                                           std::uint32_t tag14, bool hasRootD2, std::uint32_t rootD2,
                                           bool hasRoot9C, std::uint32_t root9C, PageResolver resolver)
    : pageSize_(pageSize), count_(0), fanout_(0), root_(0),
      resolver_(std::move(resolver)), valid_(false),
      cacheBytes_(0) {
    const std::uint32_t count = static_cast<std::uint32_t>(std::uint64_t(tag06) * tag14);
    if (pageSize < 12 || (pageSize & 3) ||
        (!hasRootD2 && !hasRoot9C) || !resolver_)
        return;
    fanout_ = (pageSize - 4) / 4;
    count_ = count;
    std::vector<std::uint8_t> rootPage;
    Result result = hasRootD2 ? readPage(rootD2, &rootPage) : Result::IoError;
    root_ = rootD2;
    if (hasRoot9C && (result == Result::IoError || result == Result::ChecksumError)) {
        result = readPage(root9C, &rootPage);
        root_ = root9C;
    }
    valid_ = result == Result::Ok;
}

TibRandomAccessIndex::Result TibRandomAccessIndex::readPage(
        std::uint32_t reference, std::vector<std::uint8_t>* page) const {
    try {
        ByteStorePtr store = resolver_(reference);
        if (!store || store->capacity() < 0 ||
            static_cast<std::uint64_t>(store->capacity()) < pageSize_ || !store->validate())
            return Result::IoError;
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->first != reference) continue;
                *page = it->second;
                cache_.splice(cache_.begin(), cache_, it);
                return Result::Ok;
            }
        }
        page->resize(pageSize_);
        std::size_t done = 0;
        while (done < page->size()) {
            const int count = static_cast<int>(std::min<std::size_t>(page->size() - done, 1024 * 1024));
            if (!store->readExactlyChecked(static_cast<std::int64_t>(done), page->data() + done, count))
                return Result::IoError;
            done += static_cast<std::size_t>(count);
        }
        if (!store->validate()) return Result::IoError;
        if (word(*page, fanout_) != adler32(page->data(), pageSize_ - 4)) return Result::ChecksumError;
        const std::size_t budget = 8U * 1024U * 1024U;
        if (pageSize_ <= budget) {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            bool present = false;
            for (auto it = cache_.begin(); it != cache_.end(); ++it) {
                if (it->first == reference) { cache_.splice(cache_.begin(), cache_, it); present = true; break; }
            }
            if (!present) {
                cache_.push_front(std::make_pair(reference, *page));
                cacheBytes_ += pageSize_;
                while (cacheBytes_ > budget) {
                    cacheBytes_ -= cache_.back().second.size();
                    cache_.pop_back();
                }
            }
        }
        return Result::Ok;
    } catch (const std::bad_alloc&) {
        return Result::ResourceError;
    } catch (const std::length_error&) {
        return Result::ResourceError;
    } catch (...) {
        return Result::IoError;
    }
}

TibRandomAccessIndex::Result TibRandomAccessIndex::leaf(
        std::uint32_t index, std::vector<std::uint8_t>* page) const {
    if (!valid_ || !page || index >= count_) return Result::Invalid;
    Result result = readPage(root_, page);
    if (result != Result::Ok) return result;
    std::uint64_t divisor = 1;
    for (std::uint32_t n = count_; n >= fanout_; n /= fanout_) divisor *= fanout_;
    while (divisor > 1) {
        const std::size_t slot = static_cast<std::size_t>((index / divisor) % fanout_);
        const std::uint32_t reference = word(*page, slot) & kAbsent;
        if (reference == kAbsent) return Result::Absent;
        result = readPage(reference, page);
        if (result != Result::Ok) return result;
        divisor /= fanout_;
    }
    return Result::Ok;
}

TibRandomAccessIndex::Result TibRandomAccessIndex::raw(std::uint32_t index, std::uint32_t* value) const {
    if (!value) return Result::Invalid;
    std::vector<std::uint8_t> page;
    const Result result = leaf(index, &page);
    if (result == Result::Ok) *value = word(page, index % fanout_);
    return result;
}

TibRandomAccessIndex::Result TibRandomAccessIndex::reference(std::uint32_t index, std::uint32_t* value) const {
    if (!value) return Result::Invalid;
    std::uint32_t rawValue = 0;
    const Result result = raw(index, &rawValue);
    if (result != Result::Ok) return result;
    rawValue &= kAbsent;
    if (rawValue == kAbsent) return Result::Absent;
    *value = rawValue;
    return Result::Ok;
}

TibRandomAccessIndex::Result TibRandomAccessIndex::external(std::uint32_t index, ExternalReference* value) const {
    if (!value) return Result::Invalid;
    std::uint32_t marker = 0;
    Result result = reference(index, &marker);
    if (result != Result::Ok) return result;
    if (marker != kExternal) return Result::Absent;
    if (index > std::numeric_limits<std::uint32_t>::max() - 3 ||
        index >= count_ || count_ - index <= 3) return Result::Invalid;
    std::uint32_t high = 0, low = 0, length = 0;
    result = raw(index + 1, &high);
    if (result != Result::Ok) return result;
    result = raw(index + 2, &low);
    if (result != Result::Ok) return result;
    result = raw(index + 3, &length);
    if (result != Result::Ok) return result;
    ExternalReference external;
    external.offset = (static_cast<std::uint64_t>(high) << 32) | low;
    external.length = length;
    if (external.offset > std::numeric_limits<std::uint64_t>::max() - external.length) return Result::Invalid;
    *value = external;
    return Result::Ok;
}

} // namespace fs
} // namespace peare
