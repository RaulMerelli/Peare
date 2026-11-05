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

QString safeName(QString name, const QString& fallback)
{
    if (name.isEmpty()) name = fallback;
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.') ||
              c == QLatin1Char('-') || c == QLatin1Char('_')))
            name[i] = QLatin1Char('_');
    }
    return name.isEmpty() ? fallback : name;
}

void addResource(QVector<ResourceEntry>& resources, const QString& type,
                 const QString& name, const QByteArray& data, quint64 offset,
                 const QStringList& path = {}, bool embeddedFile = true)
{
    ResourceEntry entry;
    entry.type = type;
    entry.name = name;
    entry.dataOffset = offset;
    entry.dataSize = quint64(data.size());
    // Give each container file a unique path ending in its own name.
    // This keeps resources in the same folder as separate tree entries.
    entry.hierarchyPath = path;
    entry.hierarchyPath << name;
    entry.data = data;
    entry.isEmbeddedFile = embeddedFile;
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

        std::size_t fsfIndex = decoded.blobs.size();
        QByteArray assembledFsf;
        for (std::size_t i = 0; i < decoded.blobs.size(); ++i) {
            if (decoded.blobs[i].kind != FwfPayloadKind::Fsf) continue;
            fsfIndex = i;
            for (std::size_t j = i; j < decoded.blobs.size(); ++j) {
                const auto& payload = decoded.blobs[j].payload;
                if (!payload.empty())
                    assembledFsf.append(reinterpret_cast<const char*>(payload.data()),
                                        int(payload.size()));
            }
            break;
        }

        std::size_t flashIndex = 0;
        for (const auto& blob : decoded.blobs) {
            if (blob.kind == FwfPayloadKind::Empty) continue;
            const QString base = QStringLiteral("%1_%2").arg(qulonglong(blob.index)).arg(safeName(blob.name.empty() ? blob.class_name : blob.name, blob.index));
            QStringList path;
            if (!blob.object_path.empty()) {
                const QStringList components = QString::fromUtf8(blob.object_path.c_str())
                    .split(QLatin1Char('/'), Qt::SkipEmptyParts);
                for (const QString& component : components)
                    path << safeName(component, QStringLiteral("object_%1").arg(blob.index));
            }
            if (path.isEmpty()) {
                if (blob.kind == FwfPayloadKind::Zlib || blob.kind == FwfPayloadKind::Fsf ||
                    blob.kind == FwfPayloadKind::Raw)
                    path << QStringLiteral("image_parts");
                else if (blob.kind == FwfPayloadKind::Oms)
                    path << QStringLiteral("nested");
            }
            if (blob.kind == FwfPayloadKind::Zlib && is_nk_part_name(blob.name)) {
                // Matches the original default: chunks are omitted unless its CLI is invoked with --dump-chunks.
                continue;
            }
            if (blob.kind == FwfPayloadKind::Zlib) {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".inflated.bin"), toByteArray(blob.inflated), blob.payload_offset, path);
            } else if (blob.kind == FwfPayloadKind::Fsf) {
                const QByteArray original = toByteArray(blob.payload);
                const bool hasContinuation = blob.index == fsfIndex &&
                    assembledFsf.size() > original.size();
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"),
                            base + QStringLiteral(".fsf"), original, blob.payload_offset,
                            path, !hasContinuation);
                if (hasContinuation)
                    addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"),
                                QStringLiteral("assembled_flash.fsf"), assembledFsf,
                                blob.payload_offset, path);
            } else if (blob.kind == FwfPayloadKind::FlashImage) {
                const QString name = flashIndex++ == 0 ? QStringLiteral("flash_image.bin") : base + QStringLiteral(".flash.bin");
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), name, toByteArray(blob.payload), blob.payload_offset, path);
            } else if (blob.kind == FwfPayloadKind::Oms) {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".oms"), toByteArray(blob.payload), blob.payload_offset, path);
            } else {
                addResource(module->resources_, QStringLiteral("SIEMENS_FWF_FILE"), base + QStringLiteral(".bin"), toByteArray(blob.payload), blob.payload_offset, path);
            }
        }
    } catch (const std::exception& ex) {
        module->info_.error = QString::fromLocal8Bit(ex.what());
    }
    return module;
}

} // namespace peare
