#include "PlayReadyXapModule.h"
#include "Compat.h"

#include <QFile>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>

namespace peare {
namespace {

constexpr std::int64_t kEnvelopeSize = 0x20;
constexpr std::uint32_t kMagic = 0x07455250U; // "PRE\x07", little-endian

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
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

QString utf16Le(const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) return {};
    const int unitCount = int(bytes.size() / 2);
    QVector<ushort> units(unitCount);
    for (int i = 0; i < unitCount; ++i)
        units[i] = ushort(bytes[std::size_t(i) * 2]) |
                   (ushort(bytes[std::size_t(i) * 2 + 1]) << 8);
    QString text = QString::fromUtf16(units.constData(), units.size());
    while (!text.isEmpty() && text.back().unicode() == 0) text.chop(1);
    return text;
}

struct DrmMetadata {
    QString version;
    QString algorithm;
    QString keyLength;
    QString kid;
    QString licenseUrl;
    QString checksum;
};

DrmMetadata parseDrmHeader(const QString& text)
{
    DrmMetadata result;
    QXmlStreamReader xml(text);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        const QString name = xml.name().toString();
        if (name == QLatin1String("WRMHEADER"))
            result.version = xml.attributes().value(QLatin1String("version")).toString();
        else if (name == QLatin1String("ALGID"))
            result.algorithm = xml.readElementText().trimmed();
        else if (name == QLatin1String("KEYLEN"))
            result.keyLength = xml.readElementText().trimmed();
        else if (name == QLatin1String("KID")) {
            const QString value = xml.attributes().value(QLatin1String("VALUE")).toString();
            result.kid = value.isEmpty() ? xml.readElementText().trimmed() : value;
        } else if (name == QLatin1String("LA_URL"))
            result.licenseUrl = xml.readElementText().trimmed();
        else if (name == QLatin1String("CHECKSUM"))
            result.checksum = xml.readElementText().trimmed();
    }
    return result;
}

QString metadataLanguage(const DrmMetadata& drm)
{
    QStringList fields;
    if (!drm.version.isEmpty()) fields << QStringLiteral("PlayReady %1").arg(drm.version);
    if (!drm.algorithm.isEmpty()) fields << drm.algorithm;
    if (!drm.keyLength.isEmpty()) fields << QStringLiteral("%1-bit key").arg(drm.keyLength.toUInt() * 8);
    return fields.join(QStringLiteral("; "));
}

} // namespace

bool PlayReadyXapModule::isHeader(const QByteArray& data)
{
    if (data.size() < kEnvelopeSize) return false;
    const auto* p = reinterpret_cast<const std::uint8_t*>(data.constData());
    if (le32(p) != kMagic || le32(p + 4) != 1 || le32(p + 8) != kEnvelopeSize ||
        le32(p + 12) != kEnvelopeSize)
        return false;
    const std::uint32_t drmLength = le32(p + 16);
    const std::uint32_t payloadOffset = le32(p + 20);
    const std::uint32_t encryptedLength = le32(p + 24);
    const std::uint32_t clearLength = le32(p + 28);
    return drmLength >= 32 && (drmLength & 1U) == 0 &&
           payloadOffset == std::uint32_t(kEnvelopeSize) + drmLength &&
           encryptedLength >= clearLength;
}

ModulePtr PlayReadyXapModule::open(const QString& filePath)
{
    return open(storeForFile(filePath), filePath);
}

ModulePtr PlayReadyXapModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName);
}

