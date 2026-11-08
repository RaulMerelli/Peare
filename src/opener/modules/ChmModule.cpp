#include "ChmModule.h"

#include "Compat.h"
#include "../compression/peare/Lzx_frontends.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace peare {
namespace {

constexpr std::uint64_t kMaxEntries = 1000000;
constexpr std::uint64_t kMaxName = 32768;
constexpr std::uint32_t kDirectoryChunkSizeLimit = 1u << 20;

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p)
{
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool exactLog2(std::uint32_t value, unsigned* result)
{
    if (!value || (value & (value - 1)) != 0) return false;
    unsigned bits = 0;
    while ((std::uint32_t(1) << bits) != value) ++bits;
    *result = bits;
    return true;
}

bool readBytes(const fs::ByteStorePtr& source, std::uint64_t offset,
               std::size_t length, std::vector<std::uint8_t>* bytes)
{
    if (!source || offset > std::uint64_t(source->capacity()) ||
        length > std::uint64_t(source->capacity()) - offset ||
        length > std::size_t(std::numeric_limits<int>::max())) return false;
    *bytes = source->readRange(std::int64_t(offset), std::int64_t(length));
    return bytes->size() == length;
}

struct Entry {
    QString name;
    std::uint64_t section;
    std::uint64_t offset;
    std::uint64_t size;
};

class Cursor {
public:
    Cursor(const std::uint8_t* data, std::size_t size, std::size_t pos)
        : data_(data), size_(size), pos_(pos) {}

    bool integer(std::uint64_t* value)
    {
        *value = 0;
        for (unsigned i = 0; i < 9; ++i) {
            if (pos_ >= size_) return false;
            const std::uint8_t byte = data_[pos_++];
            if (*value > (std::numeric_limits<std::uint64_t>::max() >> 7)) return false;
            *value = (*value << 7) | (byte & 0x7f);
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }

    std::size_t position() const { return pos_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
};

bool parseDirectory(const fs::ByteStorePtr& source, std::uint64_t offset,
                    std::uint64_t size, std::vector<Entry>* entries)
{
    std::vector<std::uint8_t> data;
    if (size < 84 || !readBytes(source, offset, std::size_t(size), &data) ||
        std::memcmp(data.data(), "ITSP", 4) != 0 || le32(data.data() + 4) != 1)
        return false;
    const std::uint32_t chunkSize = le32(data.data() + 16);
    const std::uint32_t chunkCount = le32(data.data() + 44);
    if (chunkSize < 32 || chunkSize > kDirectoryChunkSizeLimit ||
        chunkCount > kMaxEntries || std::uint64_t(chunkCount) * chunkSize > size - 84)
        return false;

    for (std::uint32_t index = 0; index < chunkCount; ++index) {
        const std::size_t base = 84 + std::size_t(index) * chunkSize;
        if (std::memcmp(data.data() + base, "PMGL", 4) != 0) continue;
        const std::uint32_t quickRef = le32(data.data() + base + 4);
        if (quickRef < 2 || quickRef > chunkSize - 20) return false;
        const std::size_t end = base + chunkSize - quickRef;
        Cursor cursor(data.data(), end, base + 20);
        while (cursor.position() < end) {
            std::uint64_t nameLength = 0;
            if (!cursor.integer(&nameLength) || nameLength == 0 || nameLength > kMaxName ||
                nameLength > end - cursor.position()) return false;
            const std::size_t nameStart = cursor.position();
            std::uint64_t section = 0, itemOffset = 0, itemSize = 0;
            Cursor values(data.data(), end, nameStart + std::size_t(nameLength));
            if (!values.integer(&section) || !values.integer(&itemOffset) ||
                !values.integer(&itemSize)) return false;
            const std::size_t next = values.position();
            const QString name = QString::fromLatin1(
                reinterpret_cast<const char*>(data.data() + nameStart), int(nameLength));
            entries->push_back({name, section, itemOffset, itemSize});
            cursor = Cursor(data.data(), data.size(), next);
        }
        if (cursor.position() != end || entries->size() > kMaxEntries) return false;
    }
    return true;
}

class ChmLzxStore final : public fs::IByteStore {
public:
    ChmLzxStore(fs::ByteStorePtr source, std::uint64_t contentOffset,
                std::uint64_t compressedSize, std::uint64_t outputSize,
                std::uint32_t blockSize, unsigned resetIntervalBits, unsigned windowBits,
                std::vector<std::uint64_t> offsets)
        : source_(std::move(source)), contentOffset_(contentOffset),
          compressedSize_(compressedSize), outputSize_(outputSize),
          resetBlockSize_(blockSize),
          blocksPerFolder_(std::size_t(1) << resetIntervalBits),
          folderSize_(std::uint64_t(blockSize) << resetIntervalBits),
          windowBits_(windowBits), offsets_(std::move(offsets)) {}

    std::int64_t capacity() const override
    {
        return outputSize_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(outputSize_) : -1;
    }

    bool cheapRandomAccess() const override { return true; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (pos < 0 || count <= 0 || std::uint64_t(pos) >= outputSize_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        const std::size_t wanted = std::size_t(std::min<std::uint64_t>(
            std::uint64_t(count), outputSize_ - std::uint64_t(pos)));
        std::size_t done = 0;
        while (done < wanted) {
            const std::uint64_t at = std::uint64_t(pos) + done;
            const std::size_t block = std::size_t(at / folderSize_);
            if (!loadBlock(block)) return int(done);
            const std::size_t inBlock = std::size_t(at % folderSize_);
            const std::size_t n = std::min(wanted - done, cache_.size() - inBlock);
            std::copy_n(cache_.data() + inBlock, n, dst + done);
            done += n;
        }
        return int(done);
    }

private:
    bool loadBlock(std::size_t index) const
    {
        if (cachedIndex_ == index) return true;
        const std::size_t firstResetBlock = index * blocksPerFolder_;
        const std::size_t resetBlockCount = offsets_.size() - 1 - firstResetBlock;
        const std::size_t blocksInFolder = std::min(blocksPerFolder_, resetBlockCount);
        const std::size_t afterResetBlock = firstResetBlock + blocksInFolder;
        if (afterResetBlock >= offsets_.size() ||
            offsets_[firstResetBlock] > offsets_[afterResetBlock] ||
            offsets_[afterResetBlock] > compressedSize_) return false;
        const std::uint64_t packedSize =
            offsets_[afterResetBlock] - offsets_[firstResetBlock];
        const std::uint64_t outputOffset = std::uint64_t(index) * folderSize_;
        const std::size_t outputSize = resetBlockSize_ * blocksInFolder;
        if (packedSize == 0 || packedSize > std::numeric_limits<int>::max()) return false;
        std::vector<std::uint8_t> packed;
        if (!readBytes(source_, contentOffset_ + offsets_[firstResetBlock],
                       std::size_t(packedSize), &packed)) return false;
        cache_.resize(outputSize);
        peare_lzx_cab_decoder* decoder = nullptr;
        peare_lzx_status status = peare_lzx_cab_create(std::size_t(1) << windowBits_, &decoder);
        if (status == PEARE_LZX_OK)
            status = peare_lzx_cab_decompress(decoder, packed.data(), packed.size(),
                                               cache_.data(), cache_.size());
        peare_lzx_cab_destroy(decoder);
        if (status != PEARE_LZX_OK) {
            cache_.clear();
            cachedIndex_ = std::numeric_limits<std::size_t>::max();
            return false;
        }
        cachedIndex_ = index;
        return true;
    }

    fs::ByteStorePtr source_;
    std::uint64_t contentOffset_;
    std::uint64_t compressedSize_;
    std::uint64_t outputSize_;
    std::uint32_t resetBlockSize_;
    std::size_t blocksPerFolder_;
    std::uint64_t folderSize_;
    unsigned windowBits_;
    std::vector<std::uint64_t> offsets_;
    mutable std::mutex mutex_;
    mutable std::size_t cachedIndex_ = std::numeric_limits<std::size_t>::max();
    mutable std::vector<std::uint8_t> cache_;
};

class ChmFileStore final : public fs::IByteStore {
public:
    ChmFileStore(fs::ByteStorePtr parent, std::uint64_t offset, std::uint64_t size)
        : parent_(std::move(parent)), offset_(offset), size_(size) {}
    std::int64_t capacity() const override { return std::int64_t(size_); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || std::uint64_t(pos) >= size_) return 0;
        return parent_->read(std::int64_t(offset_ + std::uint64_t(pos)), dst,
            int(std::min<std::uint64_t>(std::uint64_t(count), size_ - std::uint64_t(pos))));
    }
private:
    fs::ByteStorePtr parent_;
    std::uint64_t offset_;
    std::uint64_t size_;
};

} // namespace

ModulePtr ChmModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr ChmModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())),
        logicalName);
}

