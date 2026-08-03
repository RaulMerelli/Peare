#include "Os2PackModule.h"
#include "Os2Pack2Decoder.h"
#include "Compat.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSet>
#include <QTextCodec>
#include <array>
#include <utility>

namespace peare {
namespace {

const int kTableSize = 4096;
const int kFirstDynamic = 257;
const qsizetype kMaxDecodedMember = qsizetype(512) * 1024 * 1024;

struct PackMember {
    qsizetype headerOffset = 0;
    quint16 formatCode = 0;
    QString fileName;
    qsizetype compressedOffset = 0;
    qsizetype compressedEnd = 0;
    quint32 originalSize = 0;
    bool hasOriginalSize = false;
    quint32 nextMemberOffset = 0;
    bool hasNextMember = false;
};

quint16 le16(const QByteArray& data, qsizetype offset, bool* ok = nullptr)
{
    const bool valid = offset >= 0 && offset + 2 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& data, qsizetype offset, bool* ok = nullptr)
{
    const bool valid = offset >= 0 && offset + 4 <= data.size();
    if (ok) *ok = valid;
    if (!valid) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool signatureFormat(const QByteArray& data, qsizetype offset, quint16* format)
{
    if (offset < 0 || offset + 4 > data.size()) return false;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    if (p[0] != 0xA5 || p[1] != 0x96) return false;
    const quint16 code = quint16(p[2]) | (quint16(p[3]) << 8);
    switch (code) {
    case 0x000A: *format = 0x0A00; return true;
    case 0x0A0A: *format = 0x0A0A; return true;
    case 0x1400: *format = 0x1400; return true;
    case 0x0A14: *format = 0x0A14; return true;
    case 0xFFFF: *format = 0xFFFF; return true;
    case 0xFFFE: *format = 0xFFFE; return true;
    case 0xFFFD: *format = 0xFFFD; return true;
    default: return false;
    }
}

bool validPaddedField(const QByteArray& raw, bool allowEmpty)
{
    bool seenSpace = false;
    int nonspace = 0;
    const QByteArray forbidden("\"./\\[]:;=,+*?<>|");
    for (char c : raw) {
        const uchar value = uchar(c);
        if (value == 0x20) {
            seenSpace = true;
            continue;
        }
        if (seenSpace || value < 0x21 || forbidden.contains(char(value))) return false;
        ++nonspace;
    }
    return allowEmpty || nonspace > 0;
}

bool looksCompact83(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 20 > data.size()) return false;
    const QByteArray raw = data.mid(offset + 8, 11);
    return validPaddedField(raw.left(8), false) && validPaddedField(raw.mid(8), true);
}

QString decodeName(QByteArray raw)
{
    const int nul = raw.indexOf('\0');
    if (nul >= 0) raw.truncate(nul);
    QTextCodec* codec = QTextCodec::codecForName("IBM 850");
    return codec ? codec->toUnicode(raw) : QString::fromLatin1(raw);
}

QString decodeCompact83(const QByteArray& raw)
{
    QByteArray base = raw.left(8);
    QByteArray ext = raw.mid(8, 3);
    while (base.endsWith(' ')) base.chop(1);
    while (ext.endsWith(' ')) ext.chop(1);
    const QByteArray joined = base + (ext.isEmpty() ? QByteArray() : QByteArray(".") + ext);
    QTextCodec* codec = QTextCodec::codecForName("IBM 850");
    return codec ? codec->toUnicode(joined) : QString::fromLatin1(joined);
}

QString safeLeafName(QString name, int index)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1);
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < name.size(); ++i) {
        if (name.at(i).unicode() < 0x20 || forbidden.contains(name.at(i))) name[i] = QLatin1Char('_');
    }
    while (name.endsWith(QLatin1Char(' ')) || name.endsWith(QLatin1Char('.'))) name.chop(1);
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral(".."))
        return QStringLiteral("member_%1.bin").arg(index, 3, 10, QLatin1Char('0'));
    return name;
}