ModulePtr PlayReadyXapModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<PlayReadyXapModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::XAP;
    module->info_.description = QStringLiteral("Windows Phone XAP (PlayReady encrypted)");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open encrypted XAP package");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;
    if (file->capacity() < kEnvelopeSize) {
        module->info_.error = QStringLiteral("Truncated PlayReady XAP envelope");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> envelope = file->readRange(0, kEnvelopeSize);
    const QByteArray envelopeBytes(reinterpret_cast<const char*>(envelope.data()), int(envelope.size()));
    if (!isHeader(envelopeBytes)) {
        module->info_.error = QStringLiteral("Invalid PlayReady XAP envelope");
        return ModulePtr(std::move(module));
    }

    const std::uint32_t drmLength = le32(envelope.data() + 16);
    const std::uint32_t payloadOffset = le32(envelope.data() + 20);
    const std::uint32_t encryptedLength = le32(envelope.data() + 24);
    const std::uint32_t clearLength = le32(envelope.data() + 28);
    const std::uint64_t payloadEnd = std::uint64_t(payloadOffset) + encryptedLength;
    if (payloadEnd > std::uint64_t(file->capacity())) {
        module->info_.error = QStringLiteral("Truncated encrypted XAP payload");
        return ModulePtr(std::move(module));
    }

    const std::vector<std::uint8_t> drmBytes = file->readRange(kEnvelopeSize, drmLength);
    if (drmBytes.size() != drmLength) {
        module->info_.error = QStringLiteral("Truncated PlayReady header");
        return ModulePtr(std::move(module));
    }
    const QString drmText = utf16Le(drmBytes);
    if (!drmText.startsWith(QStringLiteral("<WRMHEADER"))) {
        module->info_.error = QStringLiteral("Invalid PlayReady XML header");
        return ModulePtr(std::move(module));
    }
    const DrmMetadata drm = parseDrmHeader(drmText);

    ResourceEntry header;
    header.type = QStringLiteral("XAP_METADATA");
    header.name = QStringLiteral("PlayReady header (raw XML)");
    header.language = metadataLanguage(drm);
    header.dataOffset = kEnvelopeSize;
    header.dataSize = drmLength;
    header.format = ModuleFormat::XAP;
    header.isEmbeddedFile = true;
    // The envelope has no directory table outside the encrypted ZIP payload.
    // Keep the two physical regions at the root instead of inventing folders.
    header.hierarchyPath.clear();
    header.content = std::make_shared<fs::SubStore>(file, kEnvelopeSize, drmLength);
    module->resources_.push_back(std::move(header));

    ResourceEntry payload;
    payload.type = QStringLiteral("XAP_ENCRYPTED_ARCHIVE");
    payload.name = QStringLiteral("Encrypted ZIP payload (raw region)");
    payload.language = drm.algorithm.isEmpty()
        ? QStringLiteral("PlayReady content key required")
        : QStringLiteral("%1; PlayReady content key required").arg(drm.algorithm);
    payload.dataOffset = payloadOffset;
    payload.dataSize = encryptedLength;
    payload.format = ModuleFormat::XAP;
    payload.isEmbeddedFile = false;
    payload.hierarchyPath.clear();
    payload.content = std::make_shared<fs::SubStore>(file, payloadOffset, encryptedLength);
    module->resources_.push_back(std::move(payload));

    QStringList details;
    details << QStringLiteral("PlayReady encrypted")
            << QStringLiteral("plaintext %1 bytes").arg(clearLength)
            << QStringLiteral("ciphertext %1 bytes").arg(encryptedLength);
    if (!drm.version.isEmpty()) details << QStringLiteral("header %1").arg(drm.version);
    if (!drm.algorithm.isEmpty()) details << drm.algorithm;
    if (!drm.kid.isEmpty()) details << QStringLiteral("KID %1").arg(drm.kid);
    details << QStringLiteral("the entire ZIP payload is encrypted as one envelope")
            << QStringLiteral("file names and directories are unavailable without the matching license/content key");
    if (payloadEnd < std::uint64_t(file->capacity()))
        details << QStringLiteral("%1 trailing bytes").arg(std::uint64_t(file->capacity()) - payloadEnd);
    module->info_.description = QStringLiteral("Windows Phone XAP — %1").arg(details.join(QStringLiteral("; ")));
    return ModulePtr(std::move(module));
}

} // namespace peare
