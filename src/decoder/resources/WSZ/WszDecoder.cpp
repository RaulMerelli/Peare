#include "WszDecoder.h"

#include <QDebug>
#include <QHash>
#include <QStringList>
#include <QtGlobal>

#include <limits>
#include <stdexcept>

namespace peare {
namespace resources {
namespace {

constexpr quint8 ElementNamed = 0x00;
constexpr quint8 ElementButton = 0x03;
constexpr quint8 ElementTheme = 0x0B;
constexpr quint8 ElementView = 0x0C;
constexpr quint8 ElementSubview = 0x0D;
constexpr quint8 ElementClsidMask = 0x80;

quint16 readUInt16At(const QByteArray& data, int offset)
{
    return quint16(quint8(data[offset])) |
           (quint16(quint8(data[offset + 1])) << 8);
}

class WszFormatException final : public std::runtime_error
{
public:
    explicit WszFormatException(const QString& message)
        : std::runtime_error(message.toStdString())
    {
    }
};

class Parser final
{
public:
    explicit Parser(const QByteArray& bytes)
        : data(bytes)
    {
    }

    QString Decode()
    {
        output.append(QStringLiteral("Windows Media Player WSZ skin\n"));
        ParseElement(0, data.size(), true);

        if (position < data.size())
            SkipZeroPadding(data.size(), QStringLiteral("trailing data"));

        if (position != data.size())
            Fail(QStringLiteral("The parser did not consume the complete resource."));

        while (!output.isEmpty() && output.back().isSpace())
            output.chop(1);
        return output;
    }

private:
    static constexpr int MaximumDepth = 256;
    static constexpr int MaximumRecords = 1000000;

    const QByteArray& data;
    QString output;
    int position = 0;
    int recordsParsed = 0;

    static QString KnownObjectName(const QString& clsid)
    {
        static const QHash<QString, QString> knownObjects = {
            {QStringLiteral("{8856F961-340A-11D0-A96B-00C04FD705A2}"), QStringLiteral("browser")},
            {QStringLiteral("{AA1AC37B-49A8-4B41-AF69-B0176C5FFC33}"), QStringLiteral("plugin")},
            {QStringLiteral("{A8A55FAC-82EA-4BD7-BD7B-11586A4D99E4}"), QStringLiteral("Skinlist")},
            {QStringLiteral("{395BF287-6477-495F-8427-2C09A23C3248}"), QStringLiteral("taskcenter")}
        };
        return knownObjects.value(clsid);
    }

