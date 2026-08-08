#include "ParallelsModule.h"
#include "Compat.h"
#include "../fs/ParallelsDisk.h"
#include "../fs/PartitionTable.h"
#include <QFile>
namespace peare {
namespace {
fs::ByteStorePtr fileStore(const QString& p) {
    auto q = std::make_shared<QFile>(p);
    if (!q->open(QIODevice::ReadOnly)) return {};
    qint64 n = q->size();
    uchar* m = n ? q->map(0, n) : nullptr;
    if (m) return std::make_shared<fs::ExternalStore>(m, n, std::static_pointer_cast<void>(q));
    auto b = q->readAll();
    return std::make_shared<fs::MemoryStore>((const std::uint8_t*)b.constData(),
                                             std::size_t(b.size()));
}
} // namespace
ModulePtr ParallelsModule::open(const fs::ByteStorePtr& f, const QString& name) {
    auto m = peare::makeUnique<ParallelsModule>();
    m->info_.filePath = name;
    m->info_.format = ModuleFormat::PARALLELS_HDD;
    m->info_.description = QStringLiteral("Parallels expandable hard disk image");
    std::string e;
    m->disk_ = fs::openParallelsDisk(f, &e);
    if (!m->disk_) {
        m->info_.error = QString::fromStdString(e);
        return ModulePtr(std::move(m));
    }
    auto parts = fs::readPartitionTable(m->disk_);
    auto add = [&](QString n, std::int64_t o, std::int64_t l, fs::ByteStorePtr c = {}) {
        ResourceEntry r;
        r.type = QStringLiteral("DISK_PARTITION");
        r.isEmbeddedFile = true;
        r.name = n;
        r.language = QStringLiteral("neutral");
        r.dataSize = quint64(l);
        r.dataOffset = quint64(o);
        r.format = ModuleFormat::PARALLELS_HDD;
        r.content = c ? c : std::make_shared<fs::SubStore>(m->disk_, o, l);
        m->resources_.push_back(std::move(r));
    };
    if (parts.empty())
        add(QStringLiteral("Whole disk"), 0, m->disk_->capacity());
    else
        for (std::size_t i = 0; i < parts.size(); ++i)
            add(QStringLiteral("Partition %1").arg(i + 1), parts[i].offset, parts[i].length,
                parts[i].content);
    return ModulePtr(std::move(m));
}
ModulePtr ParallelsModule::open(const QString& p) {
    auto f = fileStore(p);
    if (!f) {
        auto m = peare::makeUnique<ParallelsModule>();
        m->info_.filePath = p;
        m->info_.format = ModuleFormat::PARALLELS_HDD;
        m->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(m));
    }
    return open(f, p);
}
} // namespace peare
