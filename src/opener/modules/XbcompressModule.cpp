#include "XbcompressModule.h"
#include "Compat.h"
#include "../compression/LzmsDecoder.h"
#include "DeflateDecoder.h"
#include "../fs/DiscStore.h"

#include <QFile>
#include <QFileInfo>
#include <cstring>
#include <limits>
#include <vector>

namespace peare {
namespace {

const QByteArray kSignature("AFCWGPSM", 8);
quint16 readLe16(const QByteArray& data, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 readLe32(const QByteArray& data, qsizetype offset)
{
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 readLe64(const QByteArray& data, qsizetype offset)
{
    return quint64(readLe32(data, offset)) |
           (quint64(readLe32(data, offset + 4)) << 32);
}

QString recoverName(const QString& logicalName, quint16 replacedCharacter)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.isEmpty()) name = QStringLiteral("payload._");
    if (name.endsWith(QStringLiteral("._"))) {
        name.chop(2);
    } else if (name.endsWith(QLatin1Char('_'))) {
        if (replacedCharacter != 0)
            name[name.size() - 1] = QChar(replacedCharacter);
    }
    return name;
}

bool decode(const QByteArray& input, std::vector<std::uint8_t>* output, QString* error)
{
    if (input.size() < 16 || input.left(8) != kSignature) {
        *error = QStringLiteral("Invalid XBCOMPRESS header");
        return false;
    }
    const uchar algorithm = uchar(input.at(8));
    if (uchar(input.at(9)) != uchar('A')) {
        *error = QStringLiteral("Unsupported XBCOMPRESS version");
        return false;
    }
    const quint32 decodedSize = readLe32(input, 12);
    if (decodedSize > output->max_size()) {
        *error = QStringLiteral("XBCOMPRESS output exceeds the available address space");
        return false;
    }

    if (algorithm != 5 && algorithm != 2) {
        *error = QStringLiteral("Unsupported XBCOMPRESS compression algorithm");
        return false;
    }
    if (input.size() < 44 ||
        std::memcmp(input.constData() + 16, "\x0a\x51\xe5\xc0\x18\x00", 6) != 0 ||
        uchar(input.at(23)) != algorithm || readLe64(input, 24) != decodedSize) {
        *error = QStringLiteral("Invalid XBCOMPRESS compression stream header");
        return false;
    }

    const quint64 blockSize = readLe64(input, 32);
    if ((decodedSize != 0 && blockSize == 0) || blockSize > std::numeric_limits<quint32>::max()) {
        *error = QStringLiteral("Invalid XBCOMPRESS block size");
        return false;
    }

    std::vector<std::uint8_t> decoded;
    decoded.reserve(decodedSize);
    qsizetype cursor = 40;
    while (decoded.size() < decodedSize) {
        if (input.size() - cursor < 4) {
            *error = QStringLiteral("Truncated XBCOMPRESS block header");
            return false;
        }
        const quint32 compressedSize = readLe32(input, cursor);
        cursor += 4;
        if (compressedSize == 0 || compressedSize > quint32(input.size() - cursor)) {
            *error = QStringLiteral("Invalid XBCOMPRESS compressed block size");
            return false;
        }
        const std::size_t expectedSize = std::size_t(qMin<quint64>(
            blockSize, decodedSize - decoded.size()));
        std::vector<std::uint8_t> block;
        std::string decodeError;
        bool ok = false;
        if (algorithm == 5) {
            ok = compression::decompressLzmsBlock(
                reinterpret_cast<const std::uint8_t*>(input.constData() + cursor),
                compressedSize, expectedSize, &block, &decodeError);
        } else if (compressedSize < 3 || input.at(cursor) != 'C' ||
                   input.at(cursor + 1) != 'K') {
            *error = QStringLiteral("Invalid XBCOMPRESS MSZIP block signature");
            return false;
        } else {
            std::vector<std::uint8_t> dictionary;
            const std::size_t historySize = qMin<std::size_t>(32768, decoded.size());
            if (historySize) {
                dictionary.assign(decoded.end() - historySize, decoded.end());
            }
            ok = compression::inflateRawExact(
                reinterpret_cast<const std::uint8_t*>(input.constData() + cursor + 2),
                compressedSize - 2, expectedSize,
                dictionary.empty() ? nullptr : &dictionary, &block, &decodeError);
        }
        if (!ok || block.size() != expectedSize) {
            *error = decodeError.empty() ? QStringLiteral("Invalid XBCOMPRESS block data")
                                         : QString::fromStdString(decodeError);
            return false;
        }
        decoded.insert(decoded.end(), block.begin(), block.end());
        cursor += compressedSize;
    }
    if (cursor != input.size()) {
        *error = QStringLiteral("Trailing data in XBCOMPRESS stream");
        return false;
    }
    *output = std::move(decoded);
    return true;
}

}

std::unique_ptr<XbcompressModule> XbcompressModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<XbcompressModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::XBCOMPRESS;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<XbcompressModule> XbcompressModule::open(
    const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<XbcompressModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::XBCOMPRESS;
    module->info_.description = QStringLiteral("Microsoft XBCOMPRESS file");

    std::vector<std::uint8_t> decoded;
    QString error;
    if (!decode(data, &decoded, &error)) {
        module->info_.error = error;
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("XBCOMPRESS_FILE");
    entry.name = recoverName(logicalName, readLe16(data, 10));
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = 16;
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::XBCOMPRESS;
    entry.isEmbeddedFile = true;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

}
