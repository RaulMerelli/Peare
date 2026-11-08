#include "AndroidBinaryXml.h"

#include <QHash>
#include <QStringList>

namespace peare { namespace resources {
namespace {

const quint16 kXml = 0x0003;
const quint16 kStringPool = 0x0001;
const quint16 kResourceMap = 0x0180;
const quint16 kStartNamespace = 0x0100;
const quint16 kEndNamespace = 0x0101;
const quint16 kStartElement = 0x0102;
const quint16 kEndElement = 0x0103;
const quint16 kCdata = 0x0104;
const quint32 kNoIndex = 0xffffffffu;

bool range(const QByteArray& data, quint64 offset, quint64 size)
{
    return offset <= quint64(data.size()) && size <= quint64(data.size()) - offset;
}

quint16 u16(const QByteArray& data, quint64 offset)
{
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 u32(const QByteArray& data, quint64 offset)
{
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool length8(const QByteArray& data, quint64* offset, quint32* length)
{
    if (!range(data, *offset, 1)) return false;
    quint8 first = quint8(data.at(int((*offset)++)));
    if (first & 0x80) {
        if (!range(data, *offset, 1)) return false;
        first = quint8(((first & 0x7f) << 8) |
                       quint8(data.at(int((*offset)++))));
    }
    *length = first;
    return true;
}

bool length16(const QByteArray& data, quint64* offset, quint32* length)
{
    if (!range(data, *offset, 2)) return false;
    quint16 first = u16(data, *offset);
    *offset += 2;
    if (first & 0x8000) {
        if (!range(data, *offset, 2)) return false;
        first = quint16(((first & 0x7fff) << 16) | u16(data, *offset));
        *offset += 2;
    }
    *length = first;
    return true;
}

QString escapeXml(QString value, bool attribute)
{
    value.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    value.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    value.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    if (attribute) value.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return value;
}

class Parser {
public:
    explicit Parser(const QByteArray& data) : data_(data) {}

    QString parse()
    {
        if (data_.size() < 8 || u16(data_, 0) != kXml || u32(data_, 4) != quint32(data_.size()))
            return QString();
        const quint16 headerSize = u16(data_, 2);
        if (headerSize < 8 || headerSize > data_.size()) return QString();
        QStringList output;
        output << QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>");
        quint64 offset = headerSize;
        while (offset < quint64(data_.size())) {
            if (!range(data_, offset, 8)) return QString();
            const quint16 type = u16(data_, offset);
            const quint16 chunkHeader = u16(data_, offset + 2);
            const quint32 chunkSize = u32(data_, offset + 4);
            if (chunkHeader < 8 || chunkSize < chunkHeader ||
                !range(data_, offset, chunkSize)) return QString();
            bool ok = true;
            if (type == kStringPool) ok = parseStringPool(offset, chunkHeader, chunkSize);
            else if (type == kStartNamespace) ok = parseNamespace(offset, chunkHeader, chunkSize);
            else if (type == kStartElement) ok = parseStartElement(offset, chunkHeader, chunkSize, &output);
            else if (type == kEndElement) ok = parseEndElement(offset, chunkHeader, chunkSize, &output);
            else if (type == kCdata) ok = parseCdata(offset, chunkHeader, chunkSize, &output);
            else if (type != kResourceMap && type != kEndNamespace) ok = false;
            if (!ok) return QString();
            offset += chunkSize;
        }
        if (depth_ != 0 || !hasRoot_) return QString();
        return output.join(QLatin1Char('\n'));
    }

private:
    QString stringAt(quint32 index) const
    {
        return index < quint32(strings_.size()) ? strings_.at(int(index)) : QString();
    }

    bool parseStringPool(quint64 offset, quint16 header, quint32 size)
    {
        if (header < 28 || size < header) return false;
        const quint32 count = u32(data_, offset + 8);
        const quint32 styles = u32(data_, offset + 12);
        const quint32 flags = u32(data_, offset + 16);
        const quint32 stringsStart = u32(data_, offset + 20);
        const quint32 stylesStart = u32(data_, offset + 24);
        const quint64 arrays = quint64(header) + (quint64(count) + styles) * 4;
        if (count > 1000000 || arrays > size || stringsStart < arrays || stringsStart > size ||
            (styles && (stylesStart < stringsStart || stylesStart > size))) return false;
        strings_.clear();
        strings_.reserve(int(count));
        const quint64 stringLimit = offset + (styles ? stylesStart : size);
        for (quint32 i = 0; i < count; ++i) {
            const quint32 relative = u32(data_, offset + header + quint64(i) * 4);
            quint64 cursor = offset + stringsStart + relative;
            if (cursor >= stringLimit) return false;
            QString value;
            if (flags & 0x100) {
                quint32 chars = 0, bytes = 0;
                if (!length8(data_, &cursor, &chars) || !length8(data_, &cursor, &bytes) ||
                    cursor > stringLimit || quint64(bytes) + 1 > stringLimit - cursor ||
                    data_.at(int(cursor + bytes)) != '\0')
                    return false;
                value = QString::fromUtf8(data_.constData() + cursor, int(bytes));
                if (chars > quint32(data_.size())) return false;
            } else {
                quint32 chars = 0;
                if (!length16(data_, &cursor, &chars) ||
                    quint64(chars) > (quint64(size) - stringsStart) / 2 ||
                    cursor > stringLimit || quint64(chars) * 2 + 2 > stringLimit - cursor ||
                    u16(data_, cursor + quint64(chars) * 2) != 0) return false;
                value = QString::fromUtf16(reinterpret_cast<const ushort*>(data_.constData() + cursor),
                                           int(chars));
            }
            strings_.append(value);
        }
        return true;
    }

    bool parseNamespace(quint64 offset, quint16 header, quint32 size)
    {
        if (header < 16 || size < 24) return false;
        const QString prefix = stringAt(u32(data_, offset + 16));
        const QString uri = stringAt(u32(data_, offset + 20));
        if (!prefix.isEmpty() && !uri.isEmpty()) namespaces_.insert(uri, prefix);
        return true;
    }

    QString qualified(quint32 ns, quint32 name) const
    {
        const QString local = stringAt(name);
        const QString uri = stringAt(ns);
        const QString prefix = namespaces_.value(uri);
        return prefix.isEmpty() ? local : prefix + QLatin1Char(':') + local;
    }

    QString valueFor(quint32 raw, quint8 type, quint32 value) const
    {
        if (raw != kNoIndex) return stringAt(raw);
        if (type == 0x03) return stringAt(value);
        if (type == 0x10) return QString::number(value);
        if (type == 0x11) return QStringLiteral("0x%1").arg(value, 0, 16);
        if (type == 0x12) return value ? QStringLiteral("true") : QStringLiteral("false");
        if (type == 0x01) return QStringLiteral("@0x%1").arg(value, 8, 16, QLatin1Char('0'));
        if (type == 0x02) return QStringLiteral("?0x%1").arg(value, 8, 16, QLatin1Char('0'));
        return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    }

    bool parseStartElement(quint64 offset, quint16 header, quint32 size, QStringList* output)
    {
        if (header < 16 || size < 36 || strings_.isEmpty()) return false;
        const quint32 ns = u32(data_, offset + 16);
        const quint32 name = u32(data_, offset + 20);
        const quint16 attrStart = u16(data_, offset + 24);
        const quint16 attrSize = u16(data_, offset + 26);
        const quint16 attrCount = u16(data_, offset + 28);
        if (attrSize < 20 || attrCount > 4096) return false;
        const quint64 attrBase = offset + 16 + attrStart;
        if (attrBase < offset + header || quint64(attrCount) * attrSize > offset + size - attrBase)
            return false;
        QString tag = qualified(ns, name);
        if (tag.isEmpty()) return false;
        if (depth_ == 0) {
            hasRoot_ = true;
            for (QHash<QString, QString>::const_iterator it = namespaces_.constBegin();
                 it != namespaces_.constEnd(); ++it)
                output->append(QStringLiteral("<%1 xmlns:%2=\"%3\"")
                    .arg(tag, it.value(), escapeXml(it.key(), true)));
        } else {
            output->append(QString(depth_ * 2, QLatin1Char(' ')) + QLatin1Char('<') + tag);
        }
        for (quint16 i = 0; i < attrCount; ++i) {
            const quint64 attr = attrBase + quint64(i) * attrSize;
            const quint32 ans = u32(data_, attr);
            const quint32 aname = u32(data_, attr + 4);
            const quint32 raw = u32(data_, attr + 8);
            if (u16(data_, attr + 12) < 8 || !range(data_, attr + 12, 8)) return false;
            const quint8 valueType = quint8(data_.at(int(attr + 15)));
            const quint32 value = u32(data_, attr + 16);
            const QString key = qualified(ans, aname);
            if (key.isEmpty()) return false;
            output->last().append(QStringLiteral("\n") + QString((depth_ + 1) * 2, QLatin1Char(' ')) +
                key + QStringLiteral("=\"") + escapeXml(valueFor(raw, valueType, value), true) + QLatin1Char('"'));
        }
        output->last().append(QLatin1Char('>'));
        ++depth_;
        return true;
    }

    bool parseEndElement(quint64 offset, quint16 header, quint32 size, QStringList* output)
    {
        if (header < 16 || size < 24 || depth_ <= 0) return false;
        const QString tag = qualified(u32(data_, offset + 16), u32(data_, offset + 20));
        if (tag.isEmpty()) return false;
        --depth_;
        output->append(QString(depth_ * 2, QLatin1Char(' ')) + QStringLiteral("</%1>").arg(tag));
        return true;
    }

    bool parseCdata(quint64 offset, quint16 header, quint32 size, QStringList* output)
    {
        if (header < 16 || size < 28 || depth_ <= 0) return false;
        const QString text = stringAt(u32(data_, offset + 16));
        output->append(QString(depth_ * 2, QLatin1Char(' ')) + escapeXml(text, false));
        return true;
    }

    const QByteArray& data_;
    QStringList strings_;
    QHash<QString, QString> namespaces_;
    int depth_ = 0;
    bool hasRoot_ = false;
};

} // namespace

bool recognizesAndroidBinaryXml(const QByteArray& data) noexcept
{
    return data.size() >= 8 && u16(data, 0) == kXml && u16(data, 2) >= 8 &&
           u32(data, 4) == quint32(data.size());
}

ResourcePreview decodeAndroidBinaryXml(const ResourceEntry& entry) noexcept
{
    ResourcePreview result;
    try {
        Parser parser(entry.data);
        result.text = parser.parse();
        if (result.text.isEmpty()) result.error = QStringLiteral("Invalid Android binary XML");
    } catch (...) {
        result.error = QStringLiteral("Cannot decode Android binary XML");
    }
    return result;
}

} }
