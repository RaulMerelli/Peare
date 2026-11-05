#include "LhaModule.h"
#include "Compat.h"
#include "../compression/peare/LhaDecoder.h"
#include "../fs/QFileByteStore.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace peare {
namespace {

constexpr std::size_t kMaxEntryCount = 1000000;

bool hasMethodMarkerAt(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    static const char* const methods[] = {
        "-lz4-", "-lz5-", "-lzs-", "-lhd-", "-lh0-", "-lh1-", "-lh2-", "-lh3-",
        "-lh4-", "-lh5-", "-lh6-", "-lh7-", "-lhx-", "-pm0-",
        "-pm1-", "-pm2-"
    };
    if (offset > data.size() || data.size() - offset < 27 || data[offset + 20] > 3 ||
        (data[offset + 20] < 2 && data[offset] == 0))
        return false;
    for (const char* method : methods) {
        if (std::memcmp(data.data() + offset + 2, method, 5) == 0)
            return true;
    }
    return false;
}

bool hasMethodMarker(const std::vector<std::uint8_t>& data)
{
    const std::size_t limit = std::min<std::size_t>(data.size(), 256 * 1024 + 16);
    for (std::size_t marker = 2; marker + 25 <= limit; ++marker) {
        if ((data[marker] == '-' && (data[marker + 1] == 'l' || data[marker + 1] == 'p')) &&
            hasMethodMarkerAt(data, marker - 2)) return true;
    }
    return false;
}

QStringList safePathParts(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    const QString invalid = QStringLiteral("<>:\"|?*");
    QStringList parts;
    for (QString part : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) part = QStringLiteral("_");
        for (QChar& character : part) {
            if (character.unicode() < 0x20 || invalid.contains(character))
                character = QLatin1Char('_');
        }
        if (!part.isEmpty()) parts.push_back(part);
    }
    return parts;
}

std::uint16_t updateLhaCrc16(std::uint16_t crc, const std::uint8_t* data,
                            std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? std::uint16_t((crc >> 1) ^ 0xA001U)
                             : std::uint16_t(crc >> 1);
    }
    return crc;
}

std::uint16_t lhaCrc16(const std::uint8_t* data, std::size_t size)
{
    return updateLhaCrc16(0, data, size);
}

std::uint8_t byteAt(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return data[offset];
}

std::uint16_t readLe16(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return std::uint16_t(byteAt(data, offset)) |
           (std::uint16_t(byteAt(data, offset + 1)) << 8);
}

std::uint32_t readLe32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    return std::uint32_t(byteAt(data, offset)) |
           (std::uint32_t(byteAt(data, offset + 1)) << 8) |
           (std::uint32_t(byteAt(data, offset + 2)) << 16) |
           (std::uint32_t(byteAt(data, offset + 3)) << 24);
}

QString localCString(const std::uint8_t* data, std::size_t size)
{
    if (!size) return QString();
    const std::uint8_t* terminator = std::find(data, data + size, 0);
    const std::size_t length = std::size_t(terminator - data);
    if (length > std::size_t(std::numeric_limits<int>::max())) return QString();
    return QString::fromLocal8Bit(reinterpret_cast<const char*>(data), int(length));
}

struct NativeLhaEntry {
    QStringList pathParts;
    bool directory = false;
    std::vector<std::uint8_t> data;
};

