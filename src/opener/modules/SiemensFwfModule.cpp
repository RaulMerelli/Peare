#include "SiemensFwfModule.h"
#include "Compat.h"

#include <QFile>
#include <QFileInfo>

#include "SiemensFwfParser.h"

namespace peare {
namespace {

std::vector<std::uint8_t> toVector(const QByteArray& data)
{
    const auto* begin = reinterpret_cast<const std::uint8_t*>(data.constData());
    return std::vector<std::uint8_t>(begin, begin + data.size());
}

QByteArray toByteArray(const std::vector<std::uint8_t>& data)
{
    return QByteArray(reinterpret_cast<const char*>(data.data()), int(data.size()));
}

QString safeName(const std::string& name, std::size_t index)
{
    QString value = QString::fromLatin1(name.c_str());
    if (value.isEmpty()) value = QStringLiteral("blob_%1").arg(qulonglong(index));
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-') || c == QLatin1Char('_')))
            value[i] = QLatin1Char('_');
    }
    return value;
}

void addResource(QVector<ResourceEntry>& resources, const QString& type,
                 const QString& name, const QByteArray& data, quint64 offset,
                 const QStringList& path = {})
{
    ResourceEntry entry;
    entry.type = type;
    entry.name = name;
    entry.dataOffset = offset;
    entry.dataSize = quint64(data.size());
    // For container-file types the tree uses the last hierarchyPath segment as
    // the resource leaf, so each resource needs a unique full path ending in its
    // own name (otherwise multiple resources sharing a folder collapse into one
    // node). Mirrors the OS2_PACK_FILE / SZDD_FILE convention.
    entry.hierarchyPath = path;
    entry.hierarchyPath << name;
    entry.data = data;
    resources.push_back(entry);
}

QByteArray readAll(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
    return file.readAll();
}

} // namespace

ModulePtr SiemensFwfModule::open(const QString& filePath)
{
    QString error;
    const QByteArray bytes = readAll(filePath, error);
    if (!error.isEmpty()) {
        auto module = peare::makeUnique<SiemensFwfModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SIEMENS_FWF;
        module->info_.description = QStringLiteral("Siemens FWF OMS firmware archive");
        module->info_.error = error;
        return module;
    }
    return open(bytes, filePath);
}

ModulePtr SiemensFwfModule::open(const QByteArray& bytes, const QString& logicalName)
{
    auto module = peare::makeUnique<SiemensFwfModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SIEMENS_FWF;
    module->info_.description = QStringLiteral("Siemens FWF OMS firmware archive");
    try {
        const FwfDecoded decoded = decode_fwf(toVector(bytes));
        if (!decoded.nk_image.empty())
            addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), QStringLiteral("NK.bin"), toByteArray(decoded.nk_image), 0);

        std::size_t flashIndex = 0;
        for (const auto& blob : decoded.blobs) {
            if (blob.kind == FwfPayloadKind::Empty) continue;
            const QString base = QStringLiteral("%1_%2").arg(qulonglong(blob.index)).arg(safeName(blob.name.empty() ? blob.class_name : blob.name, blob.index));
            if (blob.kind == FwfPayloadKind::Zlib && is_nk_part_name(blob.name)) {
                // Matches the original default: chunks are omitted unless its CLI is invoked with --dump-chunks.
                continue;
            }
            if (blob.kind == FwfPayloadKind::Zlib) {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".inflated.bin"), toByteArray(blob.inflated), blob.payload_offset, {QStringLiteral("image_parts")});
            } else if (blob.kind == FwfPayloadKind::Fsf) {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".fsf"), toByteArray(blob.payload), blob.payload_offset, {QStringLiteral("image_parts")});
            } else if (blob.kind == FwfPayloadKind::FlashImage) {
                const QString name = flashIndex++ == 0 ? QStringLiteral("flash_image.bin") : base + QStringLiteral(".flash.bin");
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), name, toByteArray(blob.payload), blob.payload_offset);
            } else if (blob.kind == FwfPayloadKind::Oms) {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".oms"), toByteArray(blob.payload), blob.payload_offset, {QStringLiteral("nested")});
            } else {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".bin"), toByteArray(blob.payload), blob.payload_offset, {QStringLiteral("image_parts")});
            }
        }
    } catch (const std::exception& ex) {
        module->info_.error = QString::fromLocal8Bit(ex.what());
    }
    return module;
}

} // namespace peare
