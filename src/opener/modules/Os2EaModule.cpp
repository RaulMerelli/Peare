#include "Os2EaModule.h"
#include "Compat.h"

#include <QFile>
#include <QStringList>
#include <utility>

namespace peare {
namespace {

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
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString eaTypeName(quint16 type)
{
    switch (type) {
    case 0xFFF9: return QStringLiteral("icon");
    case 0xFFFA: return QStringLiteral("metafile");
    case 0xFFFB: return QStringLiteral("bitmap");
    case 0xFFFD: return QStringLiteral("ASCII");
    case 0xFFFE: return QStringLiteral("binary");
    case 0xFFDF: return QStringLiteral("multi-value multi-type");
    case 0xFFDE: return QStringLiteral("multi-value single-type");
    default: return QStringLiteral("type 0x%1").arg(type, 4, 16, QLatin1Char('0')).toUpper();
    }
}

bool unwrapSingleValue(const QByteArray& value, QByteArray* payload, quint16* type)
{
    bool ok = false;
    const quint16 valueType = le16(value, 0, &ok);
    if (!ok) return false;
    const quint16 length = le16(value, 2, &ok);
    if (!ok || qsizetype(length) > value.size() - 4) return false;
    *type = valueType;
    *payload = value.mid(4, length);
    return true;
}

bool unwrapMultiType(const QByteArray& value, QByteArray* payload)
{
    if (value.size() < 6 || le16(value, 0) != 0xFFDF) return false;
    const quint16 count = le16(value, 4);
    qsizetype pos = 6;
    QByteArray text;
    for (quint16 i = 0; i < count; ++i) {
        if (pos + 4 > value.size()) return false;
        const quint16 type = le16(value, pos);
        const quint16 length = le16(value, pos + 2);
        pos += 4;
        if (pos + length > value.size()) return false;
        if (type == 0xFFFD) {
            QByteArray item = value.mid(pos, length);
            while (item.endsWith('\0')) item.chop(1);
            if (!text.isEmpty()) text.append('\n');
            text.append(item);
        } else {
            return false;
        }
        pos += length;
    }
    *payload = text;
    return true;
}

} // namespace

std::unique_ptr<Os2EaModule> Os2EaModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<Os2EaModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::OS2_EA;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<Os2EaModule> Os2EaModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<Os2EaModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::OS2_EA;
    module->info_.description = QStringLiteral("OS/2 extended attribute list (FEALIST)");

    bool ok = false;
    const quint32 declaredSize = le32(data, 0, &ok);
    if (!ok || declaredSize != quint32(data.size()) || declaredSize < 8) {
        module->info_.error = QStringLiteral("Invalid OS/2 FEALIST size");
        return module;
    }

    qsizetype pos = 4;
    int index = 0;
    while (pos < data.size()) {
        if (pos + 4 > data.size()) {
            module->info_.error = QStringLiteral("Truncated OS/2 FEA entry");
            module->resources_.clear();
            return module;
        }
        const quint8 flags = quint8(data.at(pos));
        const quint8 nameLength = quint8(data.at(pos + 1));
        const quint16 valueLength = le16(data, pos + 2);
        const qsizetype nameOffset = pos + 4;
        const qsizetype valueOffset = nameOffset + nameLength + 1;
        if (nameLength == 0 || valueOffset > data.size() ||
            data.at(nameOffset + nameLength) != '\0' ||
            qsizetype(valueLength) > data.size() - valueOffset) {
            module->info_.error = QStringLiteral("Invalid OS/2 FEA entry at offset 0x%1")
                .arg(quint64(pos), 0, 16);
            module->resources_.clear();
            return module;
        }

        QString name = QString::fromLatin1(data.constData() + nameOffset, nameLength);
        if (name.isEmpty()) name = QStringLiteral("EA_%1").arg(index + 1);
        const QByteArray rawValue = data.mid(valueOffset, valueLength);
        QByteArray payload = rawValue;
        quint16 valueType = 0;
        QString type = QStringLiteral("OS2_EA_VALUE");

        if (unwrapMultiType(rawValue, &payload)) {
            valueType = 0xFFDF;
            type = QStringLiteral("OS2_EA_TEXT");
        } else if (unwrapSingleValue(rawValue, &payload, &valueType)) {
            if (valueType == 0xFFF9 || valueType == 0xFFFA || valueType == 0xFFFB)
                type = QStringLiteral("RT_BITMAP");
            else if (valueType == 0xFFFD) {
                while (payload.endsWith('\0')) payload.chop(1);
                type = QStringLiteral("OS2_EA_TEXT");
            }
        }

        ResourceEntry entry;
        entry.type = type;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(valueOffset);
        entry.dataSize = quint64(payload.size());
        entry.codePage = 850;
        entry.format = ModuleFormat::OS2_EA;
        entry.isOs2 = true;
        entry.baseId = valueType;
        entry.hierarchyPath = QStringList() << QStringLiteral("Extended attributes")
                                             << eaTypeName(valueType);
        entry.data = std::move(payload);
        module->resources_.push_back(std::move(entry));

        pos = valueOffset + valueLength;
        ++index;
        Q_UNUSED(flags);
    }

    if (module->resources_.isEmpty())
        module->info_.error = QStringLiteral("OS/2 FEALIST contains no attributes");
    return module;
}

} // namespace peare