ModulePtr ChmModule::open(const fs::ByteStorePtr& source, const QString& logicalName)
{
    auto module = peare::makeUnique<ChmModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::CHM;
    module->info_.description = QStringLiteral("Microsoft Compiled HTML Help (CHM)");
    std::vector<std::uint8_t> header;
    if (!source || !readBytes(source, 0, 12, &header) ||
        std::memcmp(header.data(), "ITSF", 4) != 0) {
        module->info_.error = QStringLiteral("Unsupported or invalid CHM header");
        return ModulePtr(std::move(module));
    }
    const std::uint32_t version = le32(header.data() + 4);
    const std::uint32_t headerLength = le32(header.data() + 8);
    const std::size_t fixedHeaderLength = version == 2 ? 0x58 : version == 3 ? 0x60 : 0;
    if (fixedHeaderLength == 0 || headerLength < fixedHeaderLength ||
        !readBytes(source, 0, fixedHeaderLength, &header)) {
        module->info_.error = QStringLiteral("Unsupported or invalid CHM header");
        return ModulePtr(std::move(module));
    }
    const std::uint64_t section0 = le64(header.data() + 0x38);
    const std::uint64_t section0Size = le64(header.data() + 0x40);
    const std::uint64_t directory = le64(header.data() + 0x48);
    const std::uint64_t directorySize = le64(header.data() + 0x50);
    if (directory > std::uint64_t(source->capacity()) ||
        directorySize > std::uint64_t(source->capacity()) - directory) {
        module->info_.error = QStringLiteral("CHM section is outside the input file");
        return ModulePtr(std::move(module));
    }
    // ITSF v2 section-0 entry offsets are absolute file positions.
    const std::uint64_t content = version == 2 ? 0 : le64(header.data() + 0x58);
    if (section0 > std::uint64_t(source->capacity()) ||
        section0Size > std::uint64_t(source->capacity()) - section0 ||
        content > std::uint64_t(source->capacity())) {
        module->info_.error = QStringLiteral("CHM section is outside the input file");
        return ModulePtr(std::move(module));
    }
    std::vector<Entry> entries;
    if (!parseDirectory(source, directory, directorySize, &entries)) {
        module->info_.error = QStringLiteral("Invalid CHM directory index");
        return ModulePtr(std::move(module));
    }

    std::uint64_t compressedOffset = 0, compressedSize = 0, outputSize = 0;
    std::uint32_t blockSize = 0;
    unsigned resetIntervalBits = 0;
    unsigned windowBits = 0;
    std::vector<std::uint64_t> resetOffsets;
    for (const Entry& entry : entries) {
        if (entry.section != 0) continue;
        if (entry.name == QStringLiteral("::DataSpace/Storage/MSCompressed/Content")) {
            compressedOffset = entry.offset;
            compressedSize = entry.size;
        } else if (entry.name == QStringLiteral("::DataSpace/Storage/MSCompressed/SpanInfo")) {
            std::vector<std::uint8_t> bytes;
            if (entry.size != 8 || !readBytes(source, content + entry.offset, 8, &bytes)) break;
            outputSize = le64(bytes.data());
        } else if (entry.name.contains(QStringLiteral("/ControlData"))) {
            std::vector<std::uint8_t> bytes;
            if (entry.size < 24 || !readBytes(source, content + entry.offset,
                                               std::size_t(entry.size), &bytes)) break;
            if (le32(bytes.data()) < 5 || le32(bytes.data() + 4) != 0x43585A4C ||
                le32(bytes.data() + 8) != 2) break;
            unsigned resetBits = 0, windowExponent = 0;
            if (!exactLog2(le32(bytes.data() + 12), &resetBits) || resetBits > 16 ||
                !exactLog2(le32(bytes.data() + 16), &windowExponent) || windowExponent > 6)
                break;
            resetIntervalBits = resetBits;
            windowBits = 15 + windowExponent;
        } else if (entry.name.endsWith(QStringLiteral("/ResetTable"))) {
            std::vector<std::uint8_t> bytes;
            if (entry.size < 40 || entry.size > 16 * 1024 * 1024 ||
                !readBytes(source, content + entry.offset, std::size_t(entry.size), &bytes)) break;
            if ((le32(bytes.data()) != 2 && le32(bytes.data()) != 3) ||
                le32(bytes.data() + 12) != 40 ||
                40ull + std::uint64_t(le32(bytes.data() + 4)) * 8 != entry.size) break;
            outputSize = le64(bytes.data() + 16);
            compressedSize = le64(bytes.data() + 24);
            const std::uint64_t encodedBlock = le64(bytes.data() + 32);
            // Early ITSF v2 files store the block size in the low DWORD.
            const std::uint64_t block = version == 2 && encodedBlock > (1u << 24)
                ? le32(bytes.data() + 32) : encodedBlock;
            if (block == 0 || block > (1u << 24)) break;
            blockSize = std::uint32_t(block);
            const std::uint32_t count = le32(bytes.data() + 4);
            resetOffsets.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i)
                resetOffsets.push_back(le64(bytes.data() + 40 + std::size_t(i) * 8));
        }
    }
    const std::uint64_t expectedBlocks = blockSize && outputSize
        ? 1 + (outputSize - 1) / blockSize : 0;
    if (resetOffsets.size() == expectedBlocks && resetOffsets.size() > 0)
        resetOffsets.push_back(compressedSize);
    if (compressedSize && outputSize && blockSize && windowBits >= 15 && windowBits <= 21 &&
        resetOffsets.size() == expectedBlocks + 1 && resetOffsets.front() == 0 &&
        resetOffsets.back() == compressedSize &&
        std::is_sorted(resetOffsets.begin(), resetOffsets.end()) &&
        content + compressedOffset <= std::uint64_t(source->capacity()) &&
        compressedSize <= std::uint64_t(source->capacity()) - (content + compressedOffset)) {
        const auto compressed = std::make_shared<ChmLzxStore>(source, content + compressedOffset,
            compressedSize, outputSize, blockSize, resetIntervalBits, windowBits, resetOffsets);
        for (const Entry& entry : entries) {
            if (entry.name.startsWith(QStringLiteral("::")) || entry.name == QStringLiteral("/"))
                continue;
            if (entry.section > 1) continue;
            const std::uint64_t sectionCapacity = entry.section == 0
                ? std::uint64_t(source->capacity()) - content : outputSize;
            if (entry.offset > sectionCapacity || entry.size > sectionCapacity - entry.offset)
                continue;
            QString path = entry.name;
            if (path.startsWith(QLatin1Char('/'))) path.remove(0, 1);
            const bool directoryEntry = path.endsWith(QLatin1Char('/'));
            if (directoryEntry) path.chop(1);
            if (path.isEmpty()) continue;
            QStringList pathParts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            if (pathParts.isEmpty()) continue;
            ResourceEntry resource;
            resource.type = directoryEntry ? QStringLiteral("CHM_FOLDER") : QStringLiteral("CHM_FILE");
            resource.name = pathParts.takeLast();
            resource.hierarchyPath = pathParts;
            if (directoryEntry)
                resource.hierarchyPath.push_back(resource.name);
            resource.language = QStringLiteral("neutral");
            resource.dataOffset = entry.offset;
            resource.dataSize = entry.size;
            resource.format = ModuleFormat::CHM;
            resource.isEmbeddedFile = !directoryEntry;
            resource.content = directoryEntry ? fs::ByteStorePtr() :
                (entry.section == 0
                    ? fs::ByteStorePtr(std::make_shared<ChmFileStore>(source, content + entry.offset, entry.size))
                    : fs::ByteStorePtr(std::make_shared<ChmFileStore>(compressed, entry.offset, entry.size)));
            module->resources_.push_back(std::move(resource));
        }
        return ModulePtr(std::move(module));
    }

    module->info_.error = QStringLiteral("Unsupported CHM compression metadata");
    return ModulePtr(std::move(module));
}

} // namespace peare
