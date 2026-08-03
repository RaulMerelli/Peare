#include "ZipModule.h"
#include "Compat.h"

#include <QFile>
#include <QStringList>
#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace peare {
namespace {

const quint32 kZipLocalHeader = 0x04034b50u;
const quint32 kZipCentralHeader = 0x02014b50u;
const quint32 kZipEndCentralDirectory = 0x06054b50u;
const qsizetype kMaxZipUncompressed = qsizetype(512) * 1024 * 1024;

quint16 le16(const QByteArray& data, qsizetype offset) {
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& data, qsizetype offset) {
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString decodeZipName(const QByteArray& bytes, quint16 flags) {
    return (flags & 0x0800) ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

QString normalizeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    return name;
}

QString safeLeaf(QString name, int index) {
    name = normalizeName(name);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString leaf = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i) {
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    }
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("zip_file_%1.bin").arg(index, 3, 10, QLatin1Char('0'))
                          : leaf;
}

QStringList hierarchyFor(QString name) {
    name = normalizeName(name);
    QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

bool inflateRaw(const QByteArray& src, quint32 expected, QByteArray* out) {
    if (expected > quint32(kMaxZipUncompressed)) return false;
    out->resize(int(expected));
    z_stream stream;
    std::memset(&stream, 0, sizeof(stream));
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(src.constData()));
    stream.avail_in = uInt(src.size());
    stream.next_out = reinterpret_cast<Bytef*>(out->data());
    stream.avail_out = uInt(out->size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) return false;
    const int status = inflate(&stream, Z_FINISH);
    const uLong produced = stream.total_out;
    inflateEnd(&stream);
    return status == Z_STREAM_END && produced == expected;
}

qsizetype findEocd(const QByteArray& data) {
    const qsizetype minPos = std::max<qsizetype>(0, data.size() - (65535 + 22));
    for (qsizetype pos = data.size() - 22; pos >= minPos; --pos) {
        if (le32(data, pos) == kZipEndCentralDirectory) {
            const quint16 comment = le16(data, pos + 20);
            if (pos + 22 + comment == data.size())
                return pos;
        }
        if (pos == 0) break;
    }
    return -1;
}

}  // namespace

std::unique_ptr<ZipModule> ZipModule::open(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<ZipModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::ZIP;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<ZipModule> ZipModule::open(const QByteArray& data, const QString& logicalName) {
    auto module = peare::makeUnique<ZipModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::ZIP;
    module->info_.description = QStringLiteral("ZIP archive");

    const qsizetype eocd = findEocd(data);
    if (eocd < 0) {
        module->info_.error = QStringLiteral("ZIP end of central directory not found");
        return module;
    }
    const quint16 entries = le16(data, eocd + 10);
    const quint32 cdSize = le32(data, eocd + 12);
    const quint32 cdOffset = le32(data, eocd + 16);
    if (cdOffset == 0xffffffffu || cdSize == 0xffffffffu ||
        qsizetype(cdOffset) + qsizetype(cdSize) > data.size()) {
        module->info_.error = QStringLiteral("ZIP64 archives are not supported yet");
        return module;
    }

    qsizetype pos = qsizetype(cdOffset);
    for (quint16 i = 0; i < entries; ++i) {
        if (pos + 46 > data.size() || le32(data, pos) != kZipCentralHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP central directory");
            return module;
        }
        const quint16 flags = le16(data, pos + 8);
        const quint16 method = le16(data, pos + 10);
        const quint32 compressedSize = le32(data, pos + 20);
        const quint32 uncompressedSize = le32(data, pos + 24);
        const quint16 nameLen = le16(data, pos + 28);
        const quint16 extraLen = le16(data, pos + 30);
        const quint16 commentLen = le16(data, pos + 32);
        const quint32 localOffset = le32(data, pos + 42);
        if (pos + 46 + nameLen + extraLen + commentLen > data.size()) {
            module->info_.error = QStringLiteral("Truncated ZIP central directory");
            return module;
        }
        const QString fullName = normalizeName(decodeZipName(data.mid(pos + 46, nameLen), flags));
        pos += 46 + nameLen + extraLen + commentLen;
        if (fullName.isEmpty() || fullName.endsWith(QLatin1Char('/')))
            continue;
        if (flags & 0x0001) {
            module->info_.error = QStringLiteral("Encrypted ZIP entries are not supported");
            return module;
        }
        if (localOffset == 0xffffffffu || compressedSize == 0xffffffffu ||
            uncompressedSize == 0xffffffffu) {
            module->info_.error = QStringLiteral("ZIP64 entries are not supported yet");
            return module;
        }
        const qsizetype lo = qsizetype(localOffset);
        if (lo + 30 > data.size() || le32(data, lo) != kZipLocalHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP local header");
            return module;
        }
        const quint16 localNameLen = le16(data, lo + 26);
        const quint16 localExtraLen = le16(data, lo + 28);
        const qsizetype dataOffset = lo + 30 + localNameLen + localExtraLen;
        if (dataOffset + qsizetype(compressedSize) > data.size()) {
            module->info_.error = QStringLiteral("Truncated ZIP file data");
            return module;
        }

        QByteArray payload;
        if (method == 0) {
            if (compressedSize != uncompressedSize) {
                module->info_.error = QStringLiteral("Invalid stored ZIP entry size");
                return module;
            }
            payload = data.mid(dataOffset, qsizetype(compressedSize));
        } else if (method == 8) {
            if (!inflateRaw(data.mid(dataOffset, qsizetype(compressedSize)), uncompressedSize, &payload)) {
                module->info_.error = QStringLiteral("ZIP deflate decompression failed");
                return module;
            }
        } else {
            module->info_.error = QStringLiteral("Unsupported ZIP compression method %1").arg(method);
            return module;
        }

        ResourceEntry entry;
        entry.type = QStringLiteral("ZIP_FILE");
        entry.isEmbeddedFile = true;
        entry.name = safeLeaf(fullName, i);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(dataOffset);
        entry.dataSize = quint64(payload.size());
        entry.format = ModuleFormat::ZIP;
        entry.hierarchyPath = hierarchyFor(fullName);
        entry.data = std::move(payload);
        module->resources_.push_back(std::move(entry));
    }
    return module;
}

}  // namespace peare
