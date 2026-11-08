#include "XdbfModule.h"

#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace peare {
namespace {

const std::uint64_t kHeaderSize = 24;
const std::uint64_t kEntrySize = 18;
const std::uint64_t kFreeEntrySize = 8;
const std::uint32_t kMaxTableEntries = 1000000;

bool readExact(const fs::ByteStorePtr& source, std::uint64_t offset,
               std::uint8_t* output, std::size_t length)
{
    if (!source || source->capacity() < 0 ||
        offset > std::uint64_t(source->capacity()) ||
        std::uint64_t(length) > std::uint64_t(source->capacity()) - offset)
        return false;
    std::size_t done = 0;
    while (done < length) {
        const int request = static_cast<int>(std::min<std::size_t>(length - done, 1024 * 1024));
        const int count = source->read(static_cast<std::int64_t>(offset + done), output + done, request);
        if (count <= 0 || count > request)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

std::uint16_t be16(const std::uint8_t* p)
{
    return (std::uint16_t(p[0]) << 8) | p[1];
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | p[3];
}

std::uint64_t be64(const std::uint8_t* p)
{
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

QString namespaceName(std::uint16_t value)
{
    switch (value) {
    case 1: return QStringLiteral("Achievements");
    case 2: return QStringLiteral("Images");
    case 3: return QStringLiteral("Settings");
    case 4: return QStringLiteral("Titles");
    case 5: return QStringLiteral("Strings");
    case 6: return QStringLiteral("Achievement security");
    default: return QStringLiteral("Namespace 0x") +
                   QStringLiteral("%1").arg(value, 4, 16, QLatin1Char('0')).toUpper();
    }
}

QString recordName(std::uint64_t id)
{
    return QStringLiteral("0x") + QStringLiteral("%1").arg(id, 16, 16, QLatin1Char('0')).toUpper();
}

QString decodeUtf16Be(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.size() < 2 || (bytes.size() & 1u) != 0)
        return QString();
    QVector<ushort> text;
    text.reserve(static_cast<int>(bytes.size() / 2));
    for (std::size_t i = 0; i < bytes.size(); i += 2)
        text.push_back(ushort((std::uint16_t(bytes[i]) << 8) | bytes[i + 1]));
    while (!text.isEmpty() && text.last() == 0)
        text.removeLast();
    return QString::fromUtf16(text.constData(), text.size());
}

struct Entry {
    std::uint16_t nameSpace = 0;
    std::uint64_t id = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

} // namespace

ModulePtr XdbfModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr XdbfModule::open(const fs::ByteStorePtr& source, const QString& sourceName)
{
    auto module = makeUnique<XdbfModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::XDBF;
    module->info_.description = QStringLiteral("Xbox 360 Database (XDBF/GPD)");

    std::vector<std::uint8_t> header(kHeaderSize);
    if (!readExact(source, 0, header.data(), header.size()) ||
        std::memcmp(header.data(), "XDBF", 4) != 0) {
        module->info_.error = QStringLiteral("Invalid or truncated XDBF header");
        return module;
    }

    const std::uint32_t version = be32(header.data() + 4);
    const std::uint32_t entryCapacity = be32(header.data() + 8);
    const std::uint32_t entryCount = be32(header.data() + 12);
    const std::uint32_t freeCapacity = be32(header.data() + 16);
    const std::uint32_t freeCount = be32(header.data() + 20);
    if (version != 0x00010000u || entryCapacity > kMaxTableEntries ||
        freeCapacity > kMaxTableEntries || entryCount > entryCapacity ||
        freeCount > freeCapacity) {
        module->info_.error = QStringLiteral("Invalid XDBF version or table counts");
        return module;
    }

    const std::uint64_t fileSize = source && source->capacity() >= 0
        ? static_cast<std::uint64_t>(source->capacity()) : 0;
    const std::uint64_t entryTableOffset = kHeaderSize;
    const std::uint64_t freeTableOffset = entryTableOffset + std::uint64_t(entryCapacity) * kEntrySize;
    const std::uint64_t dataOffset = freeTableOffset + std::uint64_t(freeCapacity) * kFreeEntrySize;
    if (dataOffset > fileSize || std::uint64_t(entryCount) * kEntrySize > fileSize - entryTableOffset) {
        module->info_.error = QStringLiteral("XDBF tables exceed the file bounds");
        return module;
    }

    std::vector<std::uint8_t> table(std::size_t(entryCount) * kEntrySize);
    if (!table.empty() && !readExact(source, entryTableOffset, table.data(), table.size())) {
        module->info_.error = QStringLiteral("Could not read the XDBF entry table");
        return module;
    }

    QVector<Entry> entries;
    entries.reserve(static_cast<int>(entryCount));
    QSet<QString> uniqueEntries;
    std::uint32_t namespaceCounts[7] = {};
    QString title;
    for (std::uint32_t i = 0; i < entryCount; ++i) {
        const std::uint8_t* record = table.data() + std::size_t(i) * kEntrySize;
        Entry entry;
        entry.nameSpace = be16(record);
        entry.id = be64(record + 2);
        entry.offset = be32(record + 10);
        entry.length = be32(record + 14);
        if (std::uint64_t(entry.offset) > fileSize - dataOffset ||
            std::uint64_t(entry.length) > fileSize - dataOffset - entry.offset) {
            module->info_.error = QStringLiteral("XDBF entry %1 is outside the data area").arg(i);
            return module;
        }
        const QString identity = QStringLiteral("%1:%2").arg(entry.nameSpace).arg(recordName(entry.id));
        if (uniqueEntries.contains(identity)) {
            module->info_.error = QStringLiteral("XDBF contains a duplicate namespace and record ID");
            return module;
        }
        uniqueEntries.insert(identity);
        if (entry.nameSpace < 7)
            ++namespaceCounts[entry.nameSpace];
        if (entry.nameSpace == 5 && entry.id == 0x8000 && entry.length <= 1024 * 1024) {
            std::vector<std::uint8_t> value(entry.length);
            if (!value.empty() && readExact(source, dataOffset + entry.offset, value.data(), value.size()))
                title = decodeUtf16Be(value);
        }
        entries.push_back(entry);
    }

    QString summary = QStringLiteral("Format: Xbox 360 Database (XDBF/GPD)\nVersion: 1.0\nRecords: %1\n")
        .arg(entryCount);
    if (!title.isEmpty())
        summary += QStringLiteral("Title: %1\n").arg(title);
    summary += QStringLiteral("\nNamespaces:\n");
    for (std::uint16_t i = 1; i <= 6; ++i) {
        if (namespaceCounts[i])
            summary += QStringLiteral("%1: %2\n").arg(namespaceName(i)).arg(namespaceCounts[i]);
    }

    ResourceEntry metadata;
    metadata.type = QStringLiteral("XDBF_METADATA");
    metadata.name = QStringLiteral("summary.txt");
    metadata.language = QStringLiteral("neutral");
    metadata.format = ModuleFormat::XDBF;
    metadata.hierarchyPath = QStringList() << QStringLiteral("Metadata") << metadata.name;
    metadata.data = summary.toUtf8();
    metadata.dataSize = static_cast<quint64>(metadata.data.size());
    module->resources_.push_back(std::move(metadata));

    for (const Entry& entry : entries) {
        ResourceEntry resource;
        resource.type = entry.nameSpace == 2 ? QStringLiteral("XDBF_IMAGE") :
                        entry.nameSpace == 5 ? QStringLiteral("XDBF_STRING") :
                        QStringLiteral("XDBF_RECORD");
        resource.name = recordName(entry.id);
        resource.language = QStringLiteral("neutral");
        resource.dataOffset = dataOffset + entry.offset;
        resource.dataSize = entry.length;
        resource.format = ModuleFormat::XDBF;
        resource.hierarchyPath = QStringList() << namespaceName(entry.nameSpace) << resource.name;
        resource.content = std::make_shared<fs::SubStore>(source,
            static_cast<std::int64_t>(resource.dataOffset), static_cast<std::int64_t>(resource.dataSize));
        if (entry.nameSpace == 5 && entry.length <= 1024 * 1024) {
            std::vector<std::uint8_t> value(entry.length);
            if (value.empty() || readExact(source, resource.dataOffset, value.data(), value.size()))
                resource.data = QByteArray(reinterpret_cast<const char*>(value.data()), static_cast<int>(value.size()));
        }
        module->resources_.push_back(std::move(resource));
    }
    return module;
}

} // namespace peare
