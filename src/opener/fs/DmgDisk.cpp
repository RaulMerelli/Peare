#include "DmgDisk.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QtZlib/zlib.h>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kRunNone = 0x00000000U;
const std::uint32_t kRunRaw = 0x00000001U;
const std::uint32_t kRunZeros = 0x00000002U;
const std::uint32_t kRunZlib = 0x80000005U;
const std::uint32_t kRunComment = 0x7ffffffeU;
const std::uint32_t kRunTerminator = 0xffffffffU;

std::vector<std::uint8_t> inflateDmgZlib(const std::vector<std::uint8_t>& data,
                                         std::size_t expected) {
    std::vector<std::uint8_t> out(expected);
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(out.data());
    stream.avail_out = static_cast<uInt>(out.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return std::vector<std::uint8_t>();
    const int status = inflate(&stream, Z_FINISH);
    const std::size_t produced = stream.total_out;
    inflateEnd(&stream);
    if (status != Z_STREAM_END || produced != expected) return std::vector<std::uint8_t>();
    return out;
}

}  // namespace

DmgRunStore::DmgRunStore(ByteStorePtr source, std::int64_t sectorCount,
                         std::vector<DmgRun> runs)
    : source_(std::move(source)), sectorCount_(sectorCount), runs_(std::move(runs)) {
    if (!source_) {
        error_ = "Null DMG source";
        return;
    }
    if (sectorCount_ < 0) {
        error_ = "Invalid DMG sector count";
        return;
    }
    valid_ = true;
}

int DmgRunStore::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (!valid_ || pos < 0 || count <= 0 || pos >= capacity()) return 0;
    std::int64_t want64 = capacity() - pos;
    if (want64 > count) want64 = count;
    int produced = 0;
    while (want64 > 0) {
        const std::int64_t absSector = (pos + produced) / 512;
        bool matched = false;
        for (std::size_t i = 0; i < runs_.size(); ++i) {
            const DmgRun& r = runs_[i];
            if (r.type == kRunNone || r.type == kRunComment || r.type == kRunTerminator)
                continue;
            if (absSector < r.sectorStart || absSector >= r.sectorStart + r.sectorCount)
                continue;
            const std::int64_t runOffset = (pos + produced) - r.sectorStart * 512;
            const std::int64_t leftInRun = r.sectorCount * 512 - runOffset;
            const int toCopy =
                leftInRun < want64 ? static_cast<int>(leftInRun) : static_cast<int>(want64);

            if (r.type == kRunZeros) {
                std::fill(dst + produced, dst + produced + toCopy, std::uint8_t(0));
            } else if (r.type == kRunRaw) {
                const int n = source_->read(r.compOffset + runOffset, dst + produced, toCopy);
                if (n <= 0) return produced;
                produced += n;
                want64 -= n;
                matched = true;
                break;
            } else if (r.type == kRunZlib) {
                if (!loadRun(i)) return produced;
                const std::vector<std::uint8_t>& inflated = cache_.find(i)->second;
                std::copy(inflated.begin() + static_cast<std::ptrdiff_t>(runOffset),
                          inflated.begin() + static_cast<std::ptrdiff_t>(runOffset + toCopy),
                          dst + produced);
            } else {
                return produced;
            }

            if (r.type != kRunRaw) {
                produced += toCopy;
                want64 -= toCopy;
            }
            matched = true;
            break;
        }
        if (!matched) {
            std::fill(dst + produced, dst + produced + 1, std::uint8_t(0));
            ++produced;
            --want64;
        }
    }
    return produced;
}

bool DmgRunStore::loadRun(std::size_t index) const {
    if (cache_.find(index) != cache_.end()) return true;
    if (index >= runs_.size()) return false;
    const DmgRun& r = runs_[index];
    if (r.compLength <= 2 || r.sectorCount <= 0) return false;
    const std::int64_t readOffset = r.compOffset + 2;  // DiscUtils skips zlib header.
    const std::int64_t readLength = r.compLength - 2;
    if (readLength > std::numeric_limits<int>::max()) return false;
    std::vector<std::uint8_t> compressed = source_->readRange(readOffset, readLength);
    const std::size_t expected = static_cast<std::size_t>(r.sectorCount * 512);
    std::vector<std::uint8_t> inflated = inflateDmgZlib(compressed, expected);
    if (inflated.empty() && expected != 0) return false;
    cache_[index] = inflated;
    return true;
}

}  // namespace fs
}  // namespace peare
