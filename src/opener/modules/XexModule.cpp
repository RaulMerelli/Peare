#include "XexModule.h"

#include "Crypto.h"
#include "peare/lzx_frontends.h"

#include <QFile>
#include <QCryptographicHash>
#include <array>
#include <cstring>


namespace peare {
namespace {

constexpr std::array<unsigned char, 16> kXexRetailKey = {
    0x20, 0xB1, 0x85, 0xA5, 0x9D, 0x28, 0xFD, 0xC3,
    0x40, 0x58, 0x3F, 0xBB, 0x08, 0x96, 0xBF, 0x91
};
constexpr std::array<unsigned char, 16> kXexDevkitKey = {};

bool decryptXexPayload(const QByteArray& data, quint32 securityOffset, quint32 payloadOffset,
                       const std::array<unsigned char, 16>& masterKey,
                       QByteArray* decryptedData, QString* error)
{
    constexpr quint32 kSecurityAesKeyOffset = 0x150;
    if (!decryptedData || securityOffset > quint32(data.size()) ||
        quint64(securityOffset) + kSecurityAesKeyOffset + 16u > quint64(data.size()) ||
        payloadOffset > quint32(data.size())) {
        if (error) *error = QStringLiteral("XEX AES key or payload offset is outside the file.");
        return false;
    }
    const QByteArray encryptedSessionKey = data.mid(qsizetype(securityOffset + kSecurityAesKeyOffset), 16);
    QByteArray clearSessionKey;
    if (!aes128CbcDecryptNoPadding(encryptedSessionKey, masterKey, &clearSessionKey, error) || clearSessionKey.size() != 16)
        return false;
    std::array<unsigned char, 16> sessionKey{};
    std::memcpy(sessionKey.data(), clearSessionKey.constData(), sessionKey.size());
    const QByteArray encryptedPayload = data.mid(qsizetype(payloadOffset));
    QByteArray clearPayload;
    if (!aes128CbcDecryptNoPadding(encryptedPayload, sessionKey, &clearPayload, error)) return false;
    *decryptedData = data;
    decryptedData->replace(qsizetype(payloadOffset), clearPayload.size(), clearPayload);
    return true;
}

struct XexLzxPayloadBlock { quint32 fileOffset=0; quint32 totalSize=0; QByteArray payload; };
struct XexLzxStreamResult { QByteArray stream; quint32 chunkCount=0; QString error; bool ok() const { return error.isEmpty(); } };

quint16 xexBe16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16((quint16(p[0]) << 8) | p[1]);
}

XexLzxStreamResult buildXexLzxStream(const QVector<XexLzxPayloadBlock>& blocks)
{
    XexLzxStreamResult out;
    quint64 total = 0;
    for (int bi = 0; bi < blocks.size(); ++bi) {
        const QByteArray& payload = blocks[bi].payload;
        qsizetype pos = 0;
        bool terminated = false;
        while (pos + 2 <= payload.size()) {
            const quint16 size = xexBe16(payload, pos);
            pos += 2;
            if (size == 0) { terminated = true; break; }
            if (pos + size > payload.size()) {
                out.error = QStringLiteral("LZX block %1 contains a truncated chunk (%2 bytes requested, %3 available).")
                    .arg(bi).arg(size).arg(payload.size() - pos);
                return out;
            }
            total += size;
            if (total > 0x7fffffffULL) {
                out.error = QStringLiteral("Combined XEX LZX stream exceeds the safety limit.");
                return out;
            }
            out.stream.append(payload.constData() + pos, size);
            ++out.chunkCount;
            pos += size;
        }
        if (!terminated) {
            out.error = QStringLiteral("LZX block %1 has no zero-size chunk terminator.").arg(bi);
            return out;
        }
    }
    return out;
}
constexpr quint32 kResourceInfo = 0x000002FF;
constexpr quint32 kFileFormatInfo = 0x000003FF;
constexpr quint32 kImageBaseAddress = 0x00010201;
constexpr quint32 kEntryPoint = 0x00010100;
constexpr quint32 kImportLibraries = 0x000103FF;
constexpr quint32 kChecksumTimestamp = 0x00018002;
constexpr quint32 kOriginalPeName = 0x000183FF;
constexpr quint32 kStaticLibraries = 0x000200FF;
constexpr quint32 kTlsInfo = 0x00020104;
constexpr quint32 kSystemFlags = 0x00030000;
constexpr quint32 kExecutionId = 0x00040006;
constexpr quint32 kGameRatings = 0x00040310;
constexpr quint32 kLanKey = 0x00040404;
constexpr quint32 kXbox360Logo = 0x000405FF;

quint16 le16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 le64(const QByteArray& d, qsizetype o)
{
    return quint64(le32(d, o)) | (quint64(le32(d, o + 4)) << 32);
}

quint32 be32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | quint32(p[3]);
}