    void ParseElement(int level, int parentEnd, bool isLastSibling)
    {
        if (level > MaximumDepth)
            Fail(QStringLiteral("The element nesting depth is excessive."));
        CountRecord();

        const int start = position;
        const quint16 nextSiblingOffset = ReadUInt16(parentEnd);
        const quint16 firstChildOffset = ReadUInt16(parentEnd);
        const quint8 elementType = ReadByte(parentEnd);
        const quint8 childCount = ReadByte(parentEnd);
        const quint8 attributeCount = ReadByte(parentEnd);

        if (level == 0)
        {
            if (elementType != ElementTheme)
                FailAt(start, QStringLiteral("The root element is not THEME."));
            if (nextSiblingOffset != 0)
                FailAt(start, QStringLiteral("The root element unexpectedly has a sibling offset."));
        }
        else if (elementType == ElementTheme)
        {
            FailAt(start, QStringLiteral("A nested THEME element is not valid."));
        }

        if (isLastSibling && nextSiblingOffset != 0)
            FailAt(start, QStringLiteral("The final sibling has a non-zero next offset."));
        if (!isLastSibling && nextSiblingOffset == 0)
            FailAt(start, QStringLiteral("A non-final sibling has a zero next offset."));

        const int elementEnd = nextSiblingOffset == 0
            ? parentEnd
            : CheckedOffset(start, nextSiblingOffset, QStringLiteral("next element"));

        if (elementEnd <= start || elementEnd > parentEnd)
            FailAt(start, QStringLiteral("The next-element offset is outside its parent."));

        int firstChildPosition = -1;
        if (firstChildOffset != 0)
            firstChildPosition = CheckedOffset(start, firstChildOffset, QStringLiteral("first child"));

        if (childCount == 0 && firstChildOffset != 0)
            FailAt(start, QStringLiteral("An element without children has a first-child offset."));
        if (childCount != 0 && firstChildOffset == 0)
            FailAt(start, QStringLiteral("An element with children has no first-child offset."));

        const int headerEnd = firstChildOffset == 0 ? elementEnd : firstChildPosition;
        if (headerEnd < position || headerEnd > elementEnd)
            FailAt(start, QStringLiteral("The first-child offset overlaps the element header or exceeds the element."));

        QString elementName;
        QString elementDetails;

        switch (elementType)
        {
        case ElementNamed:
        case ElementButton:
        {
            elementName = ReadWideString(headerEnd);
            const bool isButtonName = elementName.compare(QStringLiteral("buttonelement"), Qt::CaseInsensitive) == 0;
            if (elementType == ElementNamed && isButtonName)
                FailAt(start, QStringLiteral("BUTTONELEMENT uses the wrong element ID."));
            if (elementType == ElementButton && !isButtonName)
                FailAt(start, QStringLiteral("Element ID 0x03 is reserved for BUTTONELEMENT."));
            break;
        }
        case ElementTheme:
            ReadPaddingWord(headerEnd);
            elementName = QStringLiteral("theme");
            break;
        case ElementView:
            ReadPaddingWord(headerEnd);
            elementName = QStringLiteral("view");
            break;
        case ElementSubview:
            ReadPaddingWord(headerEnd);
            elementName = QStringLiteral("subview");
            break;
        default:
            if ((elementType & ElementClsidMask) == 0)
                FailAt(start, QStringLiteral("Unrecognized element ID 0x%1.").arg(QString::number(elementType, 16).rightJustified(2, QChar(u'0')).toUpper()));

            ReadPaddingWord(headerEnd);
            elementDetails = ReadGuid(headerEnd);
            elementName = KnownObjectName(elementDetails);
            if (elementName.isEmpty())
                elementName = QStringLiteral("object");
            break;
        }

        AppendIndent(level);
        output.append(elementName);
        output.append(QStringLiteral(" @0x%1 [id=0x%2, children=%3, attributes=%4]")
                          .arg(QString::number(start, 16).rightJustified(8, QChar(u'0')).toUpper())
                          .arg(QString::number(elementType, 16).rightJustified(2, QChar(u'0')).toUpper())
                          .arg(childCount)
                          .arg(attributeCount));
        if (!elementDetails.isEmpty())
        {
            output.append(QChar(u' '));
            output.append(elementDetails);
        }
        output.append(QChar(u'\n'));

        for (int i = 0; i < attributeCount; ++i)
            ParseAttribute(level + 1, headerEnd, i == attributeCount - 1);

        if (position != headerEnd)
            FailAt(position, QStringLiteral("The element header does not end at its first-child/next-element offset."));

        for (int i = 0; i < childCount; ++i)
            ParseElement(level + 1, elementEnd, i == childCount - 1);

        if (position < elementEnd)
            SkipZeroPadding(elementEnd, QStringLiteral("element padding"));
        if (position != elementEnd)
            FailAt(position, QStringLiteral("The element exceeds its declared boundary."));
    }

