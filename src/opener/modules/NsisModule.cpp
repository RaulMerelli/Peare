#include "NsisModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include "../compression/LzmaDecoder.h"
#include "../compression/peare/Bzip2Decoder.h"
#include "../fs/DiscStore.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <algorithm>
#include <QHash>
#include <limits>
#include <string>
#include <vector>

namespace peare {
namespace {

const QByteArray kSignature("\xEF\xBE\xAD\xDE" "NullsoftInst", 16);
const QByteArray kLegacySignature("\xEF\xBE\xAD\xDE" "nsisinstall\0", 16);
constexpr std::size_t kDecodedLimit = 1024ULL * 1024 * 1024;

bool isNsisSignature(const QByteArray& bytes)
{
    if (bytes.size() != 16) return false;
    const QByteArray magic = bytes.left(4);
    if (magic != QByteArray::fromHex("efbeadde") &&
        magic != QByteArray::fromHex("edbeadde")) return false;
    const QByteArray text = bytes.mid(4);
    return text == kSignature.mid(4) ||
           text == kLegacySignature.mid(4);
}

bool decompressNsisBzip2(const std::uint8_t* data, std::size_t size,
                         std::size_t outputLimit,
                         std::vector<std::uint8_t>* output)
{
    return compression::decompressNsisV1Bzip2(data, size, outputLimit, output) ||
           compression::decompressNsisBzip2(data, size, outputLimit, output);
}

quint32 readLe32(const std::uint8_t* data)
{
    return quint32(data[0]) | (quint32(data[1]) << 8) |
           (quint32(data[2]) << 16) | (quint32(data[3]) << 24);
}

QString safePath(const QString& input)
{
    for (QChar ch : input)
        if (ch.isNull() || ch.category() == QChar::Other_Control) return {};
    QString value = input;
    value.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (value.startsWith(QLatin1Char('/'))) value.remove(0, 1);
    QStringList parts;
    for (const QString& part : value.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) return {};
        if (part.contains(QLatin1Char(':'))) return {};
        parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

QString readString(const std::vector<std::uint8_t>& header, std::size_t tableOffset,
                   std::size_t tableEnd, quint32 index, bool unicode)
{
    const std::size_t unit = unicode ? 2 : 1;
    if (index > (tableEnd - tableOffset) / unit) return {};
    std::size_t pos = tableOffset + std::size_t(index) * unit;
    QString result;
    while (pos + unit <= tableEnd) {
        quint16 ch = header[pos];
        if (unicode) {
            ch |= quint16(header[pos + 1]) << 8;
        } else {
            ch = header[pos];
        }
        pos += unit;
        if (!ch) break;
        quint16 token = 0;
        if (unicode && ch >= 1 && ch <= 4)
            token = ch;
        else if (!unicode && ch >= 252 && ch <= 255)
            token = 256 - ch;
        if (token != 0) {
            if (token == 4) {
                if (pos + unit > tableEnd) return {};
                quint16 literal = header[pos++];
                if (unicode) literal |= quint16(header[pos++]) << 8;
                if (!literal) return {};
                result.append(QChar(literal));
                continue;
            }
            constexpr std::size_t encodedSize = 2;
            if (pos + encodedSize > tableEnd) return {};
            quint16 encoded = header[pos++];
            encoded |= quint16(header[pos++]) << 8;
            if ((encoded & 0x8080) != 0x8080) return {};
            const quint16 value = (encoded & 0x7f) | ((encoded & 0x7f00) >> 1);
            if (token == 3) {
                static const char* variables[] = {
                    "$0", "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9",
                    "$R0", "$R1", "$R2", "$R3", "$R4", "$R5", "$R6", "$R7", "$R8", "$R9",
                    "$CMDLINE", "$INSTDIR", "$OUTDIR", "$EXEDIR", "$LANGUAGE", "$TEMP",
                    "$PLUGINSDIR", "$EXEPATH", "$EXEFILE", "$HWNDPARENT", "$_CLICK", "$_OUTDIR"
                };
                if (value < sizeof(variables) / sizeof(variables[0]))
                    result.append(QString::fromLatin1(variables[value]));
                else
                    result.append(QStringLiteral("$VAR%1").arg(value));
            } else {
                result.append(QStringLiteral("$NSIS_TOKEN%1").arg(token));
            }
            continue;
        }
        result.append(QChar(ch));
    }
    return result;
}

QString readV1String(const std::vector<std::uint8_t>& header, std::size_t tableOffset,
                     quint32 index)
{
    if (tableOffset > header.size() || index >= header.size() - tableOffset) return {};
    static const char* variables[] = {
        "$0", "$1", "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9",
        "$R0", "$R1", "$R2", "$R3", "$R4", "$R5", "$R6", "$R7", "$R8", "$R9",
        "$CMDLINE", "$EXEDIR", "$INSTDIR", "$OUTDIR", "$LANGUAGE", "$TEMP",
        "$PLUGINSDIR", "$EXEPATH", "$EXEFILE", "$HWNDPARENT", "$_CLICK", "$_OUTDIR"
    };
    const std::size_t end = header.size();
    std::size_t pos = tableOffset + index;
    QString result;
    while (pos < end) {
        const quint8 ch = header[pos++];
        if (ch == 0) break;
        if (ch == 0xff) {
            if (pos >= end || header[pos] == 0) return {};
            result.append(QChar(header[pos++]));
        } else if (ch >= 0xdd) {
            const std::size_t variable = ch - 0xdd;
            if (variable < sizeof(variables) / sizeof(variables[0]))
                result.append(QString::fromLatin1(variables[variable]));
            else
                result.append(QStringLiteral("$VAR%1").arg(variable));
        } else {
            result.append(QChar(ch));
        }
    }
    return result;
}

QString resolveOutputPath(QString path, const QString& currentPath,
                          const QString& savedOutputPath,
                          const QHash<QString, QString>& variablePaths = {})
{
    const QStringList variables = {
        QStringLiteral("$_OUTDIR"), QStringLiteral("$INSTDIR"),
        QStringLiteral("$EXEDIR"), QStringLiteral("$PLUGINSDIR"),
        QStringLiteral("$OUTDIR"), QStringLiteral("$HWNDPARENT"),
        QStringLiteral("$CMDLINE"), QStringLiteral("$LANGUAGE"),
        QStringLiteral("$TEMP"), QStringLiteral("$EXEPATH"),
        QStringLiteral("$EXEFILE"), QStringLiteral("$_CLICK"),
        QStringLiteral("$0"), QStringLiteral("$1"), QStringLiteral("$2"),
        QStringLiteral("$3"), QStringLiteral("$4"), QStringLiteral("$5"),
        QStringLiteral("$6"), QStringLiteral("$7"), QStringLiteral("$8"),
        QStringLiteral("$9"), QStringLiteral("$R0"), QStringLiteral("$R1"),
        QStringLiteral("$R2"), QStringLiteral("$R3"), QStringLiteral("$R4"),
        QStringLiteral("$R5"), QStringLiteral("$R6"), QStringLiteral("$R7"),
        QStringLiteral("$R8"), QStringLiteral("$R9")
    };
    const QStringList relativeVariables = {
        QStringLiteral("$HWNDPARENT"), QStringLiteral("$CMDLINE"),
        QStringLiteral("$LANGUAGE"), QStringLiteral("$TEMP"),
        QStringLiteral("$EXEPATH"), QStringLiteral("$EXEFILE"),
        QStringLiteral("$_CLICK")
    };
    for (const QString& variable : variables) {
        const qsizetype offset = path.indexOf(variable, 0, Qt::CaseInsensitive);
        if (offset < 0) continue;
        const bool numberedVariable = (variable.size() == 2 && variable[1].isDigit()) ||
            (variable.size() == 3 && variable.startsWith(QStringLiteral("$R")) &&
             variable[2].isDigit());
        const QString prefix = variablePaths.contains(variable) ? variablePaths.value(variable) :
            numberedVariable ? variable :
            variable == QStringLiteral("$_OUTDIR") ? savedOutputPath :
            variable == QStringLiteral("$OUTDIR") ? currentPath :
            variable == QStringLiteral("$PLUGINSDIR") ? QStringLiteral("$PLUGINSDIR") :
            relativeVariables.contains(variable, Qt::CaseInsensitive) ? currentPath : QString();
        path = prefix + path.mid(offset + variable.size());
        break;
    }
    return safePath(path);
}

bool inflateDeflate(const std::uint8_t* data, std::size_t size,
                    std::vector<std::uint8_t>* output)
{
    compression::MemoryInflateInput input(data, size);
    std::string error;
    return compression::inflateRawStream(input, kDecodedLimit, output, &error);
}

bool decodeLzma(const std::uint8_t* data, std::size_t size,
                const std::uint8_t* properties, std::size_t propertySize,
                std::size_t expected, bool endMarker,
                std::vector<std::uint8_t>* output)
{
    if (endMarker)
        return compression::decodeLzmaRawUntilEndMarkerWithTrailing(
            data, size, properties, propertySize, output);
    return compression::decodeLzmaRaw(data, size, properties, propertySize,
                                       expected, output);
}

enum class Method { Deflate, Bzip2, Lzma };

struct FileItem {
    QString path;
    quint32 position;
};

bool parseItems(const std::vector<std::uint8_t>& header, QVector<FileItem>* items)
{
    if (header.size() < 68) return false;
    const quint32 entriesOffset = readLe32(header.data() + 20);
    const quint32 entriesCount = readLe32(header.data() + 24);
    const quint32 stringsOffset = readLe32(header.data() + 28);
    const quint32 languagesOffset = readLe32(header.data() + 36);
    if (entriesOffset > header.size() || stringsOffset > languagesOffset ||
        languagesOffset > header.size() || quint64(entriesCount) * 28 > header.size() - entriesOffset)
        return false;
    const bool unicode = languagesOffset - stringsOffset >= 2 &&
        header[stringsOffset] == 0 && header[stringsOffset + 1] == 0;
    const std::size_t stringUnit = unicode ? 2 : 1;
    if ((languagesOffset - stringsOffset) % stringUnit != 0) return false;

    QString outputPath;
    QString savedOutputPath;
    QHash<QString, QString> variablePaths;
    QSet<QString> seenItems;
    bool legacyOutdir = false;
    for (quint32 i = 0; i < entriesCount; ++i) {
        const std::uint8_t* command = header.data() + entriesOffset + std::size_t(i) * 28;
        if (readLe32(command) == 25 && readLe32(command + 4) == 29 &&
            readLe32(command + 12) == 0 && readLe32(command + 16) == 0 &&
            readString(header, stringsOffset, languagesOffset,
                       readLe32(command + 8), unicode) == QStringLiteral("$OUTDIR")) {
            legacyOutdir = true;
            break;
        }
    }
    for (quint32 i = 0; i < entriesCount; ++i) {
        const std::uint8_t* command = header.data() + entriesOffset + std::size_t(i) * 28;
        const quint32 id = readLe32(command);
        const quint32 p0 = readLe32(command + 4);
        const quint32 p1 = readLe32(command + 8);
        if (id == 11 && p1 != 0) {
            QString path = readString(header, stringsOffset, languagesOffset, p0, unicode);
            if (legacyOutdir) path.replace(QStringLiteral("$_CLICK"), QStringLiteral("$_OUTDIR"));
            outputPath = resolveOutputPath(path, outputPath, savedOutputPath, variablePaths);
        } else if (id == 25 && (p0 == 29 || p0 == 31) && readLe32(command + 12) == 0 &&
                   readLe32(command + 16) == 0 &&
                   readString(header, stringsOffset, languagesOffset, p1, unicode) ==
                       QStringLiteral("$OUTDIR")) {
            savedOutputPath = outputPath;
        } else if (id == 25 && p0 < 20 && readLe32(command + 12) == 0 &&
                   readLe32(command + 16) == 0 && p1 != 0) {
            const QString variable = p0 < 10 ?
                QStringLiteral("$") + QString::number(p0) :
                QStringLiteral("$R") + QString::number(p0 - 10);
            QString value = readString(header, stringsOffset, languagesOffset, p1, unicode);
            if (legacyOutdir) value.replace(QStringLiteral("$_CLICK"), QStringLiteral("$_OUTDIR"));
            if (value == QStringLiteral("$OUTDIR"))
                variablePaths.insert(variable, outputPath);
            else
                variablePaths.insert(variable,
                    resolveOutputPath(value, outputPath, savedOutputPath, variablePaths));
        } else if (id == 20) {
            const QString name = readString(header, stringsOffset, languagesOffset, p1, unicode);
            const QString relativePath = outputPath.isEmpty() ? name :
                outputPath + QLatin1Char('/') + name;
            const QString path = resolveOutputPath(relativePath, outputPath, savedOutputPath,
                                                   variablePaths);
            const quint32 position = readLe32(command + 12);
            const QString identity = QString::number(position) + QLatin1Char(':') + path;
            if (!path.isEmpty() && !seenItems.contains(identity)) {
                seenItems.insert(identity);
                items->push_back({path, position});
            }
        }
    }
    return true;
}

bool parseV1Items(const std::vector<std::uint8_t>& header, quint32 flags,
                  QVector<FileItem>* items)
{
    const bool uninstaller = (flags & 0x02U) != 0;
    const std::size_t fixedSize = uninstaller ? 120 : 240;
    if (header.size() < fixedSize) return false;
    const quint32 entryCount = readLe32(header.data() + 52);
    const quint32 sectionCount = uninstaller ? 0 : readLe32(header.data() + 224);
    if (entryCount > 1000000 || sectionCount > 100000) return false;
    const std::size_t sectionsOffset = fixedSize;
    const std::size_t entriesOffset = sectionsOffset + std::size_t(sectionCount) * 20;
    if (entriesOffset > header.size() ||
        quint64(entryCount) * 24 > header.size() - entriesOffset) return false;
    const std::size_t stringsOffset = entriesOffset + std::size_t(entryCount) * 24;

    QString outputPath;
    QString savedOutputPath;
    QSet<QString> seenItems;
    for (quint32 i = 0; i < entryCount; ++i) {
        const std::uint8_t* command = header.data() + entriesOffset + std::size_t(i) * 24;
        const quint32 id = readLe32(command);
        const quint32 p0 = readLe32(command + 4);
        const quint32 p1 = readLe32(command + 8);
        if (id == 14 && p1 != 0) {
            const QString path = readV1String(header, stringsOffset, p0);
            outputPath = resolveOutputPath(path, outputPath, savedOutputPath);
        } else if (id == 21) {
            const QString name = readV1String(header, stringsOffset, p1);
            const QString combined = outputPath.isEmpty() ? name :
                outputPath + QLatin1Char('/') + name;
            const QString path = resolveOutputPath(combined, outputPath, savedOutputPath);
            const quint32 position = readLe32(command + 12);
            const QString identity = QString::number(position) + QLatin1Char(':') + path;
            if (!path.isEmpty() && !seenItems.contains(identity)) {
                seenItems.insert(identity);
                items->push_back({path, position});
            }
        }
    }
    return true;
}

bool decodeArchive(const QByteArray& input, quint32 archiveOffset,
                   QVector<ResourceEntry>* resources, QString* error)
{
    const auto* raw = reinterpret_cast<const std::uint8_t*>(input.constData());
    const std::size_t inputSize = std::size_t(input.size());
    if (archiveOffset > inputSize || inputSize - archiveOffset < 28 ||
        !isNsisSignature(QByteArray(reinterpret_cast<const char*>(raw + archiveOffset + 4), 16))) {
        *error = QStringLiteral("Invalid NSIS signature");
        return false;
    }
    const quint32 flags = readLe32(raw + archiveOffset);
    const quint32 headerSize = readLe32(raw + archiveOffset + 20);
    const quint32 archiveSize = readLe32(raw + archiveOffset + 24);
    if ((flags & ~0x0fU) != 0 || headerSize < 68 || archiveSize < 28 ||
        archiveSize > inputSize - archiveOffset) {
        *error = QStringLiteral("Invalid NSIS archive header");
        return false;
    }
    const std::size_t dataOffset = std::size_t(archiveOffset) + 28;
    const std::size_t dataSize = archiveSize - 28;
    if (dataSize < 4) {
        *error = QStringLiteral("Truncated NSIS data stream");
        return false;
    }

    const quint32 firstSize = readLe32(raw + dataOffset);
    const bool solid = firstSize != headerSize &&
        (raw[dataOffset] == 0x5d || raw[dataOffset] == 0x31 ||
         (firstSize & 0x80000000U) == 0 ||
         (firstSize & 0x7fffffffU) > dataSize);
    const bool headerCompressed = !solid && (firstSize & 0x80000000U) != 0;
    Method method = Method::Deflate;
    std::vector<std::uint8_t> decoded;
    std::vector<std::uint8_t> header;
    std::size_t nonSolidOffset = 0;

    if (!solid && !headerCompressed) {
        if (firstSize != headerSize || dataSize < 4 + headerSize) {
            *error = QStringLiteral("Invalid uncompressed NSIS header block");
            return false;
        }
        header.assign(raw + dataOffset + 4, raw + dataOffset + 4 + headerSize);
        nonSolidOffset = headerSize;
    } else if (solid) {
        const std::uint8_t* stream = raw + dataOffset;
        const std::size_t streamSize = dataSize;
        if (streamSize >= 7 && stream[0] == 0x5d) {
            method = Method::Lzma;
            if (!decodeLzma(stream + 5, streamSize - 5, stream, 5, 0, true, &decoded)) {
                *error = QStringLiteral("Invalid solid NSIS LZMA stream");
                return false;
            }
        } else if (streamSize >= 2 && stream[0] == 0x31 && stream[1] < 14) {
            method = Method::Bzip2;
            if (!decompressNsisBzip2(stream, streamSize, kDecodedLimit, &decoded)) {
                *error = QStringLiteral("Invalid solid NSIS BZIP2 stream");
                return false;
            }
        } else if (!inflateDeflate(stream, streamSize, &decoded)) {
            *error = QStringLiteral("Invalid solid NSIS Deflate stream");
            return false;
        }
        if (decoded.size() < 4 || readLe32(decoded.data()) != headerSize ||
            decoded.size() < 4 + headerSize) {
            *error = QStringLiteral("NSIS solid header size mismatch");
            return false;
        }
        header.assign(decoded.begin() + 4, decoded.begin() + 4 + headerSize);
    } else {
        nonSolidOffset = firstSize & 0x7fffffffU;
        const std::size_t blockSize = nonSolidOffset;
        if (blockSize > dataSize - 4 || blockSize == 0) {
            *error = QStringLiteral("Invalid NSIS compressed header size");
            return false;
        }
        const std::uint8_t* block = raw + dataOffset + 4;
        if (block[0] == 0x5d || (blockSize >= 6 && block[1] == 0x5d)) {
            method = Method::Lzma;
            const std::size_t propertyOffset = block[0] == 0x5d ? 0 : 1;
            if (blockSize < propertyOffset + 5 ||
                !decodeLzma(block + propertyOffset + 5,
                            blockSize - propertyOffset - 5,
                            block + propertyOffset, 5, headerSize, false, &header)) {
                *error = QStringLiteral("Invalid non-solid NSIS LZMA header");
                return false;
            }
        } else if (block[0] == 0x31 && blockSize >= 2) {
            method = Method::Bzip2;
            if (!decompressNsisBzip2(block, blockSize, headerSize, &header) ||
                header.size() != headerSize) {
                *error = QStringLiteral("Invalid non-solid NSIS BZIP2 header");
                return false;
            }
        } else if (!inflateDeflate(block, blockSize, &header) || header.size() != headerSize) {
            *error = QStringLiteral("Invalid non-solid NSIS Deflate header");
            return false;
        }
    }

    QVector<FileItem> items;
    bool parsed = parseItems(header, &items);
    if (!parsed || items.isEmpty()) {
        items.clear();
        parsed = parseV1Items(header, flags, &items);
    }
    if (!parsed) {
        *error = QStringLiteral("Invalid NSIS command or string table");
        return false;
    }
    for (int i = 0; i < items.size(); ++i) {
        const FileItem& item = items[i];
        std::vector<std::uint8_t> payload;
        if (solid) {
            const std::size_t block = 4 + std::size_t(headerSize) + item.position;
            if (block > decoded.size() || decoded.size() - block < 4) continue;
            const quint32 framedSize = readLe32(decoded.data() + block);
            const bool compressed = (framedSize & 0x80000000U) != 0;
            const std::size_t length = framedSize & 0x7fffffffU;
            const std::size_t body = block + 4;
            if (length > decoded.size() - body) continue;
            if (!compressed) {
                payload.assign(decoded.begin() + body, decoded.begin() + body + length);
            } else if (method == Method::Deflate) {
                if (!inflateDeflate(decoded.data() + body, length, &payload)) continue;
            } else if (method == Method::Bzip2) {
                if (!decompressNsisBzip2(decoded.data() + body, length,
                                          kDecodedLimit, &payload)) continue;
            } else if (method == Method::Lzma) {
                if (length < 5 || !decodeLzma(decoded.data() + body + 5, length - 5,
                                               decoded.data() + body, 5, 0, true, &payload)) continue;
            }
        } else {
            const std::size_t itemOffset = dataOffset + 4 + nonSolidOffset + item.position;
            if (itemOffset > std::size_t(archiveOffset) + archiveSize ||
                std::size_t(archiveOffset) + archiveSize - itemOffset < 4) continue;
            const quint32 packedSize = readLe32(raw + itemOffset);
            const bool compressed = (packedSize & 0x80000000U) != 0;
            const std::size_t length = packedSize & 0x7fffffffU;
            const std::size_t body = itemOffset + 4;
            if (length > std::size_t(archiveOffset) + archiveSize - body) continue;
            if (!compressed) {
                payload.assign(raw + body, raw + body + length);
            } else if (method == Method::Deflate) {
                if (!inflateDeflate(raw + body, length, &payload)) continue;
            } else if (method == Method::Lzma) {
                if (length < 5 || !decodeLzma(raw + body + 5, length - 5,
                                               raw + body, 5, 0, true, &payload)) continue;
            } else if (method == Method::Bzip2) {
                if (!decompressNsisBzip2(raw + body, length, kDecodedLimit, &payload)) continue;
            }
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("NSIS_FILE");
        entry.name = QFileInfo(item.path).fileName();
        entry.language = QStringLiteral("neutral");
        entry.dataSize = payload.size();
        entry.format = ModuleFormat::NSIS;
        entry.isEmbeddedFile = true;
        entry.hierarchyPath = item.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        entry.content = std::make_shared<fs::MemoryStore>(std::move(payload));
        resources->push_back(std::move(entry));
    }
    if (resources->isEmpty()) {
        *error = QStringLiteral("NSIS installer contains no readable files");
        return false;
    }
    return true;
}

}

std::unique_ptr<NsisModule> NsisModule::open(const QString& filePath, quint32 headerOffset)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<NsisModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::NSIS;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath, headerOffset);
}

std::unique_ptr<NsisModule> NsisModule::open(const QByteArray& data,
                                              const QString& logicalName)
{
    quint32 offset = 0;
    for (quint32 i = 0; i + 28 <= quint32(data.size()) && i <= 64 * 1024 * 1024; i += 512) {
        if (isNsisSignature(data.mid(i + 4, 16))) {
            offset = i;
            break;
        }
    }
    return open(data, logicalName, offset);
}

std::unique_ptr<NsisModule> NsisModule::open(const QByteArray& data,
                                              const QString& logicalName,
                                              quint32 headerOffset)
{
    auto module = peare::makeUnique<NsisModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::NSIS;
    module->info_.description = QStringLiteral("Nullsoft Scriptable Install System installer");
    if (!decodeArchive(data, headerOffset, &module->resources_, &module->info_.error)) {
        module->resources_.clear();
    }
    return module;
}

}