qsizetype findEmbeddedPe(const QByteArray& data, qsizetype preferred)
{
    auto valid = [&](qsizetype off) {
        if (off < 0 || off + 0x40 > data.size() || data.mid(off, 2) != QByteArrayLiteral("MZ")) return false;
        const quint32 pe = le32(data, off + 0x3c);
        return pe <= quint32(data.size() - off - 4) && data.mid(off + pe, 4) == QByteArrayLiteral("PE\0\0");
    };
    if (valid(preferred)) return preferred;
    qsizetype off = data.indexOf(QByteArrayLiteral("MZ"), qMax<qsizetype>(0, preferred));
    while (off >= 0) {
        if (valid(off)) return off;
        off = data.indexOf(QByteArrayLiteral("MZ"), off + 2);
    }
    return -1;
}


QString optionalHeaderName(quint32 key)
{
    switch (key) {
    case kResourceInfo: return QStringLiteral("RESOURCE_INFO");
    case kFileFormatInfo: return QStringLiteral("FILE_FORMAT_INFO");
    case 0x000080FF: return QStringLiteral("BOUNDING_PATH");
    case 0x00010001: return QStringLiteral("ORIGINAL_BASE_ADDRESS");
    case kEntryPoint: return QStringLiteral("ENTRY_POINT");
    case kImageBaseAddress: return QStringLiteral("IMAGE_BASE_ADDRESS");
    case kImportLibraries: return QStringLiteral("IMPORT_LIBRARIES");
    case kChecksumTimestamp: return QStringLiteral("CHECKSUM_TIMESTAMP");
    case kOriginalPeName: return QStringLiteral("ORIGINAL_PE_NAME");
    case kStaticLibraries: return QStringLiteral("STATIC_LIBRARIES");
    case kTlsInfo: return QStringLiteral("TLS_INFO");
    case 0x00020200: return QStringLiteral("DEFAULT_STACK_SIZE");
    case 0x00020301: return QStringLiteral("DEFAULT_FILESYSTEM_CACHE_SIZE");
    case 0x00020401: return QStringLiteral("DEFAULT_HEAP_SIZE");
    case kSystemFlags: return QStringLiteral("SYSTEM_FLAGS");
    case kExecutionId: return QStringLiteral("EXECUTION_ID");
    case kGameRatings: return QStringLiteral("GAME_RATINGS");
    case kLanKey: return QStringLiteral("LAN_KEY");
    case kXbox360Logo: return QStringLiteral("XBOX360_LOGO");
    default: return QStringLiteral("UNKNOWN_%1").arg(key, 8, 16, QLatin1Char('0')).toUpper();
    }
}

quint32 optionalPayloadSize(const QByteArray& data, quint32 key, quint32 value)
{
    const quint32 kind = key & 0xffu;
    if (kind == 0) return 0; // immediate value
    if (kind == 1) return 4;
    if (kind == 0xffu) {
        if (value > quint32(data.size() - 4)) return 0;
        const quint32 length = be32(data, value);
        return length >= 4 && quint64(value) + length <= quint64(data.size()) ? length : 0;
    }
    const quint64 bytes = quint64(kind) * 4u;
    return bytes <= 0xffffffffu && quint64(value) + bytes <= quint64(data.size()) ? quint32(bytes) : 0;
}

QString formatXexVersion(quint32 value)
{
    const quint32 major = (value >> 28) & 0x0f;
    const quint32 minor = (value >> 24) & 0x0f;
    const quint32 build = (value >> 8) & 0xffff;
    const quint32 qfe = value & 0xff;
    return QStringLiteral("%1.%2.%3.%4").arg(major).arg(minor).arg(build).arg(qfe);
}

QString asciiPayload(const QByteArray& payload)
{
    QByteArray value = payload;
    const int nul = value.indexOf('\0');
    if (nul >= 0) value.truncate(nul);
    for (char c : value) {
        const uchar u = uchar(c);
        if (u < 0x20 || u > 0x7e) return {};
    }
    return QString::fromLatin1(value);
}



QString compressionName(quint16 value)
{
    switch (value) {
    case 0: return QStringLiteral("none");
    case 1: return QStringLiteral("basic");
    case 2: return QStringLiteral("normal (LZX)");
    default: return QStringLiteral("unknown (%1)").arg(value);
    }
}

QString encryptionName(quint16 value)
{
    switch (value) {
    case 0: return QStringLiteral("none");
    case 1: return QStringLiteral("normal (AES)");
    default: return QStringLiteral("unknown (%1)").arg(value);
    }
}


struct XexFileFormat {
    bool valid = false;
    quint16 encryption = 0;
    quint16 compression = 0;
    QVector<QPair<quint32, quint32>> basicBlocks;
    quint32 lzxWindowSize = 0;
    quint32 firstLzxBlockSize = 0;
    QByteArray firstLzxBlockHash;
};