bool decodeNativeArchive(const std::vector<std::uint8_t>& archive, std::size_t startOffset,
                         std::vector<NativeLhaEntry>* entries)
{
    if (!entries || startOffset > std::size_t(archive.size()) ||
        std::size_t(archive.size()) - startOffset < 27)
        return false;
    std::size_t offset = startOffset;
    std::size_t decodedTotal = 0;
    while (offset < std::size_t(archive.size())) {
        if (entries->size() >= kMaxEntryCount) return false;
        if (byteAt(archive, offset) == 0) break;
        if (std::size_t(archive.size()) - offset < 27) return false;
        const std::uint8_t headerLevel = byteAt(archive, offset + 20);
        if (headerLevel == 3 && std::size_t(archive.size()) - offset < 32)
            return false;
        const std::size_t headerSize = headerLevel == 2
            ? readLe16(archive, offset)
            : headerLevel == 3 ? readLe32(archive, offset + 24)
                               : byteAt(archive, offset);
        const std::size_t headerEnd = offset + headerSize +
            ((headerLevel == 2 || headerLevel == 3) ? 0 : 2);
        const std::size_t minimumHeaderSize = headerLevel == 3 ? 32 : 24;
        if (headerEnd > std::size_t(archive.size()) ||
            headerSize < minimumHeaderSize || headerLevel > 3)
            return false;
        if (headerLevel < 2) {
            std::uint8_t sum = 0;
            for (std::size_t i = offset + 2; i < headerEnd; ++i) sum += byteAt(archive, i);
            if (sum != byteAt(archive, offset + 1)) return false;
        }

        const char* method = reinterpret_cast<const char*>(archive.data() + offset + 2);
        char methodName[6] = {};
        std::memcpy(methodName, method, 5);
        const bool stored = std::memcmp(method, "-lh0-", 5) == 0 ||
                            std::memcmp(method, "-lz4-", 5) == 0 ||
                            std::memcmp(method, "-pm0-", 5) == 0;
        const bool compressed = std::memcmp(method, "-lz5-", 5) == 0 ||
                                std::memcmp(method, "-lzs-", 5) == 0 ||
                                std::memcmp(method, "-lh1-", 5) == 0 ||
                                std::memcmp(method, "-lh2-", 5) == 0 ||
                                std::memcmp(method, "-lh3-", 5) == 0 ||
                                std::memcmp(method, "-lhx-", 5) == 0 ||
                                std::memcmp(method, "-lh4-", 5) == 0 ||
                                std::memcmp(method, "-lh5-", 5) == 0 ||
                                std::memcmp(method, "-lh6-", 5) == 0 ||
                                std::memcmp(method, "-lh7-", 5) == 0 ||
                                std::memcmp(method, "-pm1-", 5) == 0 ||
                                std::memcmp(method, "-pm2-", 5) == 0;
        const bool directory = std::memcmp(method, "-lhd-", 5) == 0;
        if (!stored && !compressed && !directory) return false;

        const std::uint32_t packedSize = readLe32(archive, offset + 7);
        const std::uint32_t originalSize = readLe32(archive, offset + 11);
        const std::size_t nameLength = headerLevel < 2 ? byteAt(archive, offset + 21) : 0;
        const std::size_t nameOffset = offset + 22;
        if (headerLevel < 2 && nameOffset + nameLength + 2 > headerEnd) return false;
        QString memberName = headerLevel < 2
            ? localCString(archive.data() + nameOffset, nameLength) : QString();
        QString directoryName;
        const std::uint16_t expectedCrc = readLe16(archive,
            headerLevel >= 2 ? offset + 21 : nameOffset + nameLength);

        std::size_t extensionBytes = 0;
        if (headerLevel >= 2) {
            const std::size_t sizeWidth = headerLevel == 3 ? 4 : 2;
            const std::size_t typeOffset = sizeWidth;
            std::size_t extensionOffset = offset + (headerLevel == 3 ? 28 : 24);
            if (extensionOffset + sizeWidth > headerEnd) return false;
            std::size_t extensionSize = headerLevel == 3
                ? readLe32(archive, extensionOffset)
                : readLe16(archive, extensionOffset);
            while (extensionSize != 0) {
                if (extensionSize < sizeWidth + 1 ||
                    extensionSize > headerEnd - extensionOffset)
                    return false;
                const std::uint8_t type = byteAt(archive, extensionOffset + typeOffset);
                const std::size_t textLength = extensionSize - sizeWidth - 1;
                if (type == 1) {
                    memberName = localCString(archive.data() + extensionOffset + typeOffset + 1,
                                              textLength);
                } else if (type == 2) {
                    std::vector<std::uint8_t> rawPath(
                        archive.begin() + std::ptrdiff_t(extensionOffset + typeOffset + 1),
                        archive.begin() + std::ptrdiff_t(extensionOffset + typeOffset + 1 + textLength));
                    std::replace(rawPath.begin(), rawPath.end(), std::uint8_t(0xff), std::uint8_t('/'));
                    while (!rawPath.empty() && rawPath.back() == '/') rawPath.pop_back();
                    directoryName = localCString(rawPath.data(), rawPath.size());
                }
                extensionOffset += extensionSize;
                if (extensionOffset + sizeWidth > headerEnd) return false;
                extensionSize = headerLevel == 3
                    ? readLe32(archive, extensionOffset)
                    : readLe16(archive, extensionOffset);
            }
            if (extensionOffset + sizeWidth != headerEnd) return false;
        } else if (headerLevel == 1) {
            std::size_t extensionOffset = headerEnd;
            std::size_t extensionSize = readLe16(archive, headerEnd - 2);
            while (extensionSize != 0) {
                if (extensionSize < 3 || extensionSize > std::size_t(archive.size()) - extensionOffset ||
                    extensionBytes + extensionSize > packedSize)
                    return false;
                const std::uint8_t type = byteAt(archive, extensionOffset);
                const std::size_t textLength = extensionSize - 3;
                if (type == 1) {
                    memberName = localCString(archive.data() + extensionOffset + 1, textLength);
                } else if (type == 2) {
                    std::vector<std::uint8_t> rawPath(
                        archive.begin() + std::ptrdiff_t(extensionOffset + 1),
                        archive.begin() + std::ptrdiff_t(extensionOffset + 1 + textLength));
                    std::replace(rawPath.begin(), rawPath.end(), std::uint8_t(0xff), std::uint8_t('/'));
                    while (!rawPath.empty() && rawPath.back() == '/') rawPath.pop_back();
                    directoryName = localCString(rawPath.data(), rawPath.size());
                } else if (type == 0x40 || type == 0x41 || type == 0x42) {
                    return false;
                }
                const std::uint16_t nextSize = readLe16(archive, extensionOffset + extensionSize - 2);
                extensionOffset += extensionSize;
                extensionBytes += extensionSize;
                extensionSize = nextSize;
            }
        }

        if (directoryName.isEmpty()) {
            directoryName = memberName;
        } else if (!memberName.isEmpty()) {
            directoryName += QLatin1Char('/') + memberName;
        }
        const QStringList pathParts = safePathParts(directoryName);
        if (pathParts.isEmpty()) return false;

        const std::size_t payloadOffset = headerEnd +
            (headerLevel == 1 ? extensionBytes : 0);
        const std::size_t payloadSize = std::size_t(packedSize) -
            (headerLevel == 1 ? extensionBytes : 0);
        if (payloadOffset > std::size_t(archive.size()) ||
            payloadSize > std::size_t(archive.size()) - payloadOffset ||
            std::size_t(originalSize) > std::numeric_limits<std::size_t>::max() - decodedTotal)
            return false;

        NativeLhaEntry entry;
        entry.pathParts = pathParts;
        entry.directory = directory;
        if (!directory) {
            if (stored) {
                if (payloadSize != originalSize) return false;
                entry.data.assign(archive.data() + payloadOffset,
                                  archive.data() + payloadOffset + payloadSize);
            } else {
                char decoderMethod[6] = {};
                std::memcpy(decoderMethod, methodName, sizeof(decoderMethod));
                if (std::memcmp(method, "-lh7-", 5) == 0 && headerLevel == 1 &&
                    byteAt(archive, headerEnd - 3) == 0x20)
                    std::memcpy(decoderMethod, "-lk7-", 5);
                if (!peare::compression::decompressLh(
                           decoderMethod,
                           archive.data() + payloadOffset,
                           payloadSize, originalSize, &entry.data))
                    return false;
            }
            if (entry.data.size() != originalSize ||
                lhaCrc16(entry.data.data(), entry.data.size()) != expectedCrc) return false;
            decodedTotal += entry.data.size();
        }
        entries->push_back(std::move(entry));
        offset = headerEnd + packedSize;
        if (offset > std::size_t(archive.size())) return false;
    }
    return !entries->empty();
}

}  // namespace

