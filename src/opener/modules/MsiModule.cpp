#include "MsiModule.h"
#include "Compat.h"
#include "OleCompound.h"

#include <QFileInfo>

#include <utility>

namespace peare {
namespace {

QString decodeMsiName(const QString& encoded)
{
    static const QString alphabet =
        QStringLiteral("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._");
    QString result;
    result.reserve(encoded.size() * 2);
    for (const QChar ch : encoded) {
        const ushort value = ch.unicode();
        if (value >= 0x3800 && value < 0x4800) {
            const ushort packed = ushort(value - 0x3800);
            result += alphabet.at(packed & 0x3f);
            result += alphabet.at((packed >> 6) & 0x3f);
        } else if (value >= 0x4800 && value < 0x4840) {
            result += alphabet.at(value - 0x4800);
        } else if (value < 0x20) {
            result += QStringLiteral("[%1]").arg(value, 2, 16, QLatin1Char('0')).toUpper();
        } else {
            result += ch;
        }
    }
    return result;
}

QStringList decodeMsiPath(const QStringList& path)
{
    QStringList result;
    result.reserve(path.size());
    for (const QString& part : path) result.push_back(decodeMsiName(part));
    return result;
}

} // namespace

bool MsiModule::hasCompoundMagic(const QByteArray& data)
{
    return hasOleCompoundMagic(data);
}

bool MsiModule::hasInstallerExtension(const QString& name)
{
    const QString suffix = QFileInfo(name).suffix().toLower();
    return suffix == QStringLiteral("msi") || suffix == QStringLiteral("msm") ||
           suffix == QStringLiteral("msp") || suffix == QStringLiteral("mst") ||
           suffix == QStringLiteral("pcp") || suffix == QStringLiteral("cub");
}

ModulePtr MsiModule::open(const QString& filePath)
{
    return open(oleStoreForFile(filePath), filePath);
}

ModulePtr MsiModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr MsiModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    auto module = peare::makeUnique<MsiModule>();
    module->file_ = file;
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::MSI;
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open Windows Installer database");
        return ModulePtr(std::move(module));
    }

    OleCompoundContents compound;
    QString error;
    if (!enumerateOleCompound(file, &compound, &error)) {
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }

    module->resources_.reserve(compound.streams.size());
    for (const OleCompoundStream& stream : compound.streams) {
        ResourceEntry resource;
        resource.type = QStringLiteral("MSI_STREAM");
        resource.name = decodeMsiName(stream.name);
        resource.language = stream.miniStream
            ? QStringLiteral("OLE mini stream") : QStringLiteral("OLE stream");
        resource.dataSize = stream.size;
        resource.dataOffset = stream.dataOffset;
        resource.format = ModuleFormat::MSI;
        resource.hierarchyPath = decodeMsiPath(stream.hierarchyPath);
        resource.isEmbeddedFile = stream.size > 0;
        resource.content = stream.content;
        module->resources_.push_back(std::move(resource));
    }

    module->info_.description = QStringLiteral(
        "Windows Installer structured-storage database — %1 streams, %2 storages")
        .arg(module->resources_.size()).arg(compound.storageCount);
    return ModulePtr(std::move(module));
}

} // namespace peare