    void ParseAttribute(int level, int elementHeaderEnd, bool isLastAttribute)
    {
        CountRecord();

        const int start = position;
        const quint16 nextAttributeOffset = ReadUInt16(elementHeaderEnd);
        if (isLastAttribute && nextAttributeOffset != 0)
            FailAt(start, QStringLiteral("The final attribute has a non-zero next offset."));
        if (!isLastAttribute && nextAttributeOffset == 0)
            FailAt(start, QStringLiteral("A non-final attribute has a zero next offset."));

        const int attributeEnd = nextAttributeOffset == 0
            ? elementHeaderEnd
            : CheckedOffset(start, nextAttributeOffset, QStringLiteral("next attribute"));

        if (attributeEnd <= position || attributeEnd > elementHeaderEnd)
            FailAt(start, QStringLiteral("The next-attribute offset is invalid."));

        const quint8 attributeType = ReadByte(attributeEnd);
        QString description;

        switch (attributeType)
        {
        case 0x00:
            description = FormatNamedAttribute(QStringLiteral("named"), attributeEnd);
            break;
        case 0xE0:
            description = FormatNamedAttribute(QStringLiteral("event"), attributeEnd);
            break;
        case 0x80:
        {
            const quint16 unknown = ReadUInt16(attributeEnd);
            description = FormatNamedAttribute(QStringLiteral("JScript, unknown=%1").arg(unknown), attributeEnd);
            break;
        }
        case 0x40:
        {
            const qint32 namedAddend = ReadInt32(attributeEnd);
            const QString namedProperty = ReadWideString(attributeEnd);
            const QString namedValue = ReadWideString(attributeEnd);
            description = QStringLiteral("wmpprop %1 = %2")
                              .arg(Quote(namedProperty), Quote(FormatWmpPropValue(namedValue, namedAddend)));
            break;
        }
        case 0x48:
        case 0xC8:
        {
            // The documented layout contains a four-byte addend before
            // the dispid. Real WMP resources also use a compact layout
            // that starts directly with dispid + padding. Detect both
            // from the position of the required zero padding word.
            qint32 addend = 0;
            quint16 propertyDispId = 0;
            const int valueStart = position;

            if (HasZeroWord(valueStart + 6, attributeEnd) &&
                IsWideStringToEnd(valueStart + 8, attributeEnd))
            {
                addend = ReadInt32(attributeEnd);
                propertyDispId = ReadUInt16(attributeEnd);
                ReadPaddingWord(attributeEnd);
            }
            else if (HasZeroWord(valueStart + 2, attributeEnd) &&
                     IsWideStringToEnd(valueStart + 4, attributeEnd))
            {
                propertyDispId = ReadUInt16(attributeEnd);
                ReadPaddingWord(attributeEnd);
            }
            else
            {
                FailAt(valueStart, QStringLiteral("Invalid wmpprop attribute layout."));
            }

            const QString propertyValue = ReadWideString(attributeEnd);
            description = QStringLiteral("%1 dispid=%2 value=%3")
                              .arg(attributeType == 0x48 ? QStringLiteral("wmpprop") : QStringLiteral("wmpprop2"))
                              .arg(propertyDispId)
                              .arg(Quote(FormatWmpPropValue(propertyValue, addend)));
            break;
        }
        default:
            description = ParseUnnamedAttribute(attributeType, attributeEnd);
            break;
        }

        if (position != attributeEnd)
            FailAt(position, QStringLiteral("The attribute length does not match its next offset."));

        AppendIndent(level);
        output.append(QStringLiteral("attribute @0x%1 [type=0x%2] ")
                          .arg(QString::number(start, 16).rightJustified(8, QChar(u'0')).toUpper())
                          .arg(QString::number(attributeType, 16).rightJustified(2, QChar(u'0')).toUpper()));
        output.append(description);
        output.append(QChar(u'\n'));
    }

    QString FormatNamedAttribute(const QString& kind, int attributeEnd)
    {
        const QString name = ReadWideString(attributeEnd);
        const QString value = ReadWideString(attributeEnd);
        return kind + QChar(u' ') + Quote(name) + QStringLiteral(" = ") + Quote(value);
    }

    QString ParseUnnamedAttribute(quint8 attributeType, int attributeEnd)
    {
        const quint16 dispId = ReadUInt16(attributeEnd);
        ReadPaddingWord(attributeEnd);

        switch (attributeType)
        {
        case 0x01:
        {
            const quint16 boolValue = ReadUInt16(attributeEnd);
            const QString value = boolValue == 0 ? QStringLiteral("false")
                : boolValue == 1 ? QStringLiteral("true")
                                 : QStringLiteral("true (raw=%1)").arg(boolValue);
            return QStringLiteral("boolean dispid=%1 value=%2").arg(dispId).arg(value);
        }
        case 0x04:
            return QStringLiteral("integer dispid=%1 value=%2").arg(dispId).arg(ReadInt32(attributeEnd));
        case 0x18:
            return QStringLiteral("resourcestring dispid=%1 id=%2").arg(dispId).arg(ReadInt32(attributeEnd));
        case 0x08:
            return QStringLiteral("string dispid=%1 value=%2").arg(dispId).arg(Quote(ReadWideString(attributeEnd)));
        case 0x28:
            return QStringLiteral("wmpenabled dispid=%1 value=%2").arg(dispId).arg(Quote(ReadWideString(attributeEnd)));
        case 0x0D:
            return QStringLiteral("sysint dispid=%1 value=%2").arg(dispId).arg(Quote(ReadWideString(attributeEnd)));
        case 0x88:
            return QStringLiteral("JScript dispid=%1 value=%2").arg(dispId).arg(Quote(ReadWideString(attributeEnd)));
        default:
            FailAt(position - 1, QStringLiteral("Unrecognized attribute type 0x%1.")
                                     .arg(QString::number(attributeType, 16).rightJustified(2, QChar(u'0')).toUpper()));
            return {};
        }
    }

