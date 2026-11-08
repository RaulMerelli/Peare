#include "DxbcModule.h"

#include "Compat.h"
#include "../fs/DiscStore.h"

#include <QByteArray>
#include <QStringList>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace peare {
namespace {

constexpr std::uint64_t kHeaderSize = 32;
constexpr std::uint32_t kMaxChunks = 65536;

std::uint32_t readLe32(const std::uint8_t* data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

bool readExact(const fs::ByteStorePtr& source, std::uint64_t offset,
               std::uint8_t* output, std::size_t length)
{
    if (!source || source->capacity() < 0 ||
        offset > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
        length > std::size_t(std::numeric_limits<int>::max()) ||
        offset > std::uint64_t(source->capacity()) ||
        length > std::uint64_t(source->capacity()) - offset)
        return false;
    std::size_t done = 0;
    while (done < length) {
        const int request = static_cast<int>(length - done);
        const int count = source->read(static_cast<std::int64_t>(offset + done),
                                       output + done, request);
        if (count <= 0 || count > request)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

struct Chunk {
    std::uint32_t offset = 0;
    std::uint32_t size = 0;
    QByteArray type;
    std::uint32_t index = 0;
};

bool validFourCc(const std::uint8_t* data)
{
    for (unsigned i = 0; i < 4; ++i) {
        if (data[i] < 0x20 || data[i] > 0x7e)
            return false;
    }
    return true;
}

}  // namespace

ModulePtr DxbcModule::open(const fs::ByteStorePtr& source, const QString& sourceName)
{
    auto module = peare::makeUnique<DxbcModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::DXBC;
    module->info_.description = QStringLiteral("DirectX Shader Container");

    std::uint8_t header[kHeaderSize] = {};
    if (!readExact(source, 0, header, sizeof(header)) ||
        std::memcmp(header, "DXBC", 4) != 0) {
        module->info_.error = QStringLiteral("Invalid or truncated DXBC header");
        return ModulePtr(std::move(module));
    }

    const std::uint32_t version = readLe32(header + 20);
    const std::uint32_t declaredSize = readLe32(header + 24);
    const std::uint32_t chunkCount = readLe32(header + 28);
    const std::uint64_t tableSize = std::uint64_t(chunkCount) * 4;
    const std::uint64_t tableEnd = kHeaderSize + tableSize;
    if (version != 1 || declaredSize < kHeaderSize ||
        declaredSize > std::uint64_t(source->capacity()) ||
        chunkCount > kMaxChunks || tableEnd > declaredSize) {
        module->info_.error = QStringLiteral("Invalid DXBC header fields or chunk table");
        return ModulePtr(std::move(module));
    }

    std::vector<std::uint8_t> table(static_cast<std::size_t>(tableSize));
    if (!table.empty() && !readExact(source, kHeaderSize, table.data(), table.size())) {
        module->info_.error = QStringLiteral("Could not read the DXBC chunk table");
        return ModulePtr(std::move(module));
    }

    std::vector<Chunk> chunks;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    chunks.reserve(chunkCount);
    ranges.reserve(chunkCount);
    for (std::uint32_t i = 0; i < chunkCount; ++i) {
        const std::uint32_t offset = readLe32(table.data() + std::size_t(i) * 4);
        if (offset < tableEnd || std::uint64_t(offset) > declaredSize - 8) {
            module->info_.error = QStringLiteral("DXBC chunk offset is outside the container");
            return ModulePtr(std::move(module));
        }
        std::uint8_t chunkHeader[8] = {};
        if (!readExact(source, offset, chunkHeader, sizeof(chunkHeader)) ||
            !validFourCc(chunkHeader)) {
            module->info_.error = QStringLiteral("Invalid DXBC chunk header");
            return ModulePtr(std::move(module));
        }
        const std::uint32_t size = readLe32(chunkHeader + 4);
        const std::uint64_t end = std::uint64_t(offset) + 8 + size;
        if (end > declaredSize) {
            module->info_.error = QStringLiteral("DXBC chunk extends beyond the container");
            return ModulePtr(std::move(module));
        }
        Chunk chunk;
        chunk.offset = offset;
        chunk.size = size;
        chunk.type = QByteArray(reinterpret_cast<const char*>(chunkHeader), 4);
        chunk.index = i;
        chunks.push_back(std::move(chunk));
        ranges.emplace_back(offset, end);
    }

    std::sort(ranges.begin(), ranges.end());
    for (std::size_t i = 1; i < ranges.size(); ++i) {
        if (ranges[i].first < ranges[i - 1].second) {
            module->info_.error = QStringLiteral("DXBC chunks overlap");
            return ModulePtr(std::move(module));
        }
    }

    QStringList lines;
    lines.append(QStringLiteral("Format: DirectX Shader Container (DXBC)"));
    lines.append(QStringLiteral("Container version: %1").arg(version));
    lines.append(QStringLiteral("Declared size: %1 bytes").arg(declaredSize));
    lines.append(QStringLiteral("Source size: %1 bytes").arg(source->capacity()));
    lines.append(QStringLiteral("Chunk count: %1").arg(chunkCount));
    lines.append(QStringLiteral("Checksum: present; not verified"));
    for (const Chunk& chunk : chunks) {
        lines.append(QStringLiteral("Chunk %1: %2, offset %3, size %4")
            .arg(chunk.index).arg(QString::fromLatin1(chunk.type))
            .arg(chunk.offset).arg(chunk.size));
    }
    ResourceEntry metadata;
    metadata.type = QStringLiteral("DXBC_METADATA");
    metadata.name = QStringLiteral("DXBC metadata.txt");
    metadata.language = QStringLiteral("neutral");
    metadata.format = ModuleFormat::DXBC;
    metadata.data = lines.join(QLatin1Char('\n')).toUtf8();
    metadata.dataSize = static_cast<quint64>(metadata.data.size());
    metadata.isEmbeddedFile = true;
    metadata.content = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(metadata.data.constData()),
        static_cast<std::size_t>(metadata.data.size()));
    module->resources_.append(std::move(metadata));

    for (const Chunk& chunk : chunks) {
        ResourceEntry entry;
        entry.type = QStringLiteral("DXBC_CHUNK");
        entry.name = QStringLiteral("Chunk %1 %2.bin")
            .arg(chunk.index, 2, 10, QLatin1Char('0'))
            .arg(QString::fromLatin1(chunk.type));
        entry.format = ModuleFormat::DXBC;
        entry.dataOffset = std::uint64_t(chunk.offset) + 8;
        entry.dataSize = chunk.size;
        entry.isEmbeddedFile = true;
        entry.content = std::make_shared<fs::SubStore>(source,
            static_cast<std::int64_t>(entry.dataOffset), chunk.size);
        module->resources_.append(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