XexFileFormat readFileFormatInfo(const QByteArray& payload)
{
    XexFileFormat out;
    if (payload.size() < 8) return out;
    const quint32 declaredSize = be32(payload, 0);
    if (declaredSize < 8 || declaredSize > quint32(payload.size())) return out;
    out.encryption = quint16((uchar(payload[4]) << 8) | uchar(payload[5]));
    out.compression = quint16((uchar(payload[6]) << 8) | uchar(payload[7]));
    if (out.compression == 1) {
        if ((declaredSize - 8) % 8 != 0) return out;
        const quint32 count = (declaredSize - 8) / 8;
        out.basicBlocks.reserve(int(count));
        for (quint32 i = 0; i < count; ++i) {
            out.basicBlocks.push_back(qMakePair(
                be32(payload, 8 + qsizetype(i) * 8),
                be32(payload, 12 + qsizetype(i) * 8)));
        }
    } else if (out.compression == 2) {
        if (declaredSize < 36) return out;
        out.lzxWindowSize = be32(payload, 8);
        out.firstLzxBlockSize = be32(payload, 12);
        out.firstLzxBlockHash = payload.mid(16, 20);
        if (out.firstLzxBlockHash.size() != 20) return out;
    }
    out.valid = true;
    return out;
}

struct XexLzxBlock {
    quint32 fileOffset = 0;
    quint32 totalSize = 0;
    quint32 nextSize = 0;
    QByteArray expectedHash;
    QByteArray actualHash;
    QByteArray compressedPayload;
};

bool parseNormalCompressionChain(const QByteArray& xex, quint32 imageOffset,
                                 const XexFileFormat& format, QVector<XexLzxBlock>* blocks,
                                 QString* error)
{
    if (!blocks) return false;
    blocks->clear();
    if (!format.valid || format.compression != 2) {
        if (error) *error = QStringLiteral("Normal-compression descriptor is missing or invalid.");
        return false;
    }
    if (format.firstLzxBlockSize == 0) return true;
    quint64 offset = imageOffset;
    quint32 size = format.firstLzxBlockSize;
    QByteArray expected = format.firstLzxBlockHash;
    constexpr int kMaxBlocks = 65536;
    for (int index = 0; size != 0; ++index) {
        if (index >= kMaxBlocks) {
            if (error) *error = QStringLiteral("LZX block chain exceeds the safety limit.");
            return false;
        }
        if (size < 24 || offset + size > quint64(xex.size())) {
            if (error) *error = QStringLiteral("LZX block %1 is truncated or smaller than its 24-byte chain header.").arg(index);
            return false;
        }
        const QByteArray raw = xex.mid(qsizetype(offset), int(size));
        XexLzxBlock block;
        block.fileOffset = quint32(offset);
        block.totalSize = size;
        block.nextSize = be32(raw, 0);
        block.expectedHash = expected;
        block.actualHash = QCryptographicHash::hash(raw, QCryptographicHash::Sha1);
        block.compressedPayload = raw.mid(24);
        const QByteArray nextHash = raw.mid(4, 20);
        if (block.expectedHash.size() == 20 && block.actualHash != block.expectedHash) {
            if (error) *error = QStringLiteral("LZX block %1 failed SHA-1 validation.").arg(index);
            return false;
        }
        blocks->push_back(std::move(block));
        offset += size;
        size = blocks->constLast().nextSize;
        expected = nextHash;
    }
    return true;
}

bool reconstructBasicImage(const QByteArray& xex, quint32 imageOffset, quint32 expectedImageSize,
                           const XexFileFormat& format, QByteArray* image, QString* error)
{
    if (!image) return false;
    image->clear();
    if (!format.valid || format.encryption != 0 || format.compression != 1) {
        if (error) *error = QStringLiteral("Basic reconstruction requires an unencrypted basic-compression descriptor.");
        return false;
    }
    if (imageOffset > quint32(xex.size())) {
        if (error) *error = QStringLiteral("XEX image data offset points outside the file.");
        return false;
    }
    quint64 outputSize = 0;
    quint64 storedSize = 0;
    for (const auto& block : format.basicBlocks) {
        storedSize += block.first;
        outputSize += quint64(block.first) + block.second;
        if (storedSize > quint64(xex.size()) - imageOffset || outputSize > 0x7fffffffu) {
            if (error) *error = QStringLiteral("Basic-compression block table exceeds safe file or image bounds.");
            return false;
        }
    }
    if (expectedImageSize && outputSize != expectedImageSize) {
        if (error) *error = QStringLiteral("Basic-compression output size (%1) does not match security image size (%2).")
            .arg(outputSize).arg(expectedImageSize);
        return false;
    }
    image->reserve(int(outputSize));
    qsizetype source = imageOffset;
    for (const auto& block : format.basicBlocks) {
        if (block.first) {
            image->append(xex.constData() + source, int(block.first));
            source += block.first;
        }
        if (block.second) image->append(QByteArray(int(block.second), char(0)));
    }
    return true;
}

