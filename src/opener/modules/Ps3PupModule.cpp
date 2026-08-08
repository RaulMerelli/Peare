#include "Ps3PupModule.h"
#include "Compat.h"
#include "ModuleFormat.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace peare {
namespace {

const std::int64_t kV1HeaderSize = 0x30;
const std::int64_t kV2HeaderSize = 0x80;
const std::uint64_t kMaxSegments = 4096;

struct PupLayout {
    std::uint8_t formatFlag = 0;
    std::uint64_t fixedHeaderSize = 0;
    std::uint64_t segmentTableOffset = 0;
    std::uint64_t digestEntrySize = 0;
    std::uint64_t headerDigestSize = 0;
    QString family;
};

PupLayout layoutForFlag(std::uint8_t flag)
{
    PupLayout layout;
    layout.formatFlag = flag;
    if (flag == 1) {
        layout.fixedHeaderSize = 0x30;
        layout.segmentTableOffset = 0x30;
        layout.digestEntrySize = 0x20;
        layout.headerDigestSize = 0x20; // SHA-1 digest plus 12 bytes padding
        layout.family = QStringLiteral("PS3 layout");
    } else if (flag == 2) {
        layout.fixedHeaderSize = 0x80;
        layout.segmentTableOffset = 0x80;
        layout.digestEntrySize = 0x40;
        layout.headerDigestSize = 0x20;
        layout.family = QStringLiteral("SCE PUP v2 layout");
    }
    return layout;
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t be64(const std::uint8_t* p)
{
    return (std::uint64_t(be32(p)) << 32) | std::uint64_t(be32(p + 4));
}

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p)
{
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

QString hex64(std::uint64_t value, int width = 0)
{
    return QString::number(qulonglong(value), 16).rightJustified(width, QLatin1Char('0')).toUpper();
}

bool checkedAdd(std::uint64_t a, std::uint64_t b, std::uint64_t* out)
{
    if (!out || b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    *out = a + b;
    return true;
}

bool checkedMul(std::uint64_t a, std::uint64_t b, std::uint64_t* out)
{
    if (!out || (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)) return false;
    *out = a * b;
    return true;
}

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    }
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

bool printableText(const QByteArray& bytes)
{
    if (bytes.isEmpty()) return false;
    int printable = 0;
    for (char value : bytes) {
        const unsigned char c = static_cast<unsigned char>(value);
        if (c == 0) return false;
        if (c == '\r' || c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7f))
            ++printable;
    }
    return printable * 10 >= bytes.size() * 9;
}

QString extensionFor(const QByteArray& prefix)
{
    if (prefix.size() >= 4 && std::memcmp(prefix.constData(), "SCE\0", 4) == 0)
        return QStringLiteral(".self");
    if (prefix.size() >= 4 &&
        static_cast<unsigned char>(prefix[0]) == 0x7f &&
        std::memcmp(prefix.constData() + 1, "PKG", 3) == 0)
        return QStringLiteral(".pkg");

    const ModuleFormatInfo nested = ModuleFormatDetector::detectNestedBuffer(prefix);
    switch (nested.format) {
    case ModuleFormat::TAR: return QStringLiteral(".tar");
    case ModuleFormat::ZIP: return QStringLiteral(".zip");
    case ModuleFormat::CAB: return QStringLiteral(".cab");
    case ModuleFormat::PS3_PUP: return QStringLiteral(".pup");
    default: break;
    }

    const QByteArray trimmed = prefix.trimmed();
    if (trimmed.startsWith('<')) return QStringLiteral(".xml");
    if (printableText(prefix)) return QStringLiteral(".txt");
    return QStringLiteral(".bin");
}

QString segmentName(std::uint64_t id, const QByteArray& prefix)
{
    if (id == 0x100) return QStringLiteral("version.txt");
    if (id == 0x101) return QStringLiteral("license.xml");
    if (id == 0x200) return QStringLiteral("ps3swu.self");
    if (id == 0x201) return QStringLiteral("vsh.tar");
    if (id == 0x300) return QStringLiteral("update_files.tar");
    return QStringLiteral("segment_%1%2")
        .arg(hex64(id, id <= 0xffff ? 4 : 16), extensionFor(prefix));
}

QString firmwareVersion(const fs::ByteStorePtr& file, std::uint64_t offset, std::uint64_t size)
{
    if (!file || size == 0 || size > 128 || offset > std::uint64_t(file->capacity())) return {};
    const std::vector<std::uint8_t> bytes = file->readRange(
        std::int64_t(offset), std::int64_t(std::min<std::uint64_t>(size, 128)));
    QByteArray text(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
    const int nul = text.indexOf('\0');
    if (nul >= 0) text.truncate(nul);
    text = text.trimmed();
    return printableText(text) ? QString::fromLatin1(text) : QString();
}

} // namespace

ModulePtr Ps3PupModule::open(const QString& filePath)
{
    return open(storeForFile(filePath), filePath);
}

ModulePtr Ps3PupModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName);
}

