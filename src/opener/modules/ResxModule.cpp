#include "ResxModule.h"
#include "Compat.h"

#include <QFile>
#include <QXmlStreamReader>
#include <utility>

namespace peare {
namespace {

const qint64 kMaxResxSize = qint64(512) * 1024 * 1024;
const qsizetype kMaxDecodedResourceSize = qsizetype(512) * 1024 * 1024;

struct ResxNode {
    QString name;
    QString type;
    QString mimeType;
    QString value;
};

int base64Digit(ushort c)
{
    if (c >= 'A' && c <= 'Z') return int(c - 'A');
    if (c >= 'a' && c <= 'z') return int(c - 'a') + 26;
    if (c >= '0' && c <= '9') return int(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

bool decodeBase64(const QString& text, QByteArray* output, QString* error)
{
    QString clean;
    clean.reserve(text.size());
    for (const QChar ch : text) {
        if (ch.isSpace()) continue;
        if (ch.unicode() > 0x7f) {
            *error = QStringLiteral("RESX Base64 value contains a non-ASCII character");
            return false;
        }
        clean.append(ch);
    }

    output->clear();
    if (clean.isEmpty()) return true;
    if ((clean.size() & 3) != 0) {
        *error = QStringLiteral("RESX Base64 value has an invalid length");
        return false;
    }

    const qsizetype padding = (clean.endsWith(QStringLiteral("=="))) ? 2
                           : (clean.endsWith(QLatin1Char('=')) ? 1 : 0);
    const qsizetype decodedSize = (clean.size() / 4) * 3 - padding;
    if (decodedSize > kMaxDecodedResourceSize) {
        *error = QStringLiteral("RESX decoded resource exceeds the safety limit");
        return false;
    }
    output->reserve(decodedSize);

    for (qsizetype i = 0; i < clean.size(); i += 4) {
        const bool last = i + 4 == clean.size();
        const ushort c0 = clean.at(i).unicode();
        const ushort c1 = clean.at(i + 1).unicode();
        const ushort c2 = clean.at(i + 2).unicode();
        const ushort c3 = clean.at(i + 3).unicode();
        const int a = base64Digit(c0);
        const int b = base64Digit(c1);
        const int c = c2 == '=' ? 0 : base64Digit(c2);
        const int d = c3 == '=' ? 0 : base64Digit(c3);
        if (a < 0 || b < 0 || c < 0 || d < 0 ||
            (!last && (c2 == '=' || c3 == '=')) ||
            (c2 == '=' && c3 != '=') ||
            (c2 == '=' && (b & 0x0f) != 0) ||
            (c3 == '=' && c2 != '=' && (c & 0x03) != 0)) {
            *error = QStringLiteral("RESX Base64 value is malformed");
            return false;
        }

        output->append(char((a << 2) | (b >> 4)));
        if (c2 != '=') output->append(char(((b & 0x0f) << 4) | (c >> 2)));
        if (c3 != '=') output->append(char(((c & 0x03) << 6) | d));
    }
    return true;
}

bool isBase64MimeType(const QString& mimeType)
{
    return mimeType.trimmed().toLower().endsWith(QStringLiteral("base64"));
}

bool isByteArrayPayload(const ResxNode& node)
{
    const QString mime = node.mimeType.trimmed().toLower();
    if (mime == QStringLiteral("application/x-microsoft.net.object.bytearray.base64"))
        return true;
    return node.type.trimmed().startsWith(QStringLiteral("System.Byte[]"), Qt::CaseInsensitive);
}

bool isFileReference(const ResxNode& node)
{
    return node.type.contains(QStringLiteral("System.Resources.ResXFileRef"), Qt::CaseInsensitive);
}

bool readNamedNode(QXmlStreamReader& xml, ResxNode* node)
{
    const QXmlStreamAttributes attributes = xml.attributes();
    node->name = attributes.value(QLatin1String("name")).toString();
    node->type = attributes.value(QLatin1String("type")).toString();
    node->mimeType = attributes.value(QLatin1String("mimetype")).toString();

    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("value")) {
            node->value = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        } else {
            xml.skipCurrentElement();
        }
    }
    return !xml.hasError();
}

QString readHeader(QXmlStreamReader& xml)
{
    QString value;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("value"))
            value = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        else
            xml.skipCurrentElement();
    }
    return value;
}

