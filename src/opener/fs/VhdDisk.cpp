#include "VhdDisk.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::int64_t kSector = 512;

std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::int32_t be32s(const std::uint8_t* p) {
    return static_cast<std::int32_t>(be32(p));
}

std::int64_t be64(const std::uint8_t* p) {
    return static_cast<std::int64_t>((static_cast<std::uint64_t>(be32(p)) << 32) |
                                     static_cast<std::uint64_t>(be32(p + 4)));
}

std::int64_t roundUp(std::int64_t value, std::int64_t unit) {
    return ((value + unit - 1) / unit) * unit;
}

struct VhdFooter {
    bool valid = false;
    std::int64_t dataOffset = -1;
    std::int64_t currentSize = 0;
    std::uint32_t diskType = 0;
};

VhdFooter parseFooter(const std::vector<std::uint8_t>& sector) {
    VhdFooter f;
    if (sector.size() < 512 || std::memcmp(sector.data(), "conectix", 8) != 0)
        return f;
    const std::uint32_t version = be32(sector.data() + 12);
    if (version != 0x00010000U)
        return f;
    f.dataOffset = be64(sector.data() + 16);
    f.currentSize = be64(sector.data() + 48);
    f.diskType = be32(sector.data() + 60);
    f.valid = f.currentSize > 0;
    return f;
}

VhdFooter readFooter(const ByteStorePtr& file) {
    VhdFooter f;
    if (!file || file->capacity() < kSector)
        return f;
    f = parseFooter(file->readRange(file->capacity() - kSector, kSector));
    if (f.valid)
        return f;
    return parseFooter(file->readRange(0, kSector));
}

class VhdDynamicStore final : public IByteStore {
public:
    VhdDynamicStore(ByteStorePtr file, const VhdFooter& footer, std::string* error)
        : file_(std::move(file)), length_(footer.currentSize) {
        valid_ = readHeaderAndBat(footer.dataOffset, error);
    }

    bool valid() const { return valid_; }
    std::int64_t capacity() const override { return length_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= length_) return 0;
        const int want = static_cast<int>(std::min<std::int64_t>(count, length_ - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const std::int64_t block = p / blockSize_;
            const std::int64_t offsetInBlock = p % blockSize_;
            const int sectorInBlock = static_cast<int>(offsetInBlock / kSector);
            const int offsetInSector = static_cast<int>(offsetInBlock % kSector);
            const int chunk = static_cast<int>(std::min<std::int64_t>(
                want - produced, kSector - offsetInSector));

            if (block < 0 || block >= static_cast<std::int64_t>(bat_.size()) ||
                bat_[static_cast<std::size_t>(block)] == 0xFFFFFFFFU ||
                !sectorAllocated(block, sectorInBlock)) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const std::int64_t blockStart =
                    static_cast<std::int64_t>(bat_[static_cast<std::size_t>(block)]) * kSector;
                const std::int64_t src =
                    blockStart + bitmapSize_ + static_cast<std::int64_t>(sectorInBlock) * kSector +
                    offsetInSector;
                int got = file_->read(src, dst + produced, chunk);
                if (got < chunk)
                    std::fill(dst + produced + got, dst + produced + chunk, std::uint8_t(0));
            }
            produced += chunk;
        }
        return produced;
    }

private:
    bool readHeaderAndBat(std::int64_t headerOffset, std::string* error) {
        if (headerOffset < 0 || headerOffset + 1024 > file_->capacity()) {
            if (error) *error = "Invalid VHD dynamic header offset";
            return false;
        }
        std::vector<std::uint8_t> h = file_->readRange(headerOffset, 1024);
        if (h.size() < 1024 || std::memcmp(h.data(), "cxsparse", 8) != 0) {
            if (error) *error = "Missing VHD dynamic header";
            return false;
        }
        const std::uint32_t version = be32(h.data() + 24);
        const std::int32_t entries = be32s(h.data() + 28);
        blockSize_ = be32(h.data() + 32);
        const std::int64_t tableOffset = be64(h.data() + 16);
        if (version != 0x00010000U || entries <= 0 || blockSize_ <= 0 ||
            tableOffset < 0 || tableOffset + static_cast<std::int64_t>(entries) * 4 > file_->capacity()) {
            if (error) *error = "Invalid VHD dynamic geometry";
            return false;
        }
        bitmapSize_ = roundUp((blockSize_ + kSector * 8 - 1) / (kSector * 8), kSector);
        std::vector<std::uint8_t> rawBat =
            file_->readRange(tableOffset, static_cast<std::int64_t>(entries) * 4);
        if (rawBat.size() < static_cast<std::size_t>(entries) * 4) {
            if (error) *error = "Truncated VHD block allocation table";
            return false;
        }
        bat_.resize(static_cast<std::size_t>(entries));
        for (int i = 0; i < entries; ++i)
            bat_[static_cast<std::size_t>(i)] = be32(rawBat.data() + i * 4);
        bitmaps_.resize(bat_.size());
        return true;
    }

    bool sectorAllocated(std::int64_t block, int sectorInBlock) const {
        if (block < 0 || block >= static_cast<std::int64_t>(bat_.size()))
            return false;
        const std::size_t bi = static_cast<std::size_t>(block);
        if (bitmaps_[bi].empty()) {
            const std::int64_t bitmapOffset = static_cast<std::int64_t>(bat_[bi]) * kSector;
            bitmaps_[bi] = file_->readRange(bitmapOffset, bitmapSize_);
            if (bitmaps_[bi].size() < static_cast<std::size_t>(bitmapSize_))
                bitmaps_[bi].clear();
        }
        if (bitmaps_[bi].empty())
            return false;
        const std::size_t byteIndex = static_cast<std::size_t>(sectorInBlock / 8);
        if (byteIndex >= bitmaps_[bi].size())
            return false;
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << (7 - (sectorInBlock & 7)));
        return (bitmaps_[bi][byteIndex] & mask) != 0;
    }

    ByteStorePtr file_;
    std::int64_t length_ = 0;
    std::int64_t blockSize_ = 0;
    std::int64_t bitmapSize_ = 0;
    bool valid_ = false;
    std::vector<std::uint32_t> bat_;
    mutable std::vector<std::vector<std::uint8_t>> bitmaps_;
};

}  // namespace

ByteStorePtr openVhdDisk(const ByteStorePtr& file, std::string* error) {
    auto fail = [&](const std::string& message) -> ByteStorePtr {
        if (error) *error = message;
        return nullptr;
    };
    if (!file)
        return fail("Null VHD source");
    const VhdFooter footer = readFooter(file);
    if (!footer.valid)
        return fail("No valid VHD footer");
    if (footer.diskType == 2) {
        const std::int64_t len = std::min<std::int64_t>(footer.currentSize, file->capacity() - kSector);
        if (len <= 0)
            return fail("Invalid fixed VHD size");
        return std::make_shared<SubStore>(file, 0, len);
    }
    if (footer.diskType == 3 || footer.diskType == 4) {
        ByteStorePtr disk = std::make_shared<VhdDynamicStore>(file, footer, error);
        if (!static_cast<VhdDynamicStore*>(disk.get())->valid())
            return nullptr;
        return disk;
    }
    return fail("Unsupported VHD disk type");
}

}  // namespace fs
}  // namespace peare
