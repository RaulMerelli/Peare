#include "LivePirsModule.h"
#include "Compat.h"
#include "stfs/STFSCommon.h"
#include <QFile>
namespace peare {
std::unique_ptr<LivePirsModule> LivePirsModule::open(const QString& filePath) {
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) {
        auto m = peare::makeUnique<LivePirsModule>();
        m->info_.filePath = filePath;
        m->info_.format = ModuleFormat::LIVE_PIRS;
        m->info_.error = f.errorString();
        return m;
    }
    return open(f.readAll(), filePath);
}
std::unique_ptr<LivePirsModule> LivePirsModule::open(const QByteArray& data,
                                                     const QString& logicalName) {
    auto m = peare::makeUnique<LivePirsModule>();
    m->info_.filePath = logicalName;
    m->info_.format = ModuleFormat::LIVE_PIRS;
    const auto p = stfs::parse(data);
    if (!p.valid ||
        (p.signature != QLatin1String("LIVE") && p.signature != QLatin1String("PIRS"))) {
        m->info_.error =
            p.error.isEmpty() ? QStringLiteral("Invalid LIVE/PIRS container") : p.error;
        return m;
    }
    m->info_.description = p.description;
    stfs::populateResources(p, data, logicalName, ModuleFormat::LIVE_PIRS, &m->resources_);
    return m;
}
} // namespace peare
