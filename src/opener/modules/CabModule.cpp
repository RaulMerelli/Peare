#include "CabModule.h"
#include "Compat.h"

#include "peare/lzx_frontends.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace peare {
namespace {

const qsizetype kCabHeaderSize = 36;
const qsizetype kMaxCabUncompressed = qsizetype(512) * 1024 * 1024;

struct CabFolder {
    quint32 dataOffset = 0;
    quint16 dataBlocks = 0;
    quint16 compression = 0;
    quint8 dataReserveBytes = 0;
    QByteArray decoded;
};

struct CabFile {
    quint32 size = 0;
    quint32 folderOffset = 0;
    quint16 folderIndex = 0;
    quint16 attribs = 0;
    QString name;
};

quint16 le16(const QByteArray& data, qsizetype offset, bool* ok = nullptr) {
    const bool valid = offset >= 0 && offset + 2 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& data, qsizetype offset, bool* ok = nullptr) {
    const bool valid = offset >= 0 && offset + 4 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString safeCabName(QString name, int index) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString leaf = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i) {
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    }
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("cab_file_%1.bin").arg(index, 3, 10, QLatin1Char('0'))
                          : leaf;
}

QStringList hierarchyForCabName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

bool inflateRawDeflate(const QByteArray& src, quint32 expected, QByteArray* out) {
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
    if (status != Z_STREAM_END || produced != expected) return false;
    return true;
}

bool decodeFolder(const QByteArray& cab, const CabFolder& folder, QByteArray* decoded,
                  QString* error) {
    decoded->clear();
    qsizetype pos = qsizetype(folder.dataOffset);
    const quint16 type = folder.compression & 0x000f;
    const quint16 lzxWindowBits = (folder.compression >> 8) & 0x001f;
    QByteArray lzxStream;
    quint32 lzxExpected = 0;
    for (quint16 i = 0; i < folder.dataBlocks; ++i) {
        if (pos + 8 + folder.dataReserveBytes > cab.size()) {
            *error = QStringLiteral("Truncated CAB data block");
            return false;
        }
        const quint16 cbData = le16(cab, pos + 4);
        const quint16 cbUncomp = le16(cab, pos + 6);
        pos += 8 + folder.dataReserveBytes;
        if (pos + cbData > cab.size()) {
            *error = QStringLiteral("Truncated CAB compressed block");
            return false;
        }
        const QByteArray block = cab.mid(pos, cbData);
        pos += cbData;
        QByteArray plain;
        if (type == 3) {
            lzxStream.append(block);
            if (lzxExpected + cbUncomp < lzxExpected ||
                lzxExpected + cbUncomp > quint32(kMaxCabUncompressed)) {
                *error = QStringLiteral("CAB folder exceeds safety limit");
                return false;
            }
            lzxExpected += cbUncomp;
            continue;
        } else if (type == 0) {
            if (cbData != cbUncomp) {
                *error = QStringLiteral("Invalid CAB uncompressed block size");
                return false;
            }
            plain = block;
        } else if (type == 1) {
            if (block.size() < 2 || block.at(0) != 'C' || block.at(1) != 'K') {
                *error = QStringLiteral("Invalid CAB MSZIP block signature");
                return false;
            }
            if (!inflateRawDeflate(block.mid(2), cbUncomp, &plain)) {
                *error = QStringLiteral("CAB MSZIP decompression failed");
                return false;
            }
        } else {
            *error = QStringLiteral("Unsupported CAB compression type %1").arg(type);
            return false;
        }
        if (decoded->size() + plain.size() > kMaxCabUncompressed) {
            *error = QStringLiteral("CAB folder exceeds safety limit");
            return false;
        }
        decoded->append(plain);
    }
    if (type == 3) {
        if (lzxWindowBits < 15 || lzxWindowBits > 21) {
            *error = QStringLiteral("Invalid CAB LZX window size");
            return false;
        }
        decoded->resize(int(lzxExpected));
        peare_lzx_cab_decoder* lzx = nullptr;
        peare_lzx_status status = peare_lzx_cab_create(size_t(1u) << lzxWindowBits, &lzx);
        if (status == PEARE_LZX_OK)
            status = peare_lzx_cab_decompress(lzx, lzxStream.constData(),
                                              size_t(lzxStream.size()), decoded->data(),
                                              size_t(decoded->size()));
        peare_lzx_cab_destroy(lzx);
        if (status != PEARE_LZX_OK) {
            *error = QStringLiteral("CAB LZX decompression failed");
            decoded->clear();
            return false;
        }
    }
    return true;
}

}  // namespace