QString parseFileFormatInfo(const QByteArray& payload)
{
    if (payload.size() < 8) return QStringLiteral("Malformed FILE_FORMAT_INFO payload\n");
    const quint32 size = be32(payload, 0);
    const quint16 encryption = quint16((uchar(payload[4]) << 8) | uchar(payload[5]));
    const quint16 compression = quint16((uchar(payload[6]) << 8) | uchar(payload[7]));
    QString out = QStringLiteral("Structure size: %1 bytes\nEncryption: %2\nCompression: %3\n")
        .arg(size).arg(encryptionName(encryption)).arg(compressionName(compression));
    if (compression == 1) {
        const int count = qMax(0, (qMin<int>(size, payload.size()) - 8) / 8);
        out += QStringLiteral("Basic compression blocks: %1\n").arg(count);
        for (int i = 0; i < count; ++i) {
            out += QStringLiteral("  [%1] data=%2 zero-fill=%3\n").arg(i)
                .arg(be32(payload, 8 + i * 8)).arg(be32(payload, 12 + i * 8));
        }
    } else if (compression == 2 && payload.size() >= 36) {
        out += QStringLiteral("LZX window size: %1 bytes\nFirst compressed block: %2 bytes\n")
            .arg(be32(payload, 8)).arg(be32(payload, 12));
        out += QStringLiteral("First block hash (SHA-1): %1\n")
            .arg(QString::fromLatin1(payload.mid(16, 20).toHex()));
    }
    return out;
}

QString parseTlsInfo(const QByteArray& payload)
{
    if (payload.size() < 16) return QStringLiteral("Malformed TLS_INFO payload\n");
    return QStringLiteral("Slot count: %1\nRaw data address: 0x%2\nData size: %3 bytes\nRaw data size: %4 bytes\n")
        .arg(be32(payload, 0)).arg(be32(payload, 4), 8, 16, QLatin1Char('0'))
        .arg(be32(payload, 8)).arg(be32(payload, 12));
}

QString parseImportLibraries(const QByteArray& payload)
{
    if (payload.size() < 12) return QStringLiteral("Malformed IMPORT_LIBRARIES payload\n");
    const quint32 totalSize = be32(payload, 0);
    const quint32 stringSize = be32(payload, 4);
    const quint32 libraryCount = be32(payload, 8);
    if (totalSize < 12 || totalSize > quint32(payload.size()) || stringSize > totalSize - 12)
        return QStringLiteral("Malformed IMPORT_LIBRARIES bounds\n");
    const QByteArray strings = payload.mid(12, stringSize);
    qsizetype cursor = 12 + stringSize;
    QStringList names;
    for (const QByteArray& item : strings.split('\0')) if (!item.isEmpty()) names.push_back(QString::fromLatin1(item));
    QString out = QStringLiteral("Library count: %1\nString table size: %2 bytes\n").arg(libraryCount).arg(stringSize);
    for (quint32 i = 0; i < libraryCount; ++i) {
        if (cursor + 32 > totalSize) { out += QStringLiteral("[%1] truncated library record\n").arg(i); break; }
        const QByteArray digest = payload.mid(cursor, 20);
        const quint32 version = be32(payload, cursor + 20);
        const quint32 minimum = be32(payload, cursor + 24);
        const quint16 nameIndex = quint16((uchar(payload.at(int(cursor + 28))) << 8) | uchar(payload.at(int(cursor + 29))));
        const quint16 recordCount = quint16((uchar(payload.at(int(cursor + 30))) << 8) | uchar(payload.at(int(cursor + 31))));
        cursor += 32;
        const QString name = nameIndex < names.size() ? names[nameIndex] : QStringLiteral("string[%1]").arg(nameIndex);
        out += QStringLiteral("\n[%1] %2\nVersion: %3\nMinimum version: %4\nImport records: %5\nDigest: %6\n")
            .arg(i).arg(name).arg(formatXexVersion(version)).arg(formatXexVersion(minimum)).arg(recordCount)
            .arg(QString::fromLatin1(digest.toHex()));
        const quint64 bytes = quint64(recordCount) * 4;
        if (quint64(cursor) + bytes > totalSize) { out += QStringLiteral("Import record table is truncated\n"); break; }
        for (quint16 j = 0; j < recordCount; ++j) {
            const quint32 address = be32(payload, cursor + qsizetype(j) * 4);
            out += QStringLiteral("  [%1] 0x%2\n").arg(j).arg(address, 8, 16, QLatin1Char('0'));
        }
        cursor += qsizetype(bytes);
    }
    return out;
}

