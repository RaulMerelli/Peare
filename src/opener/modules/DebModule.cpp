#include "DebModule.h"

#include "ArModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>
#include <cstring>

namespace peare {
namespace {

bool isTarMember(const QString& name, const QString& base, bool control)
{
    if (name == base) return true;
    if (!name.startsWith(base + QLatin1Char('.'))) return false;
    const QString compression = name.mid(base.size() + 1);
    if (compression == QStringLiteral("gz") || compression == QStringLiteral("xz") ||
        compression == QStringLiteral("zst")) return true;
    return !control && (compression == QStringLiteral("bz2") ||
                        compression == QStringLiteral("lzma"));
}

class InvalidDebModule final : public IModule, public IResourceContainer {
public:
    explicit InvalidDebModule(const QString& name, const QString& error)
    {
        info_.filePath = name;
        info_.format = ModuleFormat::DEB;
        info_.description = QStringLiteral("Debian binary package");
        info_.error = error;
    }
    const ModuleInfo& info() const noexcept override { return info_; }
    const QVector<ResourceEntry>& resources() const noexcept override { return resources_; }

private:
    ModuleInfo info_;
    QVector<ResourceEntry> resources_;
};

} // namespace

bool DebModule::hasSupportedExtension(const QString& name)
{
    const QString suffix = QFileInfo(name).suffix();
    return suffix.compare(QStringLiteral("deb"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QStringLiteral("udeb"), Qt::CaseInsensitive) == 0;
}

ModulePtr DebModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr DebModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr DebModule::open(const fs::ByteStorePtr& source, const QString& sourceName)
{
    ModulePtr archive = ArModule::open(source, sourceName);
    const ModuleInfo& arInfo = archive->info();
    const auto* arResources = dynamic_cast<const IResourceContainer*>(archive.get());
    if (!arInfo.isValid() || !arResources)
        return ModulePtr(new InvalidDebModule(sourceName,
            arInfo.error.isEmpty() ? QStringLiteral("Invalid ar payload") : arInfo.error));

    const QVector<ResourceEntry>& entries = arResources->resources();
    if (entries.size() < 3 || entries.first().name != QStringLiteral("debian-binary"))
        return ModulePtr(new InvalidDebModule(sourceName,
            QStringLiteral("Missing Debian binary package header or required members")));

    const ResourceEntry& version = entries.first();
    const std::vector<std::uint8_t> versionBytes = version.content
        ? version.content->readAll() : std::vector<std::uint8_t>();
    if (versionBytes.size() != 4 || std::memcmp(versionBytes.data(), "2.0\n", 4) != 0)
        return ModulePtr(new InvalidDebModule(sourceName,
            QStringLiteral("Unsupported Debian package version")));

    int controlIndex = -1;
    int dataIndex = -1;
    for (int i = 1; i < entries.size(); ++i) {
        const QString& name = entries[i].name;
        if (controlIndex < 0 && name.startsWith(QLatin1Char('_'))) continue;
        if (dataIndex >= 0) continue;
        if (controlIndex < 0 && isTarMember(name, QStringLiteral("control.tar"), true)) {
            controlIndex = i;
            continue;
        }
        if (controlIndex >= 0 && dataIndex < 0 &&
            isTarMember(name, QStringLiteral("data.tar"), false)) {
            dataIndex = i;
            continue;
        }
        if (controlIndex < 0)
            return ModulePtr(new InvalidDebModule(sourceName,
                QStringLiteral("Unexpected member order in Debian package")));
        return ModulePtr(new InvalidDebModule(sourceName,
            QStringLiteral("Expected control.tar before data.tar")));
    }
    if (controlIndex < 0 || dataIndex < 0)
        return ModulePtr(new InvalidDebModule(sourceName,
            QStringLiteral("Missing control.tar or data.tar member")));

    auto module = peare::makeUnique<DebModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::DEB;
    module->info_.description = QStringLiteral("Debian binary package");
    module->resources_ = entries;
    for (ResourceEntry& entry : module->resources_) {
        entry.format = ModuleFormat::DEB;
        entry.type = QStringLiteral("DEB_FILE");
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