bool LhaModule::hasHeader(const QByteArray& data)
{
    return hasMethodMarker(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()));
}

ModulePtr LhaModule::openNative(const std::vector<std::uint8_t>& data,
                                const QString& logicalName)
{
    const std::size_t limit = std::min<std::size_t>(data.size(), 256 * 1024 + 16);
    std::vector<NativeLhaEntry> entries;
    bool opened = false;
    std::size_t attempts = 0;
    for (std::size_t candidate = 0; candidate + 27 <= limit; ++candidate) {
        if (!hasMethodMarkerAt(data, candidate)) continue;
        if (++attempts > 256) break;
        entries.clear();
        if (decodeNativeArchive(data, std::size_t(candidate), &entries)) {
            opened = true;
            break;
        }
    }
    if (!opened) return {};

    auto module = peare::makeUnique<LhaModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::LHA;
    module->info_.description = QStringLiteral("LHA / LZH archive");
    std::size_t decodedTotal = 0;
    for (NativeLhaEntry& source : entries) {
        ResourceEntry entry;
        entry.name = source.pathParts.last();
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::LHA;
        entry.hierarchyPath = source.pathParts;
        entry.hierarchyPath.removeLast();
        if (source.directory) {
            entry.type = QStringLiteral("LHA_DIR");
            entry.isDirectory = true;
        } else {
            if (source.data.size() > std::numeric_limits<std::size_t>::max() - decodedTotal)
                return {};
            decodedTotal += source.data.size();
            entry.type = QStringLiteral("LHA_FILE");
            entry.isEmbeddedFile = true;
            entry.dataSize = source.data.size();
            entry.content = std::make_shared<fs::MemoryStore>(std::move(source.data));
        }
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

ModulePtr LhaModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<LhaModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::LHA;
        module->info_.error = QStringLiteral("Could not read LHA archive");
        return module;
    }
    return open(source, filePath);
}

ModulePtr LhaModule::open(const fs::ByteStorePtr& source, const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<LhaModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::LHA;
        module->info_.error = QStringLiteral("Invalid LHA byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

ModulePtr LhaModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

ModulePtr LhaModule::open(std::vector<std::uint8_t> data,
                          const QString& logicalName)
{
    auto module = peare::makeUnique<LhaModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::LHA;
    module->info_.description = QStringLiteral("LHA / LZH archive");
    ModulePtr native = openNative(data, logicalName);
    if (native) return native;
    module->info_.error = QStringLiteral("Unsupported, invalid, or corrupt LHA archive");
    return ModulePtr(std::move(module));
}

}  // namespace peare