QString parseSecurityInfo(const QByteArray& data, quint32 offset)
{
    if (offset > quint32(data.size() - 8)) return QStringLiteral("Security information points outside the file\n");
    const quint32 headerSize = be32(data, offset);
    const quint32 imageSize = be32(data, offset + 4);
    if (headerSize < 8 || quint64(offset) + headerSize > quint64(data.size()))
        return QStringLiteral("Malformed XEX security information\n");
    QString out = QStringLiteral("Header size: %1 bytes\nImage size: %2 bytes\nRSA signature: %3 bytes\n")
        .arg(headerSize).arg(imageSize).arg(qMin<quint32>(256, headerSize - 8));
    if (headerSize >= 0x114) {
        out += QStringLiteral("Image flags: 0x%1\nLoad address: 0x%2\n")
            .arg(be32(data, offset + 0x108), 8, 16, QLatin1Char('0'))
            .arg(be32(data, offset + 0x110), 8, 16, QLatin1Char('0'));
    }
    return out;
}

QString resourceName(const QByteArray& raw, int index)
{
    const int nul = raw.indexOf('\0');
    const QByteArray trimmed = (nul >= 0 ? raw.left(nul) : raw).trimmed();
    bool printable = !trimmed.isEmpty();
    for (char ch : trimmed) {
        const uchar c = uchar(ch);
        if (c < 0x20 || c > 0x7e) { printable = false; break; }
    }
    return printable ? QString::fromLatin1(trimmed)
                     : QStringLiteral("resource_%1").arg(index, 3, 10, QLatin1Char('0'));
}

struct PeLayout {
    bool valid = false;
    quint64 imageBase = 0;
    quint32 sizeOfHeaders = 0;
    qsizetype sectionTable = 0;
    quint16 sectionCount = 0;
};

PeLayout readPeLayout(const QByteArray& pe)
{
    PeLayout out;
    if (pe.size() < 0x40 || pe.left(2) != QByteArrayLiteral("MZ")) return out;
    const quint32 nt = le32(pe, 0x3c);
    if (nt > quint32(pe.size() - 24) || pe.mid(nt, 4) != QByteArrayLiteral("PE\0\0")) return out;
    out.sectionCount = le16(pe, nt + 6);
    const quint16 optionalSize = le16(pe, nt + 20);
    const qsizetype optional = qsizetype(nt) + 24;
    if (optional + optionalSize > pe.size() || optionalSize < 64) return out;
    const quint16 magic = le16(pe, optional);
    if (magic == 0x10b) out.imageBase = le32(pe, optional + 28);
    else if (magic == 0x20b) out.imageBase = le64(pe, optional + 24);
    else return out;
    out.sizeOfHeaders = le32(pe, optional + 60);
    out.sectionTable = optional + optionalSize;
    if (out.sectionTable + qsizetype(out.sectionCount) * 40 > pe.size()) return out;
    out.valid = true;
    return out;
}

qsizetype mapVirtualAddress(const QByteArray& pe, const PeLayout& layout, quint32 xexImageBase,
                            quint32 address, quint32 size)
{
    if (!layout.valid) return -1;
    const quint64 base = xexImageBase ? xexImageBase : layout.imageBase;
    if (quint64(address) < base) return -1;
    const quint64 rva64 = quint64(address) - base;
    if (rva64 > 0xffffffffu) return -1;
    const quint32 rva = quint32(rva64);
    if (rva < layout.sizeOfHeaders && quint64(rva) + size <= quint64(pe.size())) return rva;
    for (quint16 i = 0; i < layout.sectionCount; ++i) {
        const qsizetype sh = layout.sectionTable + qsizetype(i) * 40;
        const quint32 virtualSize = le32(pe, sh + 8);
        const quint32 virtualAddress = le32(pe, sh + 12);
        const quint32 rawSize = le32(pe, sh + 16);
        const quint32 rawOffset = le32(pe, sh + 20);
        const quint64 span = qMax(virtualSize, rawSize);
        if (rva < virtualAddress || quint64(rva) + size > quint64(virtualAddress) + span) continue;
        const quint64 delta = quint64(rva) - virtualAddress;
        if (delta + size > rawSize || quint64(rawOffset) + delta + size > quint64(pe.size())) return -1;
        return qsizetype(quint64(rawOffset) + delta);
    }
    return -1;
}

qsizetype mapLoadedImageAddress(const QByteArray& image, const PeLayout& layout,
                                    quint32 imageBase, quint32 address, quint32 size)
{
    const qsizetype peRaw = mapVirtualAddress(image, layout, imageBase, address, size);
    if (peRaw >= 0) return peRaw;
    if (imageBase == 0 || quint64(address) < imageBase) return -1;
    const quint64 offset = quint64(address) - imageBase;
    if (offset + size > quint64(image.size())) return -1;
    return qsizetype(offset);
}