    quint8 ReadByte(int limit)
    {
        EnsureAvailable(1, limit);
        return quint8(data[position++]);
    }

    quint16 ReadUInt16(int limit)
    {
        EnsureAvailable(2, limit);
        const quint16 value = quint16(quint8(data[position])) |
                              (quint16(quint8(data[position + 1])) << 8);
        position += 2;
        return value;
    }

    qint32 ReadInt32(int limit)
    {
        EnsureAvailable(4, limit);
        const quint32 value = quint32(quint8(data[position])) |
                              (quint32(quint8(data[position + 1])) << 8) |
                              (quint32(quint8(data[position + 2])) << 16) |
                              (quint32(quint8(data[position + 3])) << 24);
        position += 4;
        return qint32(value);
    }

    QString ReadGuid(int limit)
    {
        EnsureAvailable(16, limit);
        const auto b = [this](int index) { return quint8(data[position + index]); };
        const quint32 d1 = quint32(b(0)) | (quint32(b(1)) << 8) | (quint32(b(2)) << 16) | (quint32(b(3)) << 24);
        const quint16 d2 = quint16(b(4)) | (quint16(b(5)) << 8);
        const quint16 d3 = quint16(b(6)) | (quint16(b(7)) << 8);
        const QString value = QStringLiteral("{%1-%2-%3-%4%5-%6%7%8%9%10%11}")
                                  .arg(d1, 8, 16, QChar(u'0'))
                                  .arg(d2, 4, 16, QChar(u'0'))
                                  .arg(d3, 4, 16, QChar(u'0'))
                                  .arg(b(8), 2, 16, QChar(u'0'))
                                  .arg(b(9), 2, 16, QChar(u'0'))
                                  .arg(b(10), 2, 16, QChar(u'0'))
                                  .arg(b(11), 2, 16, QChar(u'0'))
                                  .arg(b(12), 2, 16, QChar(u'0'))
                                  .arg(b(13), 2, 16, QChar(u'0'))
                                  .arg(b(14), 2, 16, QChar(u'0'))
                                  .arg(b(15), 2, 16, QChar(u'0'))
                                  .toUpper();
        position += 16;
        return value;
    }

    QString ReadWideString(int limit)
    {
        const int start = position;
        while (position + 1 < limit)
        {
            if (data[position] == 0 && data[position + 1] == 0)
            {
                const int byteLength = position - start;
                QString value;
                value.reserve(byteLength / 2);
                for (int i = start; i < position; i += 2)
                {
                    const ushort codeUnit = ushort(quint8(data[i])) |
                                            (ushort(quint8(data[i + 1])) << 8);
                    value.append(QChar(codeUnit));
                }
                position += 2;
                return value;
            }
            position += 2;
        }

        FailAt(start, QStringLiteral("A Unicode string is not zero-terminated within its boundary."));
        return {};
    }

    void ReadPaddingWord(int limit)
    {
        const quint16 padding = ReadUInt16(limit);
        if (padding != 0)
            FailAt(position - 2, QStringLiteral("A reserved padding word is non-zero."));
    }

    bool HasZeroWord(int offset, int limit) const
    {
        return offset >= 0 && offset <= limit - 2 &&
               data[offset] == 0 && data[offset + 1] == 0;
    }

    bool IsWideStringToEnd(int offset, int end) const
    {
        if (offset < 0 || end > data.size() || offset > end - 2 ||
            ((end - offset) & 1) != 0)
            return false;

        for (int i = offset; i + 1 < end; i += 2)
        {
            if (data[i] == 0 && data[i + 1] == 0)
                return i + 2 == end;
        }
        return false;
    }

