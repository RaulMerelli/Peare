#include "RpmModule.h"

#include "Bzip2Module.h"
#include "CpioModule.h"
#include "GzipModule.h"
#include "LzmaModule.h"
#include "XzModule.h"
#include "ZstdModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <vector>

namespace peare {
namespace {

constexpr std::uint8_t kLeadMagic[] = {0xED, 0xAB, 0xEE, 0xDB};
constexpr std::uint8_t kHeaderMagic[] = {0x8E, 0xAD, 0xE8, 0x01, 0, 0, 0, 0};
constexpr std::uint32_t kTagName = 1000;
constexpr std::uint32_t kTagVersion = 1001;
constexpr std::uint32_t kTagRelease = 1002;
constexpr std::uint32_t kTagArch = 1022;
constexpr std::uint32_t kTagPayloadFormat = 1124;
constexpr std::uint32_t kTagPayloadCompressor = 1125;

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

bool readAt(const fs::ByteStorePtr& source, std::int64_t offset, std::size_t size,
            std::uint8_t* output)
{
    if (!source || offset < 0 || offset > source->capacity() ||
        size > std::size_t(source->capacity() - offset) ||
        size > std::size_t(std::numeric_limits<int>::max())) return false;
    source->readExactly(offset, output, static_cast<int>(size));
    return true;
}

struct HeaderTag {
    std::uint32_t tag;
    std::uint32_t type;
    std::uint32_t offset;
    std::uint32_t count;
};

struct Header {
    std::int64_t end = 0;
    std::map<std::uint32_t, QString> strings;
};

bool parseHeader(const fs::ByteStorePtr& source, std::int64_t start, Header* header)
{
    std::uint8_t intro[16]{};
    if (!readAt(source, start, sizeof(intro), intro) ||
        std::memcmp(intro, kHeaderMagic, sizeof(kHeaderMagic)) != 0) return false;
    const std::uint32_t indexCount = be32(intro + 8);
    const std::uint32_t dataSize = be32(intro + 12);
    if (indexCount > 1000000 || dataSize > 256U * 1024U * 1024U) return false;
    const std::uint64_t indexBytes = std::uint64_t(indexCount) * 16;
    const std::uint64_t total = 16 + indexBytes + dataSize;
    if (total > std::uint64_t(source->capacity() - start)) return false;

    std::vector<std::uint8_t> indexes(static_cast<std::size_t>(indexBytes));
    std::vector<std::uint8_t> data(dataSize);
    if (!indexes.empty() && !readAt(source, start + 16, indexes.size(), indexes.data()))
        return false;
    if (!data.empty() && !readAt(source, start + 16 + std::int64_t(indexBytes),
                                 data.size(), data.data())) return false;

    for (std::uint32_t i = 0; i < indexCount; ++i) {
        const std::uint8_t* raw = indexes.data() + std::size_t(i) * 16;
        HeaderTag entry{be32(raw), be32(raw + 4), be32(raw + 8), be32(raw + 12)};
        if (entry.offset > dataSize) return false;
        std::size_t valueSize = 0;
        if (entry.type == 1 || entry.type == 2) valueSize = entry.count;
        else if (entry.type == 3) valueSize = std::size_t(entry.count) * 2;
        else if (entry.type == 4) valueSize = std::size_t(entry.count) * 4;
        else if (entry.type == 5) valueSize = std::size_t(entry.count) * 8;
        else if (entry.type == 6 || entry.type == 8 || entry.type == 9) {
            if (entry.offset >= data.size()) return false;
            const auto* begin = data.data() + entry.offset;
            const auto* end = data.data() + data.size();
            const auto* zero = std::find(begin, end, std::uint8_t(0));
            if (zero == end) return false;
            const std::size_t length = std::size_t(zero - begin);
            if (entry.tag == kTagName || entry.tag == kTagVersion ||
                entry.tag == kTagRelease || entry.tag == kTagArch ||
                entry.tag == kTagPayloadFormat || entry.tag == kTagPayloadCompressor)
                header->strings[entry.tag] = QString::fromUtf8(
                    reinterpret_cast<const char*>(begin), int(length));
            continue;
        } else if (entry.type == 0 || entry.type == 7) {
            valueSize = entry.count;
        } else {
            return false;
        }
        if (valueSize > data.size() - entry.offset) return false;
    }
    header->end = start + std::int64_t(total);
    return true;
}

class InvalidRpmModule final : public IModule, public IResourceContainer {
public:
    InvalidRpmModule(const QString& name, const QString& error)
    {
        info_.filePath = name;
        info_.format = ModuleFormat::RPM;
        info_.description = QStringLiteral("RPM package");
        info_.error = error;
    }
    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

QString tag(const Header& header, std::uint32_t id)
{
    const auto found = header.strings.find(id);
    return found == header.strings.end() ? QString() : found->second;
}

} // namespace

bool RpmModule::hasSupportedExtension(const QString& name)
{
    return QFileInfo(name).suffix().compare(QStringLiteral("rpm"), Qt::CaseInsensitive) == 0;
}

ModulePtr RpmModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr RpmModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())),
        logicalName);
}