bool parseMember(const QByteArray& data, qsizetype offset, PackMember* member, QString* error)
{
    quint16 format = 0;
    if (!signatureFormat(data, offset, &format)) {
        *error = QStringLiteral("Unknown OS/2 PACK signature at offset 0x%1").arg(quint64(offset), 0, 16);
        return false;
    }
    if (offset + 10 > data.size()) {
        *error = QStringLiteral("Truncated OS/2 PACK header");
        return false;
    }

    qsizetype pos = offset + 8;
    quint32 eaOffset = 0;
    const bool compact = format == 0x0A00 || (format == 0x0A0A && looksCompact83(data, offset));
    if (compact) {
        if (offset + 20 > data.size()) {
            *error = QStringLiteral("Truncated compact OS/2 PACK header");
            return false;
        }
        member->fileName = decodeCompact83(data.mid(offset + 8, 11));
        pos = offset + 20;
    } else {
        ++pos; // attributes
        int fileNameLength = 0;
        if (format == 0x1400 || format == 0x0A14 || format == 0x0A0A) {
            ++pos; // legacy reserved byte
            fileNameLength = 13;
        } else if (format == 0xFFFF) {
            pos += 6;
            const int nul = data.indexOf('\0', pos);
            if (nul < 0 || nul - pos > 259) {
                *error = QStringLiteral("OS/2 PACK filename is not NUL-terminated");
                return false;
            }
            fileNameLength = nul + 1 - pos;
        } else {
            pos += 3;
            bool ok = false;
            eaOffset = le32(data, pos, &ok); if (!ok) { *error = QStringLiteral("Truncated extended OS/2 PACK header"); return false; } pos += 4;
            const quint32 original = le32(data, pos, &ok); if (!ok) { *error = QStringLiteral("Truncated extended OS/2 PACK header"); return false; } pos += 4;
            const quint32 next = le32(data, pos, &ok); if (!ok) { *error = QStringLiteral("Truncated extended OS/2 PACK header"); return false; } pos += 4;
            if (format == 0xFFFD) {
                if (pos + 15 > data.size() || data.mid(pos, 7) != QByteArray("FTCOMP\0", 7)) {
                    *error = QStringLiteral("Invalid OS/2 PACK2 FTCOMP header");
                    return false;
                }
                pos += 15; // FTCOMP\0, two 16-bit fields, and one 32-bit field.
            }
            fileNameLength = int(le16(data, pos, &ok)); if (!ok) { *error = QStringLiteral("Truncated extended OS/2 PACK header"); return false; } pos += 2;
            if (original != 0 && original != 1) { member->originalSize = original; member->hasOriginalSize = true; }
            if (next != 0) { member->nextMemberOffset = next; member->hasNextMember = true; }
        }
        if (fileNameLength < 1 || fileNameLength > 512 || pos + fileNameLength > data.size()) {
            *error = QStringLiteral("Invalid OS/2 PACK filename field");
            return false;
        }
        member->fileName = decodeName(data.mid(pos, fileNameLength));
        pos += fileNameLength;
    }

    const qsizetype memberEnd = member->hasNextMember ? qsizetype(member->nextMemberOffset) : data.size();
    if (memberEnd <= offset || memberEnd > data.size()) {
        *error = QStringLiteral("Invalid OS/2 PACK next-member offset");
        return false;
    }
    qsizetype compressedEnd = memberEnd;
    if (eaOffset != 0) {
        if (eaOffset < quint32(pos) || eaOffset > quint32(memberEnd)) {
            *error = QStringLiteral("Invalid OS/2 PACK extended-attribute offset");
            return false;
        }
        compressedEnd = qsizetype(eaOffset);
    }
    if (compressedEnd < pos) {
        *error = QStringLiteral("Invalid OS/2 PACK compressed-data length");
        return false;
    }

    member->headerOffset = offset;
    member->formatCode = format;
    member->compressedOffset = pos;
    member->compressedEnd = compressedEnd;
    return true;
}