bool makeEntry(const ResxNode& node, bool metadata, ResourceEntry* output, QString* error)
{
    if (node.name.isEmpty()) {
        *error = metadata ? QStringLiteral("RESX metadata entry has no name")
                          : QStringLiteral("RESX data entry has no name");
        return false;
    }

    ResourceEntry entry;
    const bool fileReference = isFileReference(node);
    const bool encoded = !fileReference && isBase64MimeType(node.mimeType);
    const bool byteArray = encoded && isByteArrayPayload(node);

    if (metadata) {
        entry.type = QStringLiteral("RESX_METADATA");
    } else if (fileReference) {
        entry.type = QStringLiteral("RESX_FILE_REF");
    } else if (encoded && byteArray) {
        entry.type = QStringLiteral("RESX_BINARY");
    } else if (encoded) {
        entry.type = QStringLiteral("RESX_SERIALIZED_OBJECT");
    } else {
        entry.type = QStringLiteral("RESX_DATA");
    }

    if (encoded) {
        if (!decodeBase64(node.value, &entry.data, error)) {
            *error = QStringLiteral("%1: %2").arg(node.name, *error);
            return false;
        }
    } else {
        entry.data = node.value.toUtf8();
    }

    entry.name = node.name;
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = 0;
    entry.dataSize = quint64(entry.data.size());
    entry.format = ModuleFormat::RESX;
    entry.hierarchyPath = QStringList()
        << (metadata ? QStringLiteral("Metadata") : QStringLiteral("Data"));
    entry.isEmbeddedFile = byteArray;
    *output = std::move(entry);
    return true;
}

} // namespace

std::unique_ptr<ResxModule> ResxModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<ResxModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::RESX;
        module->info_.error = file.errorString();
        return module;
    }
    if (file.size() > kMaxResxSize) {
        auto module = peare::makeUnique<ResxModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::RESX;
        module->info_.error = QStringLiteral("RESX file exceeds the safety limit");
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<ResxModule> ResxModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<ResxModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::RESX;
    module->info_.description = QStringLiteral(".NET XML resource (RESX)");

    if (data.size() > kMaxResxSize) {
        module->info_.error = QStringLiteral("RESX file exceeds the safety limit");
        return module;
    }

    QXmlStreamReader xml(data);
    xml.setEntityExpansionLimit(1024);
    if (!xml.readNextStartElement() || xml.name() != QLatin1String("root")) {
        module->info_.error = xml.hasError()
            ? QStringLiteral("Invalid RESX XML: %1").arg(xml.errorString())
            : QStringLiteral("Invalid RESX root element");
        return module;
    }

    bool validMimeHeader = false;
    QString parseError;
    while (xml.readNextStartElement()) {
        if (xml.name() == QLatin1String("resheader")) {
            const QString name = xml.attributes().value(QLatin1String("name")).toString();
            const QString value = readHeader(xml);
            if (name.compare(QStringLiteral("resmimetype"), Qt::CaseInsensitive) == 0 &&
                value.trimmed().compare(QStringLiteral("text/microsoft-resx"), Qt::CaseInsensitive) == 0)
                validMimeHeader = true;
        } else if (xml.name() == QLatin1String("data") ||
                   xml.name() == QLatin1String("metadata")) {
            const bool metadata = xml.name() == QLatin1String("metadata");
            ResxNode node;
            ResourceEntry entry;
            if (!readNamedNode(xml, &node) ||
                !makeEntry(node, metadata, &entry, &parseError)) {
                if (parseError.isEmpty()) parseError = xml.errorString();
                break;
            }
            module->resources_.push_back(std::move(entry));
        } else {
            xml.skipCurrentElement();
        }
    }

    if (!parseError.isEmpty()) {
        module->resources_.clear();
        module->info_.error = parseError;
    } else if (xml.hasError()) {
        module->resources_.clear();
        module->info_.error = QStringLiteral("Invalid RESX XML: %1").arg(xml.errorString());
    } else if (!validMimeHeader) {
        module->resources_.clear();
        module->info_.error = QStringLiteral("RESX resmimetype header is missing or invalid");
    }
    return module;
}

} // namespace peare