ModulePtr Ps3PupModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<Ps3PupModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::PS3_PUP;
    module->info_.description = QStringLiteral("Sony Computer Entertainment Update Package (PUP)");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open PUP package");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    if (file->capacity() < kV1HeaderSize) {
        module->info_.error = QStringLiteral("Truncated PUP header");
        return ModulePtr(std::move(module));
    }

    const std::int64_t prefixSize = std::min<std::int64_t>(file->capacity(), kV2HeaderSize);
    const std::vector<std::uint8_t> header = file->readRange(0, prefixSize);
    if (header.size() < std::size_t(kV1HeaderSize) ||
        std::memcmp(header.data(), "SCEUF\0\0\0", 8) != 0) {
        module->info_.error = QStringLiteral("Invalid PUP magic");
        return ModulePtr(std::move(module));
    }

    const PupLayout layout = layoutForFlag(1);
    if (file->capacity() < std::int64_t(layout.fixedHeaderSize) ||
        header.size() < std::size_t(layout.fixedHeaderSize)) {
        module->info_.error = QStringLiteral("Truncated PUP v%1 header").arg(layout.formatFlag);
        return ModulePtr(std::move(module));
    }

    // PS3 PUP v1 is big-endian; SCE PUP v2 (PS Vita family) is little-endian.
    const bool littleEndian = layout.formatFlag == 2;
    const auto read32 = littleEndian ? le32 : be32;
    const auto read64 = littleEndian ? le64 : be64;
    const std::uint64_t packageVersion = read64(header.data() + 0x08);
    const std::uint64_t imageVersion = read64(header.data() + 0x10);
    const std::uint64_t segmentCount = read64(header.data() + 0x18);
    const std::uint64_t headerLength = read64(header.data() + 0x20);
    const std::uint64_t dataLength = read64(header.data() + 0x28);

    if (segmentCount == 0 || segmentCount > kMaxSegments) {
        module->info_.error = QStringLiteral("Invalid PUP segment count");
        return ModulePtr(std::move(module));
    }

    std::uint64_t segmentTableBytes = 0;
    std::uint64_t digestTableBytes = 0;
    std::uint64_t minimumHeader = 0;
    if (!checkedMul(segmentCount, 0x20, &segmentTableBytes) ||
        !checkedMul(segmentCount, layout.digestEntrySize, &digestTableBytes) ||
        !checkedAdd(layout.segmentTableOffset, segmentTableBytes, &minimumHeader) ||
        !checkedAdd(minimumHeader, digestTableBytes, &minimumHeader) ||
        !checkedAdd(minimumHeader, layout.headerDigestSize, &minimumHeader) ||
        headerLength < minimumHeader || headerLength > std::uint64_t(file->capacity())) {
        module->info_.error = QStringLiteral("Invalid PUP header length");
        return ModulePtr(std::move(module));
    }

    // Some real-world/custom PUP builders encode the field at 0x28 as the
    // data length, while others encode the absolute package length. Do not
    // reject an otherwise valid segment table solely for that convention.
    const std::uint64_t capacity = std::uint64_t(file->capacity());
    std::uint64_t declaredEnd = 0;
    if (checkedAdd(headerLength, dataLength, &declaredEnd) && declaredEnd <= capacity) {
        // canonical PS3 form: dataLength excludes the header
    } else if (dataLength >= headerLength && dataLength <= capacity) {
        declaredEnd = dataLength; // absolute package length variant
    } else {
        declaredEnd = capacity; // validate every segment against the actual file
    }
    const std::uint64_t dataAreaEnd = capacity;
    if (segmentTableBytes > std::uint64_t(std::numeric_limits<int>::max())) {
        module->info_.error = QStringLiteral("PUP segment table is too large");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> table = file->readRange(
        std::int64_t(layout.segmentTableOffset), std::int64_t(segmentTableBytes));
    if (table.size() != std::size_t(segmentTableBytes)) {
        module->info_.error = QStringLiteral("Truncated PUP segment table");
        return ModulePtr(std::move(module));
    }

    QString detectedVersion;
    for (std::uint64_t i = 0; i < segmentCount; ++i) {
        const std::uint8_t* entry = table.data() + std::size_t(i * 0x20);
        const std::uint64_t id = read64(entry);
        const std::uint64_t rawOffset = read64(entry + 0x08);
        const std::uint64_t size = read64(entry + 0x10);
        const std::uint32_t signAlgorithm = read32(entry + 0x18);
        std::uint64_t offset = rawOffset;
        // Official packages use absolute offsets. A few third-party builders
        // store offsets relative to the data area; accept that only when the
        // absolute interpretation is impossible.
        const bool absoluteOk = offset >= headerLength && offset <= dataAreaEnd &&
                                size <= dataAreaEnd - offset;
        if (size != 0 && !absoluteOk && rawOffset <= dataAreaEnd - headerLength &&
            size <= dataAreaEnd - headerLength - rawOffset)
            offset = headerLength + rawOffset;

        const bool emptyRangeValid = size == 0 &&
            (offset == 0 || (offset >= headerLength && offset <= dataAreaEnd));
        const bool populatedRangeValid = size != 0 && offset >= headerLength &&
            offset <= dataAreaEnd && size <= dataAreaEnd - offset;
        if (!emptyRangeValid && !populatedRangeValid) {
            module->info_.error = QStringLiteral("Invalid PUP segment %1 range").arg(i);
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }

        QByteArray prefix;
        if (size != 0) {
            const std::vector<std::uint8_t> sample = file->readRange(
                std::int64_t(offset), std::int64_t(std::min<std::uint64_t>(size, 512)));
            prefix = QByteArray(reinterpret_cast<const char*>(sample.data()), int(sample.size()));
        }

        ResourceEntry resource;
        resource.type = QStringLiteral("PUP_SEGMENT");
        resource.name = segmentName(id, prefix);
        if (signAlgorithm == 0)
            resource.language = QStringLiteral("HMAC-SHA1");
        else if (signAlgorithm == 2)
            resource.language = QStringLiteral("HMAC-SHA256");
        else
            resource.language = QStringLiteral("signature algorithm %1").arg(signAlgorithm);
        resource.dataOffset = quint64(offset);
        resource.dataSize = quint64(size);
        resource.baseId = int(i);
        resource.format = ModuleFormat::PS3_PUP;
        resource.isEmbeddedFile = size != 0;
        resource.hierarchyPath.clear();
        if (size != 0) {
            resource.content = std::make_shared<fs::SubStore>(
                file, std::int64_t(offset), std::int64_t(size));
        }
        module->resources_.push_back(std::move(resource));

        if (id == 0x100 && detectedVersion.isEmpty() && size != 0)
            detectedVersion = firmwareVersion(file, offset, size);
    }

    QString description;
    if (layout.formatFlag == 1) {
        description = QStringLiteral(
            "PlayStation 3 Update Package (PUP v1) — %1 segments; header 0x%2; "
            "data %3 bytes; package version 0x%4; image version 0x%5")
            .arg(segmentCount)
            .arg(hex64(headerLength))
            .arg(dataLength)
            .arg(hex64(packageVersion))
            .arg(hex64(imageVersion));
    } else {
        const std::uint32_t version = read32(header.data() + 0x10);
        const std::uint32_t build = read32(header.data() + 0x14);
        description = QStringLiteral(
            "Sony Computer Entertainment Update Package (PUP v2) — %1 segments; "
            "header 0x%2; data %3 bytes; format version 0x%4; version 0x%5; build %6")
            .arg(segmentCount)
            .arg(hex64(headerLength))
            .arg(dataLength)
            .arg(hex64(packageVersion))
            .arg(hex64(version, 8))
            .arg(build);
    }
    if (!detectedVersion.isEmpty())
        description += QStringLiteral("; firmware %1").arg(detectedVersion);
    if (declaredEnd < capacity)
        description += QStringLiteral("; %1 bytes beyond declared package length")
            .arg(capacity - declaredEnd);
    module->info_.description = description;
    return ModulePtr(std::move(module));
}

} // namespace peare
