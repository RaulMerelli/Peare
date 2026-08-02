#include "SdiModule.h"
#include "Compat.h"

#include <QFile>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kSector = 512;
const std::int64_t kHeaderSize = 512;
const std::int64_t kSectionRecordSize = 64;

std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(p[0]) |
           (static_cast<std::uint64_t>(p[1]) << 8) |
           (static_cast<std::uint64_t>(p[2]) << 16) |
           (static_cast<std::uint64_t>(p[3]) << 24) |
           (static_cast<std::uint64_t>(p[4]) << 32) |
           (static_cast<std::uint64_t>(p[5]) << 40) |
           (static_cast<std::uint64_t>(p[6]) << 48) |
           (static_cast<std::uint64_t>(p[7]) << 56);
}

std::int64_t le64s(const std::uint8_t* p) {
    return static_cast<std::int64_t>(le64(p));
}

QString sectionType(const std::uint8_t* p) {
    int n = 0;
    while (n < 8 && p[n] != 0) ++n;
    return QString::fromLatin1(reinterpret_cast<const char*>(p), n).trimmed();
}

QString cleanType(QString type) {
    if (type.isEmpty())
        return QStringLiteral("SECTION");
    for (QChar& c : type) {
        if (!c.isLetterOrNumber())
            c = QLatin1Char('_');
    }
    return type.toUpper();
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

}  // namespace

ModulePtr SdiModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<SdiModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::SDI;
    module->info_.description = QStringLiteral("System Deployment Image");
    module->file_ = file;

    if (!file || file->capacity() < kHeaderSize) {
        module->info_.error = QStringLiteral("Truncated SDI file");
        return ModulePtr(std::move(module));
    }

    std::vector<std::uint8_t> header = file->readRange(0, kHeaderSize);
    if (header.size() < kHeaderSize || std::memcmp(header.data(), "$SDI0001", 8) != 0) {
        module->info_.error = QStringLiteral("Invalid SDI signature");
        return ModulePtr(std::move(module));
    }
    const std::int64_t pageAlignment = le64s(header.data() + 0x70);
    if (pageAlignment <= 0 || pageAlignment > 0x100000) {
        module->info_.error = QStringLiteral("Invalid SDI page alignment");
        return ModulePtr(std::move(module));
    }

    const std::int64_t tocOffset = pageAlignment * kSector;
    const std::int64_t tocSize = pageAlignment * kSector;
    if (tocOffset < 0 || tocSize <= 0 || tocOffset + tocSize > file->capacity()) {
        module->info_.error = QStringLiteral("Invalid SDI table of contents");
        return ModulePtr(std::move(module));
    }

    std::vector<std::uint8_t> toc = file->readRange(tocOffset, tocSize);
    if (toc.size() < static_cast<std::size_t>(tocSize)) {
        module->info_.error = QStringLiteral("Truncated SDI table of contents");
        return ModulePtr(std::move(module));
    }

    int index = 0;
    for (std::int64_t pos = 0; pos + kSectionRecordSize <= tocSize; pos += kSectionRecordSize) {
        const std::uint8_t* r = toc.data() + pos;
        if (le64(r) == 0)
            break;
        const QString rawType = sectionType(r);
        const std::int64_t offset = le64s(r + 16);
        const std::int64_t size = le64s(r + 24);
        const std::uint64_t partitionType = le64(r + 32);
        if (offset < 0 || size < 0 || offset + size > file->capacity()) {
            module->info_.error = QStringLiteral("Invalid SDI section extent");
            return ModulePtr(std::move(module));
        }

        ResourceEntry entry;
        entry.type = QStringLiteral("SDI_%1").arg(cleanType(rawType));
        entry.isEmbeddedFile = true;
        entry.name = rawType.isEmpty()
            ? QStringLiteral("Section %1").arg(index + 1)
            : QStringLiteral("Section %1 (%2)").arg(index + 1).arg(rawType);
        if (rawType.compare(QStringLiteral("PART"), Qt::CaseInsensitive) == 0)
            entry.name += QStringLiteral(" type 0x%1").arg(qulonglong(partitionType & 0xff), 2, 16, QLatin1Char('0'));
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(offset);
        entry.dataSize = quint64(size);
        entry.content = std::make_shared<fs::SubStore>(file, offset, size);
        module->resources_.push_back(std::move(entry));
        ++index;
    }

    return ModulePtr(std::move(module));
}

ModulePtr SdiModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<SdiModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SDI;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
