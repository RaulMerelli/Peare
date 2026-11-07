#include "XzModule.h"
#include "Compat.h"

#include "../compression/Crc32.h"
#include "../compression/LzmaDecoder.h"
#include "../fs/QFileByteStore.h"

#include <QCryptographicHash>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace peare {
namespace {

constexpr std::uint8_t kMagic[] = {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00};

struct BlockIndexEntry {
    std::uint64_t unpaddedSize = 0;
    std::uint64_t unpackedSize = 0;
};

struct StreamIndex {
    std::size_t indexStart = 0;
    std::size_t footerEnd = 0;
    std::uint8_t checkId = 0;
    std::vector<BlockIndexEntry> blocks;
};

std::uint32_t readLe32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return std::uint32_t(data[offset]) | (std::uint32_t(data[offset + 1]) << 8) |
        (std::uint32_t(data[offset + 2]) << 16) | (std::uint32_t(data[offset + 3]) << 24);
}

std::uint64_t readLe64(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return std::uint64_t(readLe32(data, offset)) |
        (std::uint64_t(readLe32(data, offset + 4)) << 32);
}

bool readVli(const std::vector<std::uint8_t>& data, std::size_t end,
             std::size_t* position, std::uint64_t* value)
{
    std::uint64_t result = 0;
    unsigned shift = 0;
    for (unsigned i = 0; i < 9 && *position < end; ++i) {
        const std::uint8_t byte = data[(*position)++];
        if (shift == 63 && (byte & 0x7E) != 0) return false;
        result |= std::uint64_t(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            if (i != 0 && byte == 0) return false;
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

std::uint64_t crc64(const std::uint8_t* data, std::size_t size)
{
    static const std::array<std::uint64_t, 256> table = [] {
        std::array<std::uint64_t, 256> values = {};
        for (std::size_t i = 0; i < values.size(); ++i) {
            std::uint64_t value = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ (0xC96C5795D7870F42ULL & (0ULL - (value & 1ULL)));
            values[i] = value;
        }
        return values;
    }();
    std::uint64_t crc = UINT64_MAX;
    while (size--) crc = table[(crc ^ *data++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

QByteArray sha256(const std::uint8_t* data, std::size_t size)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (size != 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(
            size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        hash.addData(reinterpret_cast<const char*>(data), chunk);
        data += chunk;
        size -= static_cast<std::size_t>(chunk);
    }
    return hash.result();
}

std::size_t checkSize(std::uint8_t checkId)
{
    switch (checkId) {
    case 0: return 0;
    case 1: return 4;
    case 4: return 8;
    case 10: return 32;
    default: return std::numeric_limits<std::size_t>::max();
    }
}

bool parseIndex(const std::vector<std::uint8_t>& data, std::size_t start,
                std::size_t end, std::vector<BlockIndexEntry>* blocks)
{
    if (end < start + 5 || data[start] != 0 ||
        readLe32(data, end - 4) != compression::crc32(data.data() + start, end - start - 4))
        return false;
    const std::size_t contentEnd = end - 4;
    std::size_t position = start + 1;
    std::uint64_t count = 0;
    if (!readVli(data, contentEnd, &position, &count) || count > contentEnd - position)
        return false;
    std::vector<BlockIndexEntry> parsed;
    parsed.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        BlockIndexEntry entry;
        if (!readVli(data, contentEnd, &position, &entry.unpaddedSize) ||
            !readVli(data, contentEnd, &position, &entry.unpackedSize))
            return false;
        parsed.push_back(entry);
    }
    while ((position - start) % 4 != 0) {
        if (position >= contentEnd || data[position++] != 0) return false;
    }
    if (position != contentEnd) return false;
    blocks->swap(parsed);
    return true;
}

bool findStreamIndex(const std::vector<std::uint8_t>& data, std::size_t streamStart,
                     StreamIndex* result)
{
    if (data.size() - streamStart < 24 ||
        !std::equal(std::begin(kMagic), std::end(kMagic), data.begin() + streamStart))
        return false;
    const std::size_t headerCrcOffset = streamStart + 8;
    if (readLe32(data, headerCrcOffset) !=
        compression::crc32(data.data() + streamStart + 6, 2) ||
        data[streamStart + 6] != 0 || (data[streamStart + 7] & 0xF0) != 0)
        return false;
    const std::uint8_t checkId = data[streamStart + 7] & 0x0F;
    if (checkSize(checkId) == std::numeric_limits<std::size_t>::max()) return false;

    for (std::size_t footer = streamStart + 12; footer + 12 <= data.size(); footer += 4) {
        if (data[footer + 10] != 'Y' || data[footer + 11] != 'Z' ||
            readLe32(data, footer) != compression::crc32(data.data() + footer + 4, 6) ||
            data[footer + 8] != data[streamStart + 6] ||
            data[footer + 9] != data[streamStart + 7])
            continue;
        const std::uint64_t indexSize = (std::uint64_t(readLe32(data, footer + 4)) + 1) * 4;
        if (indexSize > footer - streamStart - 12) continue;
        const std::size_t indexStart = footer - static_cast<std::size_t>(indexSize);
        std::vector<BlockIndexEntry> blocks;
        if (!parseIndex(data, indexStart, footer, &blocks)) continue;

        std::size_t blockPosition = streamStart + 12;
        bool valid = true;
        for (const BlockIndexEntry& entry : blocks) {
            if (blockPosition >= indexStart || data[blockPosition] == 0) {
                valid = false;
                break;
            }
            const std::size_t headerSize = (std::size_t(data[blockPosition]) + 1) * 4;
            if (headerSize < 8 || headerSize > indexStart - blockPosition ||
                readLe32(data, blockPosition + headerSize - 4) !=
                    compression::crc32(data.data() + blockPosition, headerSize - 4) ||
                entry.unpaddedSize <= headerSize + checkSize(checkId) ||
                entry.unpaddedSize > indexStart - blockPosition)
                valid = false;
            if (!valid) break;
            const std::size_t paddedSize = static_cast<std::size_t>(
                (entry.unpaddedSize + 3) & ~std::uint64_t(3));
            if (paddedSize > indexStart - blockPosition) {
                valid = false;
                break;
            }
            blockPosition += paddedSize;
        }
        if (!valid || blockPosition != indexStart) continue;
        result->indexStart = indexStart;
        result->footerEnd = footer + 12;
        result->checkId = checkId;
        result->blocks = std::move(blocks);
        return true;
    }
    return false;
}

bool decodeStream(const std::vector<std::uint8_t>& data, std::size_t streamStart,
                  const StreamIndex& index, std::vector<std::uint8_t>* output)
{
    std::size_t position = streamStart + 12;
    const std::size_t checkBytes = checkSize(index.checkId);
    for (const BlockIndexEntry& entry : index.blocks) {
        const std::size_t headerSize = (std::size_t(data[position]) + 1) * 4;
        const std::uint8_t flags = data[position + 1];
        if ((flags & 0x3C) != 0) return false;
        const unsigned filterCount = (flags & 3) + 1;
        std::size_t cursor = position + 2;
        std::uint64_t declaredCompressed = 0;
        std::uint64_t declaredUnpacked = 0;
        if ((flags & 0x40) != 0 &&
            !readVli(data, position + headerSize - 4, &cursor, &declaredCompressed))
            return false;
        if ((flags & 0x80) != 0 &&
            !readVli(data, position + headerSize - 4, &cursor, &declaredUnpacked))
            return false;
        std::uint64_t filterId = 0;
        std::uint64_t propertiesSize = 0;
        if (filterCount != 1 || !readVli(data, position + headerSize - 4, &cursor, &filterId) ||
            !readVli(data, position + headerSize - 4, &cursor, &propertiesSize) ||
            filterId != 0x21 || propertiesSize != 1 ||
            cursor + propertiesSize > position + headerSize - 4)
            return false;
        const std::uint8_t property = data[cursor++];
        while (cursor < position + headerSize - 4) {
            if (data[cursor++] != 0) return false;
        }

        const std::uint64_t compressedSize = entry.unpaddedSize - headerSize - checkBytes;
        if (compressedSize == 0 ||
            ((flags & 0x40) != 0 && declaredCompressed != compressedSize) ||
            ((flags & 0x80) != 0 && declaredUnpacked != entry.unpackedSize) ||
            compressedSize > std::numeric_limits<std::size_t>::max() ||
            entry.unpackedSize > std::numeric_limits<std::size_t>::max() ||
            entry.unpackedSize > output->max_size() - output->size())
            return false;
        const std::size_t payload = position + headerSize;
        std::vector<std::uint8_t> decoded;
        if (!compression::decodeLzma2Raw(data.data() + payload,
                static_cast<std::size_t>(compressedSize), &property, 1,
                static_cast<std::size_t>(entry.unpackedSize), &decoded))
            return false;
        const std::size_t compressedEnd = payload + static_cast<std::size_t>(compressedSize);
        const std::size_t blockPadding = (4 -
            ((headerSize + static_cast<std::size_t>(compressedSize)) & 3)) & 3;
        const std::size_t checkOffset = compressedEnd + blockPadding;
        for (std::size_t i = compressedEnd; i < checkOffset; ++i)
            if (data[i] != 0) return false;
        if (index.checkId != 0) {
            std::vector<std::uint8_t> checkData(data.begin() + std::ptrdiff_t(checkOffset),
                data.begin() + std::ptrdiff_t(checkOffset + checkBytes));
            if (index.checkId == 1 && readLe32(checkData, 0) !=
                compression::crc32(decoded.data(), decoded.size())) return false;
            if (index.checkId == 4 && readLe64(checkData, 0) !=
                crc64(decoded.data(), decoded.size())) return false;
            if (index.checkId == 10) {
                const QByteArray digest = sha256(decoded.data(), decoded.size());
                if (!std::equal(digest.cbegin(), digest.cend(),
                                reinterpret_cast<const char*>(checkData.data()))) return false;
            }
        }
        output->insert(output->end(), decoded.begin(), decoded.end());
        position += headerSize + static_cast<std::size_t>(compressedSize) +
            blockPadding + checkBytes;
    }
    return position == index.indexStart;
}

QString fallbackName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".txz"), Qt::CaseInsensitive)) name.chop(4);
    else if (name.endsWith(QStringLiteral(".xz"), Qt::CaseInsensitive)) name.chop(3);
    return name.isEmpty() ? QStringLiteral("xz_payload") : name;
}

bool decompress(const std::vector<std::uint8_t>& data, std::vector<std::uint8_t>* output)
{
    if (data.size() < 24 || data.size() % 4 != 0) return false;
    std::size_t streamStart = 0;
    std::vector<std::uint8_t> decoded;
    while (streamStart < data.size()) {
        StreamIndex index;
        if (!findStreamIndex(data, streamStart, &index) ||
            !decodeStream(data, streamStart, index, &decoded)) return false;
        streamStart = index.footerEnd;
        const std::size_t paddingStart = streamStart;
        while (streamStart < data.size() && data[streamStart] == 0) ++streamStart;
        if ((streamStart - paddingStart) % 4 != 0) return false;
        if (streamStart < data.size() &&
            (data.size() - streamStart < sizeof(kMagic) ||
             !std::equal(std::begin(kMagic), std::end(kMagic), data.begin() + streamStart)))
            return false;
    }
    if (decoded.empty() && data.size() < 24) return false;
    output->swap(decoded);
    return true;
}

} // namespace

bool XzModule::decodePayload(const std::vector<std::uint8_t>& data,
                             std::vector<std::uint8_t>* output)
{
    return decompress(data, output);
}

std::unique_ptr<XzModule> XzModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<XzModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::XZ;
        module->info_.error = QStringLiteral("Could not read XZ input");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<XzModule> XzModule::open(const fs::ByteStorePtr& source,
                                         const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<XzModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::XZ;
        module->info_.error = QStringLiteral("Invalid XZ byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<XzModule> XzModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

std::unique_ptr<XzModule> XzModule::open(std::vector<std::uint8_t> data,
                                         const QString& logicalName)
{
    auto module = peare::makeUnique<XzModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::XZ;
    module->info_.description = QStringLiteral("XZ compressed stream");
    std::vector<std::uint8_t> decoded;
    if (!decompress(data, &decoded)) {
        module->info_.error = QStringLiteral("Invalid, unsupported or corrupt XZ stream");
        return module;
    }
    ResourceEntry entry;
    entry.type = QStringLiteral("XZ_FILE");
    entry.name = fallbackName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::XZ;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