bool decompress(const QByteArray& data, qsizetype start, qsizetype end, quint32 expectedSize,
                bool hasExpectedSize, QByteArray* decoded, QString* error)
{
    std::array<quint16, kTableSize> parent{};
    std::array<quint16, kTableSize> useCount{};
    std::array<quint16, kTableSize> size{};
    std::array<uchar, kTableSize> firstValue{};
    std::array<uchar, kTableSize> value{};
    std::array<quint16, kTableSize> older{};
    std::array<quint16, kTableSize> newer{};

    for (int code = 1; code <= 256; ++code) {
        const uchar byteValue = uchar(code - 1);
        firstValue[code] = byteValue;
        value[code] = byteValue;
        size[code] = 1;
        useCount[code] = 1;
    }
    for (int code = kFirstDynamic; code < kTableSize; ++code) {
        if (code < kTableSize - 1) newer[code] = quint16(code + 1);
        if (code > kFirstDynamic) older[code] = quint16(code - 1);
    }

    int oldest = kFirstDynamic;
    int newest = kTableSize - 1;
    int oldCode = 0;
    bool sawStop = false;
    qsizetype pos = start;
    bool firstHalf = true;
    uchar hold = 0;
    decoded->clear();

    auto unlink = [&](int code) {
        const int next = newer[code];
        const int previous = older[code];
        if (code == newest) newest = previous; else older[next] = quint16(previous);
        if (code == oldest) oldest = next; else newer[previous] = quint16(next);
        older[code] = newer[code] = 0;
    };
    auto addMru = [&](int code) {
        newer[newest] = quint16(code);
        older[code] = quint16(newest);
        newer[code] = 0;
        newest = code;
    };

    while (pos < end) {
        int code = 0;
        if (firstHalf) {
            if (pos + 2 > end) { *error = QStringLiteral("Truncated 12-bit OS/2 PACK code"); return false; }
            code = (uchar(data.at(pos)) << 4) | (uchar(data.at(pos + 1)) >> 4);
            hold = uchar(data.at(pos + 1));
            pos += 2;
        } else {
            if (pos >= end) { *error = QStringLiteral("Truncated 12-bit OS/2 PACK code"); return false; }
            code = ((hold & 0x0F) << 8) | uchar(data.at(pos));
            ++pos;
        }
        firstHalf = !firstHalf;
        if (code == 0) { sawStop = true; break; }

        if (oldCode != 0) {
            const int lru = oldest;
            if (lru < kFirstDynamic || lru >= kTableSize || useCount[lru] != 0) {
                *error = QStringLiteral("Corrupt OS/2 PACK LZW replacement list"); return false;
            }
            const int oldParent = parent[lru];
            unlink(lru);
            if (oldParent != 0) {
                if (useCount[oldParent] == 0) { *error = QStringLiteral("Corrupt OS/2 PACK LZW reference count"); return false; }
                --useCount[oldParent];
                if (useCount[oldParent] == 0) addMru(oldParent);
            }
            if (oldCode < 1 || oldCode >= kTableSize || size[oldCode] == 0) { *error = QStringLiteral("Invalid previous OS/2 PACK LZW code"); return false; }
            const int source = code != lru ? code : oldCode;
            if (source < 1 || source >= kTableSize || size[source] == 0) { *error = QStringLiteral("Invalid OS/2 PACK LZW code"); return false; }
            const int newSize = size[oldCode] + 1;
            if (newSize > kTableSize - 256) { *error = QStringLiteral("OS/2 PACK LZW string exceeds dictionary limit"); return false; }
            firstValue[lru] = firstValue[oldCode];
            value[lru] = firstValue[source];
            size[lru] = quint16(newSize);
            parent[lru] = quint16(oldCode);
            if (useCount[oldCode] > 0) ++useCount[oldCode];
            else { unlink(oldCode); useCount[oldCode] = 1; }
            useCount[lru] = 0;
            addMru(lru);
        }

        if (code < 1 || code >= kTableSize || size[code] == 0) { *error = QStringLiteral("Invalid OS/2 PACK LZW group code"); return false; }
        const int length = size[code];
        if (decoded->size() > kMaxDecodedMember - length) { *error = QStringLiteral("OS/2 PACK member exceeds 512 MiB safety limit"); return false; }
        QByteArray stack(length, '\0');
        int stackPos = length;
        int current = code;
        while (stackPos > 0) {
            if (current < 1 || current >= kTableSize || size[current] != stackPos) { *error = QStringLiteral("Broken OS/2 PACK LZW parent chain"); return false; }
            stack[--stackPos] = char(value[current]);
            current = parent[current];
        }
        decoded->append(stack);
        oldCode = code;
    }

    if (!sawStop) { *error = QStringLiteral("OS/2 PACK stream has no stop code"); return false; }
    if (hasExpectedSize && quint32(decoded->size()) != expectedSize) {
        *error = QStringLiteral("OS/2 PACK size mismatch: expected %1, decoded %2").arg(expectedSize).arg(decoded->size());
        return false;
    }
    return true;
}

} // namespace

std::unique_ptr<Os2PackModule> Os2PackModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<Os2PackModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::OS2_PACK;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<Os2PackModule> Os2PackModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<Os2PackModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::OS2_PACK;
    module->info_.description = QStringLiteral("IBM/Microsoft OS/2 PACK/PACK2 archive");

    qsizetype offset = 0;
    int index = 0;
    QSet<qsizetype> seen;
    while (true) {
        if (seen.contains(offset)) { module->info_.error = QStringLiteral("Cyclic OS/2 PACK member chain"); module->resources_.clear(); return module; }
        seen.insert(offset);
        PackMember member;
        QString error;
        if (!parseMember(data, offset, &member, &error)) { module->info_.error = error; module->resources_.clear(); return module; }
        QByteArray decoded;
        bool decodedOk = false;
        if (member.formatCode == 0xFFFD) {
            if (!member.hasOriginalSize) {
                error = QStringLiteral("PACK2 member has no original size");
            } else {
                decodedOk = decompressOs2Pack2(data, member.compressedOffset, member.compressedEnd,
                                               member.originalSize, &decoded, &error);
            }
        } else {
            decodedOk = decompress(data, member.compressedOffset, member.compressedEnd,
                                   member.originalSize, member.hasOriginalSize, &decoded, &error);
        }
        if (!decodedOk) {
            module->info_.error = QStringLiteral("%1: %2").arg(member.fileName, error); module->resources_.clear(); return module;
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("OS2_PACK_FILE");
        entry.isEmbeddedFile = true;
        entry.name = safeLeafName(member.fileName, index);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(member.compressedOffset);
        entry.dataSize = quint64(decoded.size());
        entry.format = ModuleFormat::OS2_PACK;
        entry.isOs2 = true;
        entry.hierarchyPath = QStringList() << entry.name;
        entry.data = decoded;
        module->resources_.push_back(std::move(entry));

        ++index;
        if (!member.hasNextMember) break;
        offset = qsizetype(member.nextMemberOffset);
    }
    return module;
}

} // namespace peare
