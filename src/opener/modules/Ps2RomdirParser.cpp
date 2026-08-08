#include "Ps2RomdirParser.h"

#include <QStringList>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace peare {
namespace {

quint16 le16(const std::uint8_t* p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::uint8_t* p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 align16(quint64 value)
{
    return (value + 15ULL) & ~15ULL;
}

quint64 align4(quint64 value)
{
    return (value + 3ULL) & ~3ULL;
}

QString entryName(const std::uint8_t* p)
{
    int length = 0;
    while (length < 10 && p[length] != 0) ++length;
    if (length == 0) return {};
    for (int i = 0; i < length; ++i) {
        if (p[i] < 0x20 || p[i] > 0x7e) return {};
    }
    return QString::fromLatin1(reinterpret_cast<const char*>(p), length);
}

bool zeroEntry(const std::uint8_t* p)
{
    for (int i = 0; i < 16; ++i) if (p[i] != 0) return false;
    return true;
}

QString printableText(const std::uint8_t* p, int length)
{
    QByteArray text(reinterpret_cast<const char*>(p), length);
    const int nul = text.indexOf('\0');
    if (nul >= 0) text.truncate(nul);
    for (int i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text.at(i));
        if (c < 0x20 && c != '\t' && c != '\r' && c != '\n') text[i] = ' ';
    }
    return QString::fromLatin1(text).simplified();
}

QString extInfoSummary(const fs::ByteStorePtr& store, quint64 offset, quint16 size)
{
    if (!store || size < 4 || offset > quint64(store->capacity()) ||
        size > quint64(store->capacity()) - offset)
        return {};

    const std::vector<std::uint8_t> bytes = store->readRange(qint64(offset), size);
    QStringList fields;
    quint64 pos = 0;
    while (pos + 4 <= bytes.size()) {
        const std::uint8_t* p = bytes.data() + pos;
        const quint16 value = le16(p);
        const quint8 extra = p[2];
        const quint8 type = p[3];
        if (value == 0 && extra == 0 && type == 0) break;
        const quint64 recordSize = align4(4ULL + extra);
        if (recordSize > bytes.size() - pos) break;

        if (type == 2) {
            fields << QStringLiteral("version 0x%1").arg(value, 4, 16, QLatin1Char('0'));
        } else if (type == 3 && extra) {
            const QString comment = printableText(p + 4, extra);
            if (!comment.isEmpty()) fields << comment;
        } else if (type == 1 && extra) {
            const QString date = printableText(p + 4, extra);
            if (!date.isEmpty()) fields << date;
        }
        pos += recordSize;
    }
    return fields.join(QStringLiteral(" — "));
}

bool hasRequiredHeader(const fs::ByteStorePtr& store, quint64 base)
{
    if (!store || base > quint64(store->capacity()) ||
        quint64(store->capacity()) - base < 48)
        return false;
    const std::vector<std::uint8_t> head = store->readRange(qint64(base), 48);
    if (head.size() != 48) return false;
    // In a complete PS2 BIOS the RESET entry describes the bytes preceding
    // ROMDIR, therefore its size is the ROMDIR file offset.  Standalone IOPRP
    // images legitimately place ROMDIR at zero and consequently use size 0.
    const quint32 resetSize = le32(head.data() + 12);
    return entryName(head.data()) == QStringLiteral("RESET") &&
           (resetSize == 0 || resetSize == base) &&
           entryName(head.data() + 16) == QStringLiteral("ROMDIR") &&
           entryName(head.data() + 32) == QStringLiteral("EXTINFO");
}

} // namespace