void appendRawResource(QVector<ResourceEntry>& resources, const QString& type, const QString& name,
                       const QByteArray& payload, quint64 sourceOffset,
                       const QStringList& hierarchyPath)
{
    if (payload.isEmpty()) return;
    ResourceEntry entry;
    entry.type = type;
    entry.name = name;
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = sourceOffset;
    entry.format = ModuleFormat::XEX;
    entry.data = payload;
    entry.dataSize = quint64(entry.data.size());
    entry.hierarchyPath = hierarchyPath;
    resources.push_back(std::move(entry));
}

}

std::unique_ptr<XexModule> XexModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<XexModule>(new XexModule);
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::XEX;
    module->info_.description = QStringLiteral("Xbox 360 Executable (XEX)");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { module->info_.error = file.errorString(); return module; }
    const QByteArray data = file.readAll();
    const QByteArray magic = data.left(4);
    if (data.size() < 24 || (magic != QByteArrayLiteral("XEX1") && magic != QByteArrayLiteral("XEX2"))) {
        module->info_.error = QStringLiteral("Invalid XEX1/XEX2 header"); return module;
    }
    module->info_.description = magic == QByteArrayLiteral("XEX1")
        ? QStringLiteral("Xbox 360 Executable (XEX1)")
        : QStringLiteral("Xbox 360 Executable (XEX2)");

    const quint32 peDataOffset = be32(data, 8);
    const quint32 securityOffset = be32(data, 16);
    const quint32 optionalCount = be32(data, 20);
    module->info_.headerOffset = peDataOffset;
    if (securityOffset <= quint32(data.size() - 4)) {
        quint32 securitySize = be32(data, securityOffset);
        if (securitySize < 4 || quint64(securityOffset) + securitySize > quint64(data.size()))
            securitySize = qMin<quint32>(0x180u, quint32(data.size()) - securityOffset);
        appendRawResource(module->resources_, QStringLiteral("XEX_SECURITY_INFO"),
                          QStringLiteral("security_info"), data.mid(securityOffset, securitySize),
                          securityOffset, QStringList());
    }
    if (optionalCount > quint32((data.size() - 24) / 8)) {
        module->info_.error = QStringLiteral("Invalid XEX optional-header table");
        return module;
    }

    quint32 resourceInfoOffset = 0;
    quint32 xexImageBase = 0;
    if (securityOffset <= quint32(data.size() - 0x114))
        xexImageBase = be32(data, securityOffset + 0x110);
    quint32 fileFormatInfoOffset = 0;
    quint32 fileFormatInfoSize = 0;
    QString originalPeName;
    for (quint32 i = 0; i < optionalCount; ++i) {
        const qsizetype item = 24 + qsizetype(i) * 8;
        const quint32 key = be32(data, item);
        const quint32 value = be32(data, item + 4);
        const quint32 payloadSize = optionalPayloadSize(data, key, value);
        if (key == kResourceInfo) resourceInfoOffset = value;
        if (key == kImageBaseAddress && value != 0) xexImageBase = value;
        if (key == kFileFormatInfo) { fileFormatInfoOffset = value; fileFormatInfoSize = payloadSize; }

        if (key == kEntryPoint || key == kImageBaseAddress || key == kSystemFlags || key == 0x00010001 ||
            key == 0x00020200) {
            appendRawResource(module->resources_, QStringLiteral("XEX_OPTIONAL_HEADER"),
                              optionalHeaderName(key), data.mid(item, 8), quint64(item),
                              QStringList());
        } else if (payloadSize && value <= quint32(data.size() - payloadSize)) {
            const QByteArray payload = data.mid(value, payloadSize);
            ResourceEntry raw;
            raw.type = QStringLiteral("XEX_OPTIONAL_DATA");
            raw.name = optionalHeaderName(key);
            raw.language = QStringLiteral("neutral");
            raw.dataOffset = value; raw.dataSize = payloadSize; raw.format = ModuleFormat::XEX; raw.data = payload;
            raw.hierarchyPath = QStringList();
            module->resources_.push_back(std::move(raw));

            if (key == kOriginalPeName && payloadSize >= 4)
                originalPeName = asciiPayload(payload.mid(4));
        }
    }

    qsizetype peOffset = findEmbeddedPe(data, peDataOffset);
    QByteArray peImage;
    PeLayout peLayout;
    bool reconstructedImage = false;
    if (peOffset >= 0) {
        peImage = data.mid(peOffset);
    } else if (fileFormatInfoOffset && fileFormatInfoSize &&
               fileFormatInfoOffset <= quint32(data.size() - fileFormatInfoSize)) {
        const XexFileFormat fileFormat = readFileFormatInfo(data.mid(fileFormatInfoOffset, fileFormatInfoSize));
        XexFileFormat decodedFormat = fileFormat;
        const quint32 expectedImageSize = securityOffset <= quint32(data.size() - 8)
            ? be32(data, securityOffset + 4) : 0;
        QByteArray imageData = data;
        QString decryptionMode;
        QString reconstructionError;
        if (fileFormat.valid && fileFormat.encryption == 1) {
            const std::array<std::array<unsigned char, 16>, 2> candidates = {kXexRetailKey, kXexDevkitKey};
            const QStringList names = {QStringLiteral("retail"), QStringLiteral("devkit")};
            bool decrypted = false;
            for (int candidate = 0; candidate < int(candidates.size()); ++candidate) {
                QByteArray trial;
                QString aesError;
                if (!decryptXexPayload(data, securityOffset, peDataOffset, candidates[size_t(candidate)], &trial, &aesError)) {
                    reconstructionError = aesError;
                    continue;
                }
                if (fileFormat.compression == 2) {
                    QVector<XexLzxBlock> validationBlocks;
                    QString validationError;
                    if (!parseNormalCompressionChain(trial, peDataOffset, fileFormat, &validationBlocks, &validationError)) {
                        reconstructionError = validationError;
                        continue;
                    }
                }
                imageData = std::move(trial);
                decryptionMode = names[candidate];
                decrypted = true;
                decodedFormat.encryption = 0;
                break;
            }
            if (!decrypted) {
                module->info_.error = QStringLiteral("XEX AES decryption failed: %1").arg(reconstructionError);
            }
        }
        if (module->info_.error.isEmpty() && decodedFormat.valid && decodedFormat.compression == 0 &&
            peDataOffset <= quint32(imageData.size())) {
            peImage = imageData.mid(qsizetype(peDataOffset));
            if (expectedImageSize && quint32(peImage.size()) > expectedImageSize)
                peImage.truncate(int(expectedImageSize));
            reconstructedImage = true;
            peOffset = peDataOffset;
        } else if (module->info_.error.isEmpty() && reconstructBasicImage(imageData, peDataOffset, expectedImageSize, decodedFormat,
                                  &peImage, &reconstructionError)) {
            reconstructedImage = true;
            peOffset = peDataOffset;
        } else if (module->info_.error.isEmpty() && fileFormat.valid && fileFormat.compression == 1) {
        } else if (module->info_.error.isEmpty() && fileFormat.valid && fileFormat.compression == 2) {
            QVector<XexLzxBlock> blocks;
            QString chainError;
            if (parseNormalCompressionChain(imageData, peDataOffset, fileFormat, &blocks, &chainError)) {
                QString summary = QStringLiteral("Encryption: %1\nLZX window size: %2 bytes\nBlock count: %3\n")
                    .arg(decryptionMode.isEmpty() ? QStringLiteral("none") : QStringLiteral("AES (%1 key)").arg(decryptionMode)).arg(fileFormat.lzxWindowSize).arg(blocks.size());
                quint64 compressedBytes = 0;
                for (int i = 0; i < blocks.size(); ++i) {
                    const XexLzxBlock& block = blocks[i];
                    compressedBytes += quint64(block.compressedPayload.size());
                    summary += QStringLiteral("[%1] file=0x%2 total=%3 payload=%4 SHA-1=%5\n")
                        .arg(i).arg(block.fileOffset, 8, 16, QLatin1Char('0')).arg(block.totalSize)
                        .arg(block.compressedPayload.size()).arg(QString::fromLatin1(block.actualHash.toHex()));
                }
                QVector<XexLzxPayloadBlock> payloadBlocks;
                payloadBlocks.reserve(blocks.size());
                for (const XexLzxBlock& block : blocks) {
                    XexLzxPayloadBlock payloadBlock;
                    payloadBlock.fileOffset = block.fileOffset;
                    payloadBlock.totalSize = block.totalSize;
                    payloadBlock.payload = block.compressedPayload;
                    payloadBlocks.push_back(payloadBlock);
                }
                const XexLzxStreamResult stream = buildXexLzxStream(payloadBlocks);
                if (stream.ok()) {
                    QByteArray reconstructed;
                    peare_lzx_xex_decoder *decoder = nullptr;
                    peare_lzx_status decoderStatus = PEARE_LZX_INVALID_ARGUMENT;
                    if (expectedImageSize > 0 && fileFormat.lzxWindowSize > 0) {
                        reconstructed.resize(qsizetype(expectedImageSize));
                        decoderStatus = peare_lzx_xex_create(fileFormat.lzxWindowSize,
                                                            expectedImageSize, &decoder);
                        if (decoderStatus == PEARE_LZX_OK) {
                            decoderStatus = peare_lzx_xex_decompress(
                                decoder, stream.stream.constData(), size_t(stream.stream.size()),
                                reconstructed.data(), size_t(reconstructed.size()));
                        }
                        peare_lzx_xex_destroy(decoder);
                    }
                    if (decoderStatus == PEARE_LZX_OK) {
                        peImage = reconstructed;
                        reconstructedImage = true;
                        peOffset = peDataOffset;
                        summary += QStringLiteral("Compressed payload bytes: %1\nDeblocked stream bytes: %2\nChunk count: %3\nSHA-1 chain: valid\nDecoder status: success\nReconstructed image bytes: %4\n")
                            .arg(compressedBytes).arg(stream.stream.size()).arg(stream.chunkCount).arg(peImage.size());
                    } else {
                        summary += QStringLiteral("Compressed payload bytes: %1\nDeblocked stream bytes: %2\nChunk count: %3\nSHA-1 chain: valid\nDecoder status: failed (%4)\n")
                            .arg(compressedBytes).arg(stream.stream.size()).arg(stream.chunkCount).arg(int(decoderStatus));
                    }
                } else {
                    summary += QStringLiteral("Compressed payload bytes: %1\nSHA-1 chain: valid\nDeblocking error: %2\n")
                        .arg(compressedBytes).arg(stream.error);
                }
            } else {
            }
        }
    }
    if (!peImage.isEmpty()) {
        peLayout = readPeLayout(peImage);
        if (peLayout.valid) {
            const QString embeddedName = originalPeName.isEmpty() ? QStringLiteral("image.pe") : originalPeName;

            ResourceEntry peModule;
            peModule.type = QStringLiteral("PE_MODULE");
            peModule.name = embeddedName;
            peModule.language = QStringLiteral("neutral");
            peModule.dataOffset = 0;
            peModule.dataSize = quint64(peImage.size());
            peModule.format = ModuleFormat::PE;
            peModule.hierarchyPath = QStringList() << embeddedName;
            peModule.data = peImage;
            module->resources_.push_back(std::move(peModule));

        } else {
            ResourceEntry loadedImage;
            loadedImage.type = QStringLiteral("XEX_LOADED_IMAGE");
            loadedImage.name = QStringLiteral("loaded_image");
            loadedImage.language = QStringLiteral("neutral");
            loadedImage.dataOffset = 0;
            loadedImage.dataSize = quint64(peImage.size());
            loadedImage.format = ModuleFormat::XEX;
            loadedImage.hierarchyPath = QStringList();
            loadedImage.data = peImage;
            module->resources_.push_back(std::move(loadedImage));
        }
    }

    if (resourceInfoOffset != 0) {
        if (resourceInfoOffset > quint32(data.size() - 4)) {
            module->info_.error = QStringLiteral("XEX RESOURCE_INFO points outside the file");
            return module;
        }
        const quint32 blockSize = be32(data, resourceInfoOffset);
        if (blockSize < 4 || (blockSize - 4) % 16 != 0 ||
            quint64(resourceInfoOffset) + blockSize > quint64(data.size())) {
            module->info_.error = QStringLiteral("Malformed XEX RESOURCE_INFO block");
            return module;
        }
        const quint32 count = (blockSize - 4) / 16;
        for (quint32 i = 0; i < count; ++i) {
            const qsizetype descriptor = qsizetype(resourceInfoOffset) + 4 + qsizetype(i) * 16;
            const QByteArray rawName = data.mid(descriptor, 8);
            const QString name = resourceName(rawName, int(i));
            const quint32 address = be32(data, descriptor + 8);
            const quint32 size = be32(data, descriptor + 12);
            appendRawResource(module->resources_, QStringLiteral("XEX_RESOURCE_INFO"),
                              name, data.mid(descriptor, 16), quint64(descriptor),
                              QStringList());

            if (!peImage.isEmpty() && size != 0) {
                const qsizetype raw = mapLoadedImageAddress(peImage, peLayout, xexImageBase, address, size);
                if (raw >= 0) {
                    const quint64 absoluteResourceOffset =
                        reconstructedImage ? quint64(raw) : quint64(peOffset + raw);
                    const QByteArray resourceData = peImage.mid(raw, size);

                    ResourceEntry entry;
                    entry.name = name;
                    entry.language = QStringLiteral("neutral");
                    entry.dataOffset = absoluteResourceOffset;
                    entry.dataSize = size;
                    entry.data = resourceData;

                    // Expose the payload exactly once. OpenerSession is the only
                    // component allowed to detect and expand nested modules.
                    entry.type = QStringLiteral("XEX_RESOURCE");
                    entry.format = ModuleFormat::Unknown;
                    entry.hierarchyPath = QStringList();
                    module->resources_.push_back(std::move(entry));
                }
            }
        }
    }

    for (ResourceEntry& entry : module->resources_) {
        if (entry.hierarchyPath.isEmpty()) {
            if (entry.type.startsWith(QStringLiteral("XEX_LZX")))
                entry.hierarchyPath = QStringList();
            else
                entry.hierarchyPath = QStringList() << entry.type;
        }
    }
    return module;
}

} // namespace peare