std::unique_ptr<CabModule> CabModule::open(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<CabModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::CAB;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<CabModule> CabModule::open(const QByteArray& data, const QString& logicalName) {
    auto module = peare::makeUnique<CabModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::CAB;
    module->info_.description = QStringLiteral("Microsoft Cabinet (CAB) archive");

    if (data.size() < kCabHeaderSize || std::memcmp(data.constData(), "MSCF", 4) != 0) {
        module->info_.error = QStringLiteral("Invalid CAB signature or truncated header");
        return module;
    }
    const quint32 cbCabinet = le32(data, 8);
    const quint32 coffFiles = le32(data, 16);
    const quint16 cFolders = le16(data, 26);
    const quint16 cFiles = le16(data, 28);
    const quint16 flags = le16(data, 30);
    if (cbCabinet > quint32(data.size()) || coffFiles >= quint32(data.size()) || cFolders == 0) {
        module->info_.error = QStringLiteral("Invalid CAB header fields");
        return module;
    }
    qsizetype pos = kCabHeaderSize;
    quint8 folderReserve = 0;
    quint8 dataReserve = 0;
    if (flags & 0x0004) {
        if (pos + 4 > data.size()) {
            module->info_.error = QStringLiteral("Truncated CAB reserve header");
            return module;
        }
        const quint16 headerReserve = le16(data, pos);
        folderReserve = uchar(data.at(pos + 2));
        dataReserve = uchar(data.at(pos + 3));
        pos += 4 + headerReserve;
    }

    std::vector<CabFolder> folders;
    folders.reserve(cFolders);
    for (quint16 i = 0; i < cFolders; ++i) {
        if (pos + 8 > data.size()) {
            module->info_.error = QStringLiteral("Truncated CAB folder table");
            return module;
        }
        CabFolder folder;
        folder.dataOffset = le32(data, pos);
        folder.dataBlocks = le16(data, pos + 4);
        folder.compression = le16(data, pos + 6);
        folder.dataReserveBytes = dataReserve;
        folders.push_back(folder);
        pos += 8 + folderReserve;
    }

    std::vector<CabFile> files;
    pos = qsizetype(coffFiles);
    for (quint16 i = 0; i < cFiles; ++i) {
        if (pos + 16 > data.size()) {
            module->info_.error = QStringLiteral("Truncated CAB file table");
            return module;
        }
        CabFile file;
        file.size = le32(data, pos);
        file.folderOffset = le32(data, pos + 4);
        file.folderIndex = le16(data, pos + 8);
        file.attribs = le16(data, pos + 14);
        pos += 16;
        const qsizetype end = data.indexOf('\0', pos);
        if (end < 0) {
            module->info_.error = QStringLiteral("CAB filename is not NUL-terminated");
            return module;
        }
        file.name = QString::fromLocal8Bit(data.constData() + pos, int(end - pos));
        pos = end + 1;
        if (file.folderIndex >= folders.size()) {
            module->info_.error = QStringLiteral("CAB file references invalid folder");
            return module;
        }
        files.push_back(file);
    }

    for (std::size_t i = 0; i < folders.size(); ++i) {
        QString error;
        if (!decodeFolder(data, folders[i], &folders[i].decoded, &error)) {
            module->info_.error = error;
            return module;
        }
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        const CabFile& file = files[i];
        const CabFolder& folder = folders[file.folderIndex];
        if (file.folderOffset > quint32(folder.decoded.size()) ||
            file.size > quint32(folder.decoded.size()) - file.folderOffset) {
            module->info_.error = QStringLiteral("CAB file range exceeds decoded folder");
            return module;
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("CAB_FILE");
        entry.isEmbeddedFile = true;
        entry.name = safeCabName(file.name, int(i));
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = file.folderOffset;
        entry.dataSize = file.size;
        entry.format = ModuleFormat::CAB;
        entry.hierarchyPath = hierarchyForCabName(file.name);
        entry.data = folder.decoded.mid(qsizetype(file.folderOffset), qsizetype(file.size));
        module->resources_.push_back(std::move(entry));
    }
    return module;
}

}  // namespace peare