    void EnsureAvailable(int count, int limit)
    {
        if (limit < 0 || limit > data.size() || position < 0 ||
            count < 0 || position > limit - count)
            FailAt(position, QStringLiteral("Unexpected end of WSZ data."));
    }

    void SkipZeroPadding(int end, const QString& description)
    {
        while (position < end && data[position] == 0)
            ++position;

        if (position != end)
            FailAt(position, QStringLiteral("Unexpected non-zero %1.").arg(description));
    }

    void CountRecord()
    {
        ++recordsParsed;
        if (recordsParsed > MaximumRecords)
            Fail(QStringLiteral("The resource contains too many elements or attributes."));
    }

    static int CheckedOffset(int basePosition, quint16 relativeOffset, const QString& description)
    {
        if (basePosition > std::numeric_limits<int>::max() - int(relativeOffset))
            throw std::overflow_error(QStringLiteral("The %1 offset is too large.").arg(description).toStdString());
        return basePosition + int(relativeOffset);
    }

    static QString FormatWmpPropValue(const QString& value, qint32 addend)
    {
        if (addend > 0)
            return value + QChar(u'+') + QString::number(addend);
        if (addend < 0)
            return value + QString::number(addend);
        return value;
    }

    static QString Quote(const QString& value)
    {
        QString result;
        result.reserve(value.size() + 2);
        result.append(QChar(u'"'));
        for (const QChar ch : value)
        {
            switch (ch.unicode())
            {
            case u'\\': result.append(QStringLiteral("\\\\")); break;
            case u'"': result.append(QStringLiteral("\\\"")); break;
            case u'\r': result.append(QStringLiteral("\\r")); break;
            case u'\n': result.append(QStringLiteral("\\n")); break;
            case u'\t': result.append(QStringLiteral("\\t")); break;
            default:
                if (ch.category() == QChar::Other_Control)
                    result.append(QStringLiteral("\\u%1").arg(ch.unicode(), 4, 16, QChar(u'0')).toUpper());
                else
                    result.append(ch);
                break;
            }
        }
        result.append(QChar(u'"'));
        return result;
    }

    void AppendIndent(int level)
    {
        output.append(QString(level * 2, QChar(u' ')));
    }

    [[noreturn]] static void Fail(const QString& message)
    {
        throw WszFormatException(message);
    }

    [[noreturn]] static void FailAt(int offset, const QString& message)
    {
        throw WszFormatException(QStringLiteral("Offset 0x%1: %2")
                                     .arg(QString::number(offset, 16).rightJustified(8, QChar(u'0')).toUpper())
                                     .arg(message));
    }
};

} // namespace

bool WszDecoder::TryDecode(const QByteArray& data, QString& decoded)
{
    decoded.clear();
    if (!LooksLikeWsz(data))
        return false;

    try
    {
        decoded = Parser(data).Decode();
        return true;
    }
    catch (const WszFormatException& ex)
    {
        qWarning().noquote() << "Header matched a Windows Media Player WSZ skin, but decoding failed:"
                             << ex.what();
        return false;
    }
    catch (const std::overflow_error& ex)
    {
        qWarning().noquote() << "Header matched a Windows Media Player WSZ skin, but an offset overflowed:"
                             << ex.what();
        return false;
    }
}

bool WszDecoder::LooksLikeWsz(const QByteArray& data)
{
    if (data.size() < 9)
        return false;

    // The root must be a THEME element. It has no sibling and its
    // predefined-element padding word is zero.
    const quint16 nextSiblingOffset = readUInt16At(data, 0);
    const quint16 firstChildOffset = readUInt16At(data, 2);
    const quint8 elementType = quint8(data[4]);
    const quint8 childCount = quint8(data[5]);

    if (nextSiblingOffset != 0 || elementType != ElementTheme ||
        data[7] != 0 || data[8] != 0)
        return false;

    if (childCount == 0)
        return firstChildOffset == 0;
    if (firstChildOffset == 0)
        return false;

    // Element offsets are measured from the start of the element.
    const int firstChildPosition = firstChildOffset;
    return firstChildPosition >= 9 && firstChildPosition < data.size();
}

} // namespace resources
} // namespace peare
