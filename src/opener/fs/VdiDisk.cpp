#include "VdiDisk.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kVdiSignature = 0xbeda107fU;
const std::uint32_t kBlockFree = 0xffffffffU;
const std::uint32_t kBlockZero = 0xfffffffeU;

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::int32_t le32s(const std::uint8_t* p) {
    return static_cast<std::int32_t>(le32(p));
}

std::int64_t le64(const std::uint8_t* p) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(le32(p)) |
                                     (static_cast<std::uint64_t>(le32(p + 4)) << 32));
}

struct VdiHeader {
    std::uint32_t version = 0;
    std::uint32_t imageType = 0;
    std::uint32_t blocksOffset = 0;
    std::uint32_t dataOffset = 0;
    std::int64_t diskSize = 0;
    std::int32_t blockSize = 0;
    std::int32_t blockExtraSize = 0;
    std::int32_t blockCount = 0;
};

bool readHeader(const ByteStorePtr& file, VdiHeader* h, std::string* error) {
    if (!file || file->capacity() < 72 + 348) {
        if (error) *error = "Truncated VDI image";
        return false;
    }
    std::vector<std::uint8_t> pre = file->readRange(0, 72);
    if (pre.size() < 72 || le32(pre.data() + 64) != kVdiSignature) {
        if (error) *error = "Missing VDI pre-header signature";
        return false;
    }
    h->version = le32(pre.data() + 68);
    const std::uint16_t major = static_cast<std::uint16_t>(h->version >> 16);
    const std::uint16_t minor = static_cast<std::uint16_t>(h->version & 0xffffU);

    if (major == 0) {
        std::vector<std::uint8_t> b = file->readRange(72, 348);
        if (b.size() < 348) {
            if (error) *error = "Truncated VDI v0 header";
            return false;
        }
        h->imageType = le32(b.data());
        h->diskSize = le64(b.data() + 280);
        h->blockSize = le32s(b.data() + 288);
        h->blockCount = le32s(b.data() + 292);
        h->blocksOffset = 72 + 348;
        h->dataOffset = h->blocksOffset + static_cast<std::uint32_t>(h->blockCount) * 4U;
        h->blockExtraSize = 0;
    } else if (major == 1 && minor == 1) {
        std::vector<std::uint8_t> sizeBytes = file->readRange(72, 4);
        if (sizeBytes.size() < 4) {
            if (error) *error = "Truncated VDI header size";
            return false;
        }
        const std::uint32_t headerSize = le32(sizeBytes.data());
        if (headerSize < 384 || headerSize > 4096 || 72 + headerSize > file->capacity()) {
            if (error) *error = "Invalid VDI header size";
            return false;
        }
        std::vector<std::uint8_t> b = file->readRange(72, headerSize);
        if (b.size() < headerSize) {
            if (error) *error = "Truncated VDI v1 header";
            return false;
        }
        h->imageType = le32(b.data() + 4);
        h->blocksOffset = le32(b.data() + 268);
        h->dataOffset = le32(b.data() + 272);
        h->diskSize = le64(b.data() + 296);
        h->blockSize = le32s(b.data() + 304);
        h->blockExtraSize = le32s(b.data() + 308);
        h->blockCount = le32s(b.data() + 312);
    } else {
        if (error) *error = "Unsupported VDI version";
        return false;
    }

    if (h->imageType == 3 || h->imageType == 4) {
        if (error) *error = "VDI differencing/undo images require a parent image";
        return false;
    }
    if (h->imageType != 1 && h->imageType != 2) {
        if (error) *error = "Unsupported VDI image type";
        return false;
    }
    if (h->diskSize <= 0 || h->blockSize <= 0 || h->blockExtraSize < 0 || h->blockCount <= 0 ||
        h->blocksOffset == 0 || h->dataOffset == 0 ||
        static_cast<std::int64_t>(h->blocksOffset) + static_cast<std::int64_t>(h->blockCount) * 4 >
            file->capacity()) {
        if (error) *error = "Invalid VDI geometry";
        return false;
    }
    return true;
}

class VdiStore final : public IByteStore {
public:
    VdiStore(ByteStorePtr file, const VdiHeader& header, std::string* error)
        : file_(std::move(file)), header_(header) {
        valid_ = readBlockTable(error);
    }

    bool valid() const { return valid_; }
    std::int64_t capacity() const override { return header_.diskSize; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= header_.diskSize) return 0;
        const int want = static_cast<int>(std::min<std::int64_t>(count, header_.diskSize - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const std::int64_t block = p / header_.blockSize;
            const std::int64_t offsetInBlock = p % header_.blockSize;
            const int chunk = static_cast<int>(std::min<std::int64_t>(
                want - produced, static_cast<std::int64_t>(header_.blockSize) - offsetInBlock));

            if (block < 0 || block >= static_cast<std::int64_t>(blockTable_.size())) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const std::uint32_t physical = blockTable_[static_cast<std::size_t>(block)];
                if (physical == kBlockFree || physical == kBlockZero) {
                    std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
                } else {
                    const std::int64_t stride =
                        static_cast<std::int64_t>(header_.blockSize) + header_.blockExtraSize;
                    const std::int64_t src = static_cast<std::int64_t>(header_.dataOffset) +
                                             static_cast<std::int64_t>(physical) * stride +
                                             header_.blockExtraSize + offsetInBlock;
                    int got = file_->read(src, dst + produced, chunk);
                    if (got < chunk)
                        std::fill(dst + produced + got, dst + produced + chunk, std::uint8_t(0));
                }
            }
            produced += chunk;
        }
        return produced;
    }

private:
    bool readBlockTable(std::string* error) {
        const std::int64_t tableBytes = static_cast<std::int64_t>(header_.blockCount) * 4;
        std::vector<std::uint8_t> raw = file_->readRange(header_.blocksOffset, tableBytes);
        if (raw.size() < static_cast<std::size_t>(tableBytes)) {
            if (error) *error = "Truncated VDI block table";
            return false;
        }
        blockTable_.resize(static_cast<std::size_t>(header_.blockCount));
        for (int i = 0; i < header_.blockCount; ++i)
            blockTable_[static_cast<std::size_t>(i)] = le32(raw.data() + i * 4);
        return true;
    }

    ByteStorePtr file_;
    VdiHeader header_;
    bool valid_ = false;
    std::vector<std::uint32_t> blockTable_;
};

}  // namespace

ByteStorePtr openVdiDisk(const ByteStorePtr& file, std::string* error) {
    auto fail = [&](const std::string& message) -> ByteStorePtr {
        if (error) *error = message;
        return nullptr;
    };
    VdiHeader header;
    if (!readHeader(file, &header, error))
        return nullptr;
    ByteStorePtr disk = std::make_shared<VdiStore>(file, header, error);
    if (!static_cast<VdiStore*>(disk.get())->valid())
        return nullptr;
    if (disk->capacity() <= 0)
        return fail("Invalid VDI logical size");
    return disk;
}

}  // namespace fs
}  // namespace peare
