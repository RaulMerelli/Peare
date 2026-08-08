#include "MsgModule.h"
#include "Compat.h"
#include "OleCompound.h"

#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace peare {
namespace {

const QString kSubstgPrefix = QStringLiteral("__substg1.0_");

QByteArray utf16LeNeedle(const QString& text)
{
    QByteArray bytes;
    bytes.reserve(text.size() * 2);
    for (const QChar ch : text) {
        const ushort value = ch.unicode();
        bytes.append(char(value & 0xff));
        bytes.append(char((value >> 8) & 0xff));
    }
    return bytes;
}

QByteArray readSmall(const fs::ByteStorePtr& store, std::int64_t maximum = 1024 * 1024)
{
    if (!store || store->capacity() <= 0 || store->capacity() > maximum)
        return {};
    const std::vector<std::uint8_t> bytes = store->readRange(0, store->capacity());
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
}

QString decodeUnicode(const fs::ByteStorePtr& store)
{
    const QByteArray bytes = readSmall(store);
    if (bytes.size() < 2) return {};
    QVector<ushort> units;
    units.reserve(bytes.size() / 2);
    for (int pos = 0; pos + 1 < bytes.size(); pos += 2) {
        const ushort value = ushort(quint8(bytes.at(pos))) |
                             (ushort(quint8(bytes.at(pos + 1))) << 8);
        if (value == 0) break;
        units.push_back(value);
    }
    return units.isEmpty() ? QString() : QString::fromUtf16(units.constData(), units.size());
}

QString decodeAnsi(const fs::ByteStorePtr& store)
{
    QByteArray bytes = readSmall(store);
    const int zero = bytes.indexOf('\0');
    if (zero >= 0) bytes.truncate(zero);
    return QString::fromLatin1(bytes);
}

QString propertyTag(const QString& name)
{
    if (!name.startsWith(kSubstgPrefix) || name.size() < kSubstgPrefix.size() + 8)
        return {};
    return name.mid(kSubstgPrefix.size(), 8).toUpper();
}

QString propertyString(const OleCompoundStream& stream)
{
    const QString tag = propertyTag(stream.name);
    if (tag.endsWith(QStringLiteral("001F"))) return decodeUnicode(stream.content);
    if (tag.endsWith(QStringLiteral("001E"))) return decodeAnsi(stream.content);
    return {};
}

bool samePath(const QStringList& a, const QStringList& b)
{
    if (a.size() != b.size())
        return false;

    for (int i = 0; i < a.size(); ++i) {
        if (a.at(i) != b.at(i))
            return false;
    }

    return true;
}

const OleCompoundStream* findSibling(const OleCompoundContents& compound,
                                     const QStringList& path,
                                     const QString& tag)
{
    const QString target = kSubstgPrefix + tag;
    for (const OleCompoundStream& stream : compound.streams) {
        if (samePath(stream.hierarchyPath, path) &&
            stream.name.compare(target, Qt::CaseInsensitive) == 0)
            return &stream;
    }
    return nullptr;
}

const OleCompoundStream* findRootProperty(const OleCompoundContents& compound,
                                          const QString& tag)
{
    return findSibling(compound, {}, tag);
}

QString stringProperty(const OleCompoundContents& compound,
                       const QStringList& path,
                       const QStringList& tags)
{
    for (const QString& tag : tags) {
        const OleCompoundStream* stream = findSibling(compound, path, tag);
        if (!stream) continue;
        const QString value = propertyString(*stream).trimmed();
        if (!value.isEmpty()) return value;
    }
    return {};
}

bool isAttachmentPath(const QStringList& path)
{
    return !path.isEmpty() &&
           path.first().startsWith(QStringLiteral("__attach_version1.0_#"),
                                   Qt::CaseInsensitive);
}

int storageOrdinal(const QString& storage)
{
    const int hash = storage.lastIndexOf(QLatin1Char('#'));
    bool ok = false;
    const int value = hash >= 0 ? storage.mid(hash + 1).toInt(&ok, 16) : 0;
    return ok ? value + 1 : 0;
}

QStringList friendlyPath(const QStringList& path)
{
    QStringList result;
    for (const QString& part : path) {
        if (part.startsWith(QStringLiteral("__attach_version1.0_#"), Qt::CaseInsensitive)) {
            result.push_back(QStringLiteral("Attachments"));
            const int n = storageOrdinal(part);
            result.push_back(n > 0 ? QStringLiteral("Attachment %1").arg(n)
                                   : QStringLiteral("Attachment"));
        } else if (part.startsWith(QStringLiteral("__recip_version1.0_#"), Qt::CaseInsensitive)) {
            result.push_back(QStringLiteral("Recipients"));
            const int n = storageOrdinal(part);
            result.push_back(n > 0 ? QStringLiteral("Recipient %1").arg(n)
                                   : QStringLiteral("Recipient"));
        } else if (part.compare(QStringLiteral("__nameid_version1.0"),
                                Qt::CaseInsensitive) == 0) {
            result.push_back(QStringLiteral("Named properties"));
        } else if (part.compare(QStringLiteral("__substg1.0_3701000D"),
                                Qt::CaseInsensitive) == 0) {
            result.push_back(QStringLiteral("Embedded message"));
        } else {
            result.push_back(part);
        }
    }
    return result;
}

QString friendlyPropertyName(const QString& tag)
{
    if (tag == QStringLiteral("001A001F") || tag == QStringLiteral("001A001E"))
        return QStringLiteral("Message class");
    if (tag == QStringLiteral("0037001F") || tag == QStringLiteral("0037001E"))
        return QStringLiteral("Subject");
    if (tag == QStringLiteral("007D001F") || tag == QStringLiteral("007D001E"))
        return QStringLiteral("Transport headers");
    if (tag == QStringLiteral("1000001F") || tag == QStringLiteral("1000001E"))
        return QStringLiteral("Body");
    if (tag == QStringLiteral("10130102")) return QStringLiteral("HTML body");
    if (tag == QStringLiteral("10090102")) return QStringLiteral("Compressed RTF body");
    if (tag == QStringLiteral("3001001F") || tag == QStringLiteral("3001001E"))
        return QStringLiteral("Display name");
    if (tag == QStringLiteral("39FE001F") || tag == QStringLiteral("39FE001E"))
        return QStringLiteral("SMTP address");
    if (tag == QStringLiteral("3704001F") || tag == QStringLiteral("3704001E"))
        return QStringLiteral("Attachment filename");
    if (tag == QStringLiteral("3707001F") || tag == QStringLiteral("3707001E"))
        return QStringLiteral("Attachment long filename");
    if (tag == QStringLiteral("370E001F") || tag == QStringLiteral("370E001E"))
        return QStringLiteral("Attachment MIME type");
    return {};
}

bool hasMsgStructure(const OleCompoundContents& compound)
{
    bool properties = false;
    for (const OleCompoundStream& stream : compound.streams) {
        if (stream.hierarchyPath.isEmpty() &&
            stream.name.compare(QStringLiteral("__properties_version1.0"),
                                Qt::CaseInsensitive) == 0) {
            properties = true;
            break;
        }
    }
    if (!properties) return false;

    const OleCompoundStream* messageClass = findRootProperty(compound, QStringLiteral("001A001F"));
    if (!messageClass) messageClass = findRootProperty(compound, QStringLiteral("001A001E"));
    if (!messageClass) return false;
    return propertyString(*messageClass).startsWith(QStringLiteral("IPM."), Qt::CaseInsensitive);
}

QString attachmentName(const OleCompoundContents& compound,
                       const OleCompoundStream& dataStream)
{
    QString name = stringProperty(compound, dataStream.hierarchyPath,
        {QStringLiteral("3707001F"), QStringLiteral("3707001E"),
         QStringLiteral("3704001F"), QStringLiteral("3704001E")});
    name = QFileInfo(name).fileName();
    if (!name.isEmpty()) return name;
    const int n = storageOrdinal(dataStream.hierarchyPath.first());
    return n > 0 ? QStringLiteral("Attachment %1").arg(n) : QStringLiteral("Attachment");
}

} // namespace

