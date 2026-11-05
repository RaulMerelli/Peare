#include "EotFont.h"
#include "third_party/libeot/src/peare_mtx_bridge.h"

#include <QtEndian>

#include <limits>

namespace peare {
namespace font {
namespace {

const quint32 kMaximumEotBytes = 128u * 1024u * 1024u;
const quint32 kCompressedFlag = 0x00000004u;
const quint32 kXorFlag = 0x10000000u;

quint16 readLe16(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool takeString(const QByteArray& data, int* offset, QString* value)
{
    if (*offset > data.size() - 4 || readLe16(data, *offset) != 0) return false;
    *offset += 2;
    const quint16 length = readLe16(data, *offset);
    *offset += 2;
    if ((length & 1) || length > data.size() - *offset) return false;
    if (length) {
        value->reserve(length / 2);
        for (int i = 0; i < length; i += 2)
            value->append(QChar(readLe16(data, *offset + i)));
    } else {
        value->clear();
    }
    *offset += length;
    return true;
}

} // namespace

bool hasEotHeader(const QByteArray& data)
{
    if (data.size() < 82 || quint32(data.size()) > kMaximumEotBytes) return false;
    const quint32 version = readLe32(data, 8);
    if (version != 0x00010000u && version != 0x00020001u && version != 0x00020002u)
        return false;
    const quint32 totalSize = readLe32(data, 0);
    const quint32 fontSize = readLe32(data, 4);
    if (totalSize < 82 || totalSize > kMaximumEotBytes || fontSize > totalSize - 82 ||
        readLe16(data, 34) != 0x504c || readLe16(data, 80) != 0)
        return false;
    for (int offset = 64; offset < 80; offset += 4)
        if (readLe32(data, offset) != 0) return false;
    return true;
}

bool parseEot(const QByteArray& data, EotFont* font, QString* error)
{
    if (!font || !hasEotHeader(data) || readLe32(data, 0) != quint32(data.size()))
        return fail(error, QStringLiteral("Invalid EOT header or total size"));

    const quint32 version = readLe32(data, 8);
    const quint32 flags = readLe32(data, 12);

    int offset = 80;
    QString family;
    QString style;
    QString ignored;
    if (!takeString(data, &offset, &family) || !takeString(data, &offset, &style) ||
        !takeString(data, &offset, &ignored) || !takeString(data, &offset, &ignored))
        return fail(error, QStringLiteral("Invalid EOT name strings"));

    if (version != 0x00010000u) {
        if (offset > data.size() - 4 || readLe16(data, offset) != 0)
            return fail(error, QStringLiteral("Invalid EOT root-string padding"));
        offset += 2;
        const quint16 rootLength = readLe16(data, offset);
        offset += 2;
        if ((rootLength & 1) || rootLength > data.size() - offset)
            return fail(error, QStringLiteral("Invalid EOT root string"));
        offset += rootLength;
        if (version == 0x00020002u) {
            if (offset > data.size() - 20 ||
                readLe16(data, offset + 8) != 0 ||
                readLe16(data, offset + 10) != 0 || readLe32(data, offset + 12) != 0 ||
                readLe32(data, offset + 16) != 0)
                return fail(error, QStringLiteral("Unsupported EOT 2.0.2 extension"));
            offset += 20;
        }
    }

    const quint32 fontSize = readLe32(data, 4);
    if (offset < 0 || quint32(data.size() - offset) != fontSize)
        return fail(error, QStringLiteral("EOT font data length mismatch"));

    QByteArray fontData = data.mid(offset, int(fontSize));
    QByteArray sfnt;
    if (flags & kCompressedFlag) {
        uint8_t* expanded = nullptr;
        uint32_t expandedSize = 0;
        const int status = peare_mtx_decompress(
            reinterpret_cast<const uint8_t*>(fontData.constData()),
            uint32_t(fontData.size()), (flags & kXorFlag) != 0, &expanded,
            &expandedSize);
        if (status != 0 || !expanded || expandedSize == 0 ||
            expandedSize > kMaximumEotBytes ||
            expandedSize > uint32_t((std::numeric_limits<int>::max)())) {
            peare_mtx_free(expanded);
            return fail(error, QStringLiteral("Invalid or unsupported EOT MTX data"));
        }
        sfnt = QByteArray(reinterpret_cast<const char*>(expanded), int(expandedSize));
        peare_mtx_free(expanded);
    } else {
        sfnt = fontData;
        if (flags & kXorFlag)
            for (int i = 0; i < sfnt.size(); ++i)
                sfnt[i] = char(uchar(sfnt.at(i)) ^ 0x50);
    }
    EotFont parsed;
    parsed.version = version;
    parsed.flags = flags;
    parsed.familyName = family;
    parsed.styleName = style;
    parsed.sfntBytes = sfnt;
    *font = parsed;
    return true;
}

} // namespace font
} // namespace peare
