#include "ResourceForkModule.h"

#include <QFile>

namespace peare {
namespace {

quint16 readBe16(const QByteArray& data, int offset)
{
    return quint16(uchar(data.at(offset)) << 8) | uchar(data.at(offset + 1));
}

quint32 readBe24(const QByteArray& data, int offset)
{
    return (quint32(uchar(data.at(offset))) << 16) |
           (quint32(uchar(data.at(offset + 1))) << 8) | uchar(data.at(offset + 2));
}

quint32 readBe32(const QByteArray& data, int offset)
{
    return (quint32(uchar(data.at(offset))) << 24) |
           (quint32(uchar(data.at(offset + 1))) << 16) |
           (quint32(uchar(data.at(offset + 2))) << 8) | uchar(data.at(offset + 3));
}

bool rangeFits(quint64 offset, quint64 length, quint64 total)
{
    return offset <= total && length <= total - offset;
}

bool extractResources(const QByteArray& data, QVector<ResourceEntry>* output)
{
    if (!output || data.size() < 128 || data.size() > 128 * 1024 * 1024)
        return false;
    const quint32 dataOffset = readBe32(data, 0);
    const quint32 mapOffset = readBe32(data, 4);
    const quint32 dataLength = readBe32(data, 8);
    const quint32 mapLength = readBe32(data, 12);
    if (dataOffset < 16 || mapOffset < 16 || mapLength < 30 ||
        !rangeFits(dataOffset, dataLength, quint64(data.size())) ||
        !rangeFits(mapOffset, mapLength, quint64(data.size())) ||
        (quint64(dataOffset) < quint64(mapOffset) + mapLength &&
         quint64(mapOffset) < quint64(dataOffset) + dataLength))
        return false;

    const int map = int(mapOffset);
    if (data.mid(map, 16) != data.left(16) && data.mid(map, 16) != QByteArray(16, '\0'))
        return false;
    const quint16 typeListOffset = readBe16(data, map + 24);
    const quint16 nameListOffset = readBe16(data, map + 26);
    if (typeListOffset < 28 || nameListOffset < typeListOffset || nameListOffset > mapLength)
        return false;
    const quint16 countMinusOne = readBe16(data, map + typeListOffset);
    if (countMinusOne == 0xffff)
        return false;
    const quint32 typeCount = quint32(countMinusOne) + 1;
    const quint64 entriesStart = quint64(typeListOffset) + 2;
    const quint64 referencesStart = entriesStart + quint64(typeCount) * 8;
    if (typeCount > 4096 || referencesStart > nameListOffset)
        return false;

    QVector<ResourceEntry> parsed;
    quint64 totalCount = 0;
    for (quint32 typeIndex = 0; typeIndex < typeCount; ++typeIndex) {
        const int typeEntry = map + int(entriesStart + quint64(typeIndex) * 8);
        const quint16 itemCountMinusOne = readBe16(data, typeEntry + 4);
        if (itemCountMinusOne == 0xffff)
            return false;
        const quint32 itemCount = quint32(itemCountMinusOne) + 1;
        totalCount += itemCount;
        if (totalCount > 65536)
            return false;
        const quint16 referenceOffset = readBe16(data, typeEntry + 6);
        const quint64 referenceEnd = quint64(referenceOffset) + quint64(itemCount) * 12;
        if (referenceOffset < 2 + quint64(typeCount) * 8 ||
            referenceEnd > quint64(nameListOffset) - typeListOffset)
            return false;

        const QString type = QString::fromLatin1(data.constData() + typeEntry, 4);
        const int referenceStart = map + int(typeListOffset) + referenceOffset;
        for (quint32 itemIndex = 0; itemIndex < itemCount; ++itemIndex) {
            const int reference = referenceStart + int(itemIndex * 12);
            const qint16 id = qint16(readBe16(data, reference));
            const quint16 nameOffset = readBe16(data, reference + 2);
            const quint32 payloadOffset = readBe24(data, reference + 5);
            if (nameOffset != 0xffff &&
                !rangeFits(quint64(nameListOffset) + nameOffset + 1, 0, mapLength))
                return false;
            if (!rangeFits(payloadOffset, 4, dataLength))
                return false;
            const quint64 record = quint64(dataOffset) + payloadOffset;
            const quint32 size = readBe32(data, int(record));
            if (!rangeFits(quint64(payloadOffset) + 4, size, dataLength))
                return false;

            QString name;
            if (nameOffset != 0xffff) {
                const int namePosition = map + nameListOffset + nameOffset;
                const quint8 nameLength = quint8(data.at(namePosition));
                if (!rangeFits(quint64(nameListOffset) + nameOffset + 1, nameLength, mapLength))
                    return false;
                name = QString::fromLatin1(data.constData() + namePosition + 1, nameLength);
            }
            ResourceEntry entry;
            entry.type = type;
            entry.name = name.isEmpty() ? QStringLiteral("%1-%2.bin").arg(type).arg(id) : name;
            entry.baseId = id;
            entry.dataSize = size;
            entry.format = ModuleFormat::Unknown;
            entry.data = data.mid(int(record + 4), int(size));
            parsed.append(std::move(entry));
        }
    }
    *output = std::move(parsed);
    return true;
}

} // namespace

bool ResourceForkModule::hasCandidateHeader(const QByteArray& prefix, quint64 totalSize)
{
    if (prefix.size() < 16 || totalSize < 128) return false;
    const quint32 dataOffset = readBe32(prefix, 0);
    const quint32 mapOffset = readBe32(prefix, 4);
    const quint32 dataLength = readBe32(prefix, 8);
    const quint32 mapLength = readBe32(prefix, 12);
    return dataOffset >= 16 && mapOffset >= 16 && mapLength >= 30 &&
           rangeFits(dataOffset, dataLength, totalSize) &&
           rangeFits(mapOffset, mapLength, totalSize) &&
           !(quint64(dataOffset) < quint64(mapOffset) + mapLength &&
             quint64(mapOffset) < quint64(dataOffset) + dataLength);
}

bool ResourceForkModule::hasHeader(const QByteArray& data)
{
    if (!hasCandidateHeader(data, quint64(data.size()))) return false;
    QVector<ResourceEntry> resources;
    return extractResources(data, &resources);
}

ModulePtr ResourceForkModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 128LL * 1024 * 1024) {
        std::unique_ptr<ResourceForkModule> module(new ResourceForkModule());
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::MAC_RESOURCE_FORK;
        module->info_.error = file.isOpen()
            ? QStringLiteral("Resource fork input exceeds the 128 MiB limit") : file.errorString();
        return ModulePtr(module.release());
    }
    return open(file.readAll(), filePath);
}

ModulePtr ResourceForkModule::open(const QByteArray& data, const QString& sourceName)
{
    std::unique_ptr<ResourceForkModule> module(new ResourceForkModule());
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::MAC_RESOURCE_FORK;
    if (!extractResources(data, &module->resources_)) {
        module->info_.error = QStringLiteral("Invalid or unsupported Apple resource fork");
        return ModulePtr(module.release());
    }
    module->info_.description = QStringLiteral("Apple resource fork; %1 resource(s)")
        .arg(module->resources_.size());
    return ModulePtr(module.release());
}

} // namespace peare