bool MsgModule::hasDirectoryMarkers(const QByteArray& data)
{
    if (!hasOleCompoundMagic(data)) return false;
    const QByteArray props = utf16LeNeedle(QStringLiteral("__properties_version1.0"));
    const QByteArray clsW = utf16LeNeedle(QStringLiteral("__substg1.0_001A001F"));
    const QByteArray clsA = utf16LeNeedle(QStringLiteral("__substg1.0_001A001E"));
    return data.contains(props) && (data.contains(clsW) || data.contains(clsA));
}

bool MsgModule::isOutlookMessage(const fs::ByteStorePtr& file)
{
    if (!file) return false;
    const std::vector<std::uint8_t> head = file->readRange(0, 8);
    const QByteArray header(reinterpret_cast<const char*>(head.data()), int(head.size()));
    if (!hasOleCompoundMagic(header)) return false;
    OleCompoundContents compound;
    return enumerateOleCompound(file, &compound, nullptr) && hasMsgStructure(compound);
}

bool MsgModule::isOutlookMessageFile(const QString& filePath)
{
    return isOutlookMessage(oleStoreForFile(filePath));
}

ModulePtr MsgModule::open(const QString& filePath)
{
    return open(oleStoreForFile(filePath), filePath);
}

ModulePtr MsgModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr MsgModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<MsgModule>();
    module->file_ = file;
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::MSG;
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open Outlook MSG file");
        return ModulePtr(std::move(module));
    }

    OleCompoundContents compound;
    QString error;
    if (!enumerateOleCompound(file, &compound, &error)) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }
    if (!hasMsgStructure(compound)) {
        module->info_.error = QStringLiteral("Not an Outlook MSG structured-storage message");
        return ModulePtr(std::move(module));
    }

    int attachments = 0;
    module->resources_.reserve(compound.streams.size());
    for (const OleCompoundStream& stream : compound.streams) {
        const QString tag = propertyTag(stream.name);
        const bool attachmentData = isAttachmentPath(stream.hierarchyPath) &&
                                    tag == QStringLiteral("37010102");

        ResourceEntry resource;
        resource.dataSize = stream.size;
        resource.dataOffset = stream.dataOffset;
        resource.content = stream.content;
        resource.language = stream.miniStream
            ? QStringLiteral("OLE mini stream") : QStringLiteral("OLE stream");

        if (attachmentData) {
            ++attachments;
            resource.type = QStringLiteral("MSG_ATTACHMENT");
            resource.name = attachmentName(compound, stream);
            resource.hierarchyPath = QStringList() << QStringLiteral("Attachments");
            resource.isEmbeddedFile = stream.size > 0;
        } else {
            resource.format = ModuleFormat::MSG;
            resource.type = stream.name.compare(QStringLiteral("__properties_version1.0"),
                                                Qt::CaseInsensitive) == 0
                ? QStringLiteral("MSG_PROPERTY_TABLE") : QStringLiteral("MSG_PROPERTY");
            const QString friendly = friendlyPropertyName(tag);
            resource.name = friendly.isEmpty() ? stream.name
                : QStringLiteral("%1 [%2]").arg(friendly, tag);
            resource.hierarchyPath = stream.hierarchyPath.isEmpty()
                ? QStringList{QStringLiteral("Message")} : friendlyPath(stream.hierarchyPath);
            if (tag.endsWith(QStringLiteral("001F"))) resource.codePage = 1200;
        }
        module->resources_.push_back(std::move(resource));
    }

    const QString subject = stringProperty(compound, {},
        {QStringLiteral("0037001F"), QStringLiteral("0037001E")});
    module->info_.description = subject.isEmpty()
        ? QStringLiteral("Microsoft Outlook MSG message — %1 streams, %2 attachments")
              .arg(compound.streams.size()).arg(attachments)
        : QStringLiteral("Microsoft Outlook MSG message — %1 — %2 streams, %3 attachments")
              .arg(subject).arg(compound.streams.size()).arg(attachments);
    return ModulePtr(std::move(module));
}

} // namespace peare
