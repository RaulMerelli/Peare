#include "GzipModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include "../compression/Crc32.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>
#include <algorithm>
#include <limits>
#include <vector>

namespace peare {
namespace {

using compression::crc32;

quint16 le16(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    const auto* p = data.data() + offset;
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::vector<std::uint8_t>& data, std::size_t offset)
{
    const auto* p = data.data() + offset;
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString fallbackName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".gzip"), Qt::CaseInsensitive))
        name.chop(5);
    else if (name.endsWith(QStringLiteral(".gz"), Qt::CaseInsensitive))
        name.chop(3);
    return name.isEmpty() ? QStringLiteral("gzip_payload") : name;
}

bool readZeroTerminated(const std::vector<std::uint8_t>& data, std::size_t* position,
                        std::vector<std::uint8_t>* value)
{
    const std::size_t start = *position;
    while (*position < data.size() && data[*position] != 0) ++*position;
    if (*position >= data.size()) return false;
    value->assign(data.begin() + std::ptrdiff_t(start),
                  data.begin() + std::ptrdiff_t(*position));
    ++*position;
    return true;
}

} // namespace

std::unique_ptr<GzipModule> GzipModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<GzipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::GZIP;
        module->info_.error = QStringLiteral("Could not read GZIP input");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<GzipModule> GzipModule::open(const fs::ByteStorePtr& source,
                                             const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<GzipModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::GZIP;
        module->info_.error = QStringLiteral("Invalid GZIP byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<GzipModule> GzipModule::open(const QByteArray& data,
                                             const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

std::unique_ptr<GzipModule> GzipModule::open(std::vector<std::uint8_t> data,
                                             const QString& logicalName)
{
    auto module = peare::makeUnique<GzipModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::GZIP;
    module->info_.description = QStringLiteral("GZIP compressed stream");
    if (data.size() < 18 || data[0] != 0x1F || data[1] != 0x8B) {
        module->info_.error = QStringLiteral("Invalid or truncated GZIP header");
        return module;
    }

    std::vector<std::uint8_t> decoded;
    std::size_t position = 0;
    QString memberName;
    std::size_t memberCount = 0;
    while (position < data.size()) {
        if (data[position] == 0) {
            while (position < data.size() && data[position] == 0) ++position;
            if (position == data.size()) break;
        }
        const std::size_t headerStart = position;
        if (data.size() - position < 10 || data[position] != 0x1F ||
            data[position + 1] != 0x8B || data[position + 2] != 8) {
            module->info_.error = QStringLiteral("Invalid GZIP member header");
            return module;
        }
        const std::uint8_t flags = data[position + 3];
        if ((flags & 0xE0u) != 0) {
            module->info_.error = QStringLiteral("Reserved GZIP flags are set");
            return module;
        }
        position += 10;
        if ((flags & 0x04u) != 0) {
            if (data.size() - position < 2) {
                module->info_.error = QStringLiteral("Truncated GZIP extra-field length");
                return module;
            }
            const quint16 length = le16(data, position);
            position += 2;
            if (data.size() - position < length) {
                module->info_.error = QStringLiteral("Truncated GZIP extra field");
                return module;
            }
            position += length;
        }
        std::vector<std::uint8_t> originalName;
        if ((flags & 0x08u) != 0 && !readZeroTerminated(data, &position, &originalName)) {
            module->info_.error = QStringLiteral("Truncated GZIP original filename");
            return module;
        }
        if ((flags & 0x10u) != 0) {
            std::vector<std::uint8_t> comment;
            if (!readZeroTerminated(data, &position, &comment)) {
                module->info_.error = QStringLiteral("Truncated GZIP comment");
                return module;
            }
        }
        if ((flags & 0x02u) != 0) {
            if (data.size() - position < 2 ||
                le16(data, position) != (crc32(data.data() + headerStart,
                    position - headerStart) & 0xFFFFu)) {
                module->info_.error = QStringLiteral("GZIP header CRC mismatch");
                return module;
            }
            position += 2;
        }
        if (!originalName.empty() && memberName.isEmpty()) {
            if (originalName.size() > std::size_t(std::numeric_limits<int>::max())) {
                module->info_.error = QStringLiteral("GZIP original filename is too long");
                return module;
            }
            memberName = QFileInfo(QString::fromLatin1(
                reinterpret_cast<const char*>(originalName.data()),
                int(originalName.size()))).fileName();
        }

        peare::compression::MemoryInflateInput input(
            data.data() + position, data.size() - position);
        std::vector<std::uint8_t> member;
        std::string error;
        const std::size_t remaining = decoded.max_size() - decoded.size();
        if (!peare::compression::inflateRawStream(input, remaining, &member, &error)) {
            module->info_.error = QString::fromStdString(error);
            return module;
        }
        position += input.consumed();
        if (data.size() - position < 8) {
            module->info_.error = QStringLiteral("Truncated GZIP trailer");
            return module;
        }
        if (le32(data, position) != crc32(member.data(), member.size())) {
            module->info_.error = QStringLiteral("GZIP data CRC mismatch");
            return module;
        }
        if (le32(data, position + 4) != quint32(member.size() & 0xFFFFFFFFu)) {
            module->info_.error = QStringLiteral("GZIP uncompressed size mismatch");
            return module;
        }
        position += 8;
        decoded.insert(decoded.end(), member.begin(), member.end());
        ++memberCount;
    }
    if (memberCount == 0) {
        module->info_.error = QStringLiteral("GZIP stream has no members");
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("GZIP_FILE");
    entry.name = memberName.isEmpty() ? fallbackName(logicalName) : memberName;
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::GZIP;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
