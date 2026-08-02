#include "DmgModule.h"

#include "Compat.h"

#include "../fs/DmgDisk.h"

#include <QByteArray>
#include <QFile>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace peare {
namespace {

struct BlkxBlock {
    std::int64_t firstSector = 0;
    std::int64_t sectorCount = 0;
    std::vector<fs::DmgRun> runs;
};

std::uint32_t be32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return (std::uint32_t(b[o]) << 24) | (std::uint32_t(b[o + 1]) << 16) |
           (std::uint32_t(b[o + 2]) << 8) | std::uint32_t(b[o + 3]);
}

std::uint64_t be64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return (std::uint64_t(be32(b, o)) << 32) | be32(b, o + 4);
}

std::int64_t sbe64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::int64_t>(be64(b, o));
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

std::vector<QByteArray> blkxDataFromPlist(const QByteArray& xml, QString* error) {
    std::vector<QByteArray> out;
    QXmlStreamReader reader(xml);
    QString lastKey;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement()) {
            if (reader.name() == QLatin1String("key")) {
                lastKey = reader.readElementText();
            } else if (reader.name() == QLatin1String("data") &&
                       lastKey == QLatin1String("Data")) {
                const QByteArray base64 =
                    reader.readElementText().remove(QChar::Space).remove(QChar::LineFeed)
                        .remove(QChar::CarriageReturn).remove(QChar('\t')).toLatin1();
                out.push_back(QByteArray::fromBase64(base64));
                lastKey.clear();
            }
        }
    }
    if (reader.hasError()) {
        if (error) *error = reader.errorString();
        return std::vector<QByteArray>();
    }
    return out;
}

bool parseBlkx(const QByteArray& bytes, BlkxBlock* out) {
    if (!out || bytes.size() < 204) return false;
    std::vector<std::uint8_t> b(reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                               reinterpret_cast<const std::uint8_t*>(bytes.constData()) + bytes.size());
    const std::uint32_t signature = be32(b, 0);
    if (signature != 0x6d697368U) return false;  // "mish"
    out->firstSector = sbe64(b, 8);
    out->sectorCount = sbe64(b, 16);
    const std::uint32_t runs = be32(b, 200);
    if (runs > 100000 || 204 + std::uint64_t(runs) * 40 > std::uint64_t(bytes.size()))
        return false;
    for (std::uint32_t i = 0; i < runs; ++i) {
        const std::size_t o = 204 + std::size_t(i) * 40;
        fs::DmgRun r;
        r.type = be32(b, o);
        r.sectorStart = sbe64(b, o + 8);
        r.sectorCount = sbe64(b, o + 16);
        r.compOffset = sbe64(b, o + 24);
        r.compLength = sbe64(b, o + 32);
        out->runs.push_back(r);
    }
    return out->sectorCount >= 0;
}

}  // namespace

ModulePtr DmgModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<DmgModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::DMG;
    module->info_.description = QStringLiteral("Apple UDIF disk image");
    if (!file || file->capacity() < 512) {
        module->info_.error = QStringLiteral("Truncated DMG");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> trailer = file->readRange(file->capacity() - 512, 512);
    if (be32(trailer, 0) != 0x6b6f6c79U) {
        module->info_.error = QStringLiteral("Invalid DMG trailer");
        return ModulePtr(std::move(module));
    }
    const std::uint64_t xmlOffset = be64(trailer, 216);
    const std::uint64_t xmlLength = be64(trailer, 224);
    if (xmlLength == 0 || xmlOffset + xmlLength > std::uint64_t(file->capacity()) ||
        xmlLength > std::uint64_t(std::numeric_limits<int>::max())) {
        module->info_.error = QStringLiteral("Invalid DMG resource fork");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> xmlBytes = file->readRange(std::int64_t(xmlOffset),
                                                               std::int64_t(xmlLength));
    const QByteArray xml(reinterpret_cast<const char*>(xmlBytes.data()),
                         static_cast<int>(xmlBytes.size()));
    QString xmlError;
    const std::vector<QByteArray> datas = blkxDataFromPlist(xml, &xmlError);
    if (datas.empty()) {
        module->info_.error =
            xmlError.isEmpty() ? QStringLiteral("No DMG blkx resources found") : xmlError;
        return ModulePtr(std::move(module));
    }

    for (std::size_t i = 0; i < datas.size(); ++i) {
        BlkxBlock block;
        if (!parseBlkx(datas[i], &block) || block.runs.empty()) continue;
        auto store = std::make_shared<fs::DmgRunStore>(file, block.sectorCount, block.runs);
        if (!store->valid()) continue;

        ResourceEntry entry;
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.isEmbeddedFile = true;
        entry.name = QStringLiteral("UDIF block %1").arg(i + 1);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(block.firstSector) * 512;
        entry.dataSize = quint64(block.sectorCount) * 512;
        entry.content = store;
        module->resources_.push_back(std::move(entry));
    }

    if (module->resources_.isEmpty())
        module->info_.error = QStringLiteral("No readable DMG blkx resources found");
    return ModulePtr(std::move(module));
}

ModulePtr DmgModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<DmgModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::DMG;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