bool parsePs2Romdir(const fs::ByteStorePtr& store, quint64 baseOffset,
                    Ps2RomdirImageInfo* image, QString* error)
{
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!image) return fail(QStringLiteral("Missing ROMDIR output"));
    *image = {};
    if (!hasRequiredHeader(store, baseOffset))
        return fail(QStringLiteral("Invalid PS2 ROMDIR header"));

    const std::vector<std::uint8_t> head = store->readRange(qint64(baseOffset), 48);
    const quint32 romdirSize = le32(head.data() + 16 + 12);
    const quint32 extinfoSize = le32(head.data() + 32 + 12);
    if (romdirSize < 64 || (romdirSize & 15U) != 0 || romdirSize > 16U * 1024U * 1024U)
        return fail(QStringLiteral("Invalid ROMDIR section size"));
    if (baseOffset > quint64(store->capacity()) ||
        romdirSize > quint64(store->capacity()) - baseOffset)
        return fail(QStringLiteral("Truncated ROMDIR section"));

    const std::vector<std::uint8_t> directory =
        store->readRange(qint64(baseOffset), romdirSize);
    QVector<Ps2RomdirEntryInfo> entries;
    bool terminated = false;
    for (quint64 pos = 0; pos + 16 <= directory.size(); pos += 16) {
        const std::uint8_t* raw = directory.data() + pos;
        if (zeroEntry(raw)) {
            terminated = true;
            break;
        }
        const QString name = entryName(raw);
        if (name.isEmpty()) return fail(QStringLiteral("Invalid ROMDIR file name"));
        Ps2RomdirEntryInfo entry;
        entry.name = name;
        entry.extInfoSize = le16(raw + 10);
        entry.fileSize = le32(raw + 12);
        entries.push_back(entry);
        if (entries.size() > 65535)
            return fail(QStringLiteral("Too many ROMDIR entries"));
    }
    if (!terminated || entries.size() < 3 ||
        entries[0].name != QStringLiteral("RESET") ||
        (entries[0].fileSize != 0 && entries[0].fileSize != baseOffset) ||
        entries[1].name != QStringLiteral("ROMDIR") || entries[1].fileSize != romdirSize ||
        entries[2].name != QStringLiteral("EXTINFO") || entries[2].fileSize != extinfoSize)
        return fail(QStringLiteral("Invalid required ROMDIR entries"));

    // ROMDIR sizes describe the complete linear ROM image beginning at byte 0.
    // The RESET payload occupies [0, baseOffset), followed by ROMDIR, EXTINFO
    // and the remaining files, each aligned to 16 bytes.
    const bool completeRomImage = entries[0].fileSize == baseOffset;
    quint64 dataOffset = completeRomImage ? 0 : baseOffset;
    quint64 extOffset = 0;
    quint64 totalExtInfo = 0;
    for (int i = 0; i < entries.size(); ++i) {
        Ps2RomdirEntryInfo& entry = entries[i];
        entry.dataOffset = dataOffset;
        if (entry.fileSize > quint64(store->capacity()) - dataOffset)
            return fail(QStringLiteral("ROMDIR file exceeds image bounds: %1").arg(entry.name));
        if (std::numeric_limits<quint64>::max() - dataOffset < align16(entry.fileSize))
            return fail(QStringLiteral("ROMDIR offset overflow"));
        dataOffset += align16(entry.fileSize);
        if (std::numeric_limits<quint64>::max() - totalExtInfo < entry.extInfoSize)
            return fail(QStringLiteral("EXTINFO size overflow"));
        totalExtInfo += entry.extInfoSize;
    }

    const quint64 expectedExtinfoOffset = baseOffset + align16(romdirSize);
    if (entries[2].dataOffset != expectedExtinfoOffset ||
        expectedExtinfoOffset > quint64(store->capacity()) ||
        extinfoSize > quint64(store->capacity()) - expectedExtinfoOffset)
        return fail(QStringLiteral("Invalid EXTINFO placement"));
    if (totalExtInfo > extinfoSize)
        return fail(QStringLiteral("EXTINFO records exceed section size"));

    extOffset = expectedExtinfoOffset;
    for (Ps2RomdirEntryInfo& entry : entries) {
        entry.extInfoOffset = extOffset;
        entry.extInfoSummary = extInfoSummary(store, extOffset, entry.extInfoSize);
        extOffset += entry.extInfoSize;
    }

    image->baseOffset = baseOffset;
    image->romdirSize = romdirSize;
    image->extinfoSize = extinfoSize;
    image->entries = std::move(entries);
    if (error) error->clear();
    return true;
}

qint64 findPs2Romdir(const fs::ByteStorePtr& store, quint64 scanLimit)
{
    if (!store || store->capacity() < 64) return -1;
    const quint64 length = std::min<quint64>(quint64(store->capacity()), scanLimit);
    const std::vector<std::uint8_t> probe = store->readRange(0, qint64(length));
    const QByteArray bytes(reinterpret_cast<const char*>(probe.data()), int(probe.size()));
    const QByteArray signature("RESET\0\0\0\0\0", 10);
    int pos = -1;
    while ((pos = bytes.indexOf(signature, pos + 1)) >= 0) {
        if ((pos & 15) != 0) continue;
        Ps2RomdirImageInfo image;
        if (parsePs2Romdir(store, quint64(pos), &image)) return pos;
    }
    return -1;
}

qint64 findPs2Romdir(const QByteArray& data)
{
    if (data.isEmpty()) return -1;
    const auto store = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
    return findPs2Romdir(store, quint64(data.size()));
}

} // namespace peare