ModulePtr RpmModule::open(const fs::ByteStorePtr& source, const QString& sourceName)
{
    if (!source || source->capacity() < 96)
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Truncated RPM lead")));
    std::uint8_t lead[96]{};
    if (!readAt(source, 0, sizeof(lead), lead) ||
        std::memcmp(lead, kLeadMagic, sizeof(kLeadMagic)) != 0 || lead[4] < 3 || lead[4] > 4)
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Invalid RPM lead")));

    Header signature;
    if (!parseHeader(source, 96, &signature))
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Invalid RPM signature header")));
    const std::int64_t mainStart = (signature.end + 7) & ~std::int64_t(7);
    Header main;
    if (!parseHeader(source, mainStart, &main))
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Invalid RPM metadata header")));
    if (tag(main, kTagPayloadFormat).compare(QStringLiteral("cpio"), Qt::CaseInsensitive) != 0 &&
        !tag(main, kTagPayloadFormat).isEmpty())
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Unsupported RPM payload format")));

    const std::int64_t payloadOffset = main.end;
    const std::int64_t payloadSize = source->capacity() - payloadOffset;
    if (payloadSize <= 0 || std::uint64_t(payloadSize) > std::numeric_limits<std::size_t>::max())
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Missing RPM payload")));
    std::vector<std::uint8_t> payload = source->readRange(payloadOffset, payloadSize);
    if (payload.size() != std::size_t(payloadSize))
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Cannot read RPM payload")));

    QString compressor = tag(main, kTagPayloadCompressor).toLower();
    if (compressor.isEmpty()) {
        if (payload.size() >= 2 && payload[0] == 0x1F && payload[1] == 0x8B) compressor = QStringLiteral("gzip");
        else if (payload.size() >= 4 && std::memcmp(payload.data(), "BZh", 3) == 0) compressor = QStringLiteral("bzip2");
        else if (payload.size() >= 6 && std::memcmp(payload.data(), "\xFD" "7zXZ\0", 6) == 0) compressor = QStringLiteral("xz");
        else if (payload.size() >= 4 && payload[0] == 0x28 && payload[1] == 0xB5 &&
                 payload[2] == 0x2F && payload[3] == 0xFD) compressor = QStringLiteral("zstd");
        else compressor = QStringLiteral("none");
    }

    fs::ByteStorePtr cpio;
    const QString payloadName = sourceName + QStringLiteral("!payload.cpio");
    if (compressor == QStringLiteral("none")) {
        cpio = std::make_shared<fs::MemoryStore>(std::move(payload));
    } else {
        ModulePtr decoded;
        if (compressor == QStringLiteral("gzip")) decoded = GzipModule::open(std::move(payload), payloadName);
        else if (compressor == QStringLiteral("bzip2")) decoded = Bzip2Module::open(std::move(payload), payloadName);
        else if (compressor == QStringLiteral("xz")) decoded = XzModule::open(std::move(payload), payloadName);
        else if (compressor == QStringLiteral("zstd")) decoded = ZstdModule::open(std::move(payload), payloadName);
        else if (compressor == QStringLiteral("lzma"))
            decoded = LzmaModule::open(
                std::make_shared<fs::MemoryStore>(std::move(payload)), payloadName);
        else
            return ModulePtr(new InvalidRpmModule(sourceName,
                QStringLiteral("Unsupported RPM payload compressor: %1").arg(compressor)));
        if (!decoded->info().isValid())
            return ModulePtr(new InvalidRpmModule(sourceName, decoded->info().error));
        const auto* container = dynamic_cast<const IResourceContainer*>(decoded.get());
        if (!container || container->resources().size() != 1 || !container->resources().first().content)
            return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Invalid decompressed RPM payload")));
        cpio = container->resources().first().content;
    }

    ModulePtr archive = CpioModule::open(cpio, sourceName);
    if (!archive->info().isValid())
        return ModulePtr(new InvalidRpmModule(sourceName, archive->info().error));
    const auto* container = dynamic_cast<const IResourceContainer*>(archive.get());
    if (!container)
        return ModulePtr(new InvalidRpmModule(sourceName, QStringLiteral("Invalid RPM CPIO archive")));

    auto module = peare::makeUnique<RpmModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::RPM;
    module->info_.description = QStringLiteral("RPM package: %1-%2-%3 (%4)")
        .arg(tag(main, kTagName), tag(main, kTagVersion), tag(main, kTagRelease), tag(main, kTagArch));
    module->resources_ = container->resources();
    for (ResourceEntry& entry : module->resources_) {
        entry.type = QStringLiteral("RPM_FILE");
        entry.format = ModuleFormat::RPM;
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
