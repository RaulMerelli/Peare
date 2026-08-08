#include "MdfMdsModule.h"
#include "Compat.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <cstring>
#include <cstdint>
#include <memory>
#include <utility>
namespace peare {
namespace {
fs::ByteStorePtr storeForFile(const QString& path) {
    auto h = std::make_shared<QFile>(path);
    if (!h->open(QIODevice::ReadOnly)) return nullptr;
    qint64 n = h->size();
    uchar* p = n > 0 ? h->map(0, n) : nullptr;
    if (p)
        return std::make_shared<fs::ExternalStore>(reinterpret_cast<const std::uint8_t*>(p), n,
                                                   std::static_pointer_cast<void>(h));
    QByteArray b = h->readAll();
    return std::make_shared<fs::MemoryStore>(reinterpret_cast<const std::uint8_t*>(b.constData()),
                                             std::size_t(b.size()));
}
class SectorStore final : public fs::IByteStore {
public:
    SectorStore(fs::ByteStorePtr p, qint64 raw, qint64 off) : p_(std::move(p)), r_(raw), o_(off) {}
    std::int64_t capacity() const override { return p_ ? (p_->capacity() / r_) * 2048 : 0; }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!p_ || pos < 0 || count <= 0 || pos >= capacity()) return 0;
        int want = int(std::min<std::int64_t>(count, capacity() - pos)), done = 0;
        while (done < want) {
            auto q = pos + done, sec = q / 2048, in = q % 2048;
            int n = int(std::min<std::int64_t>(2048 - in, want - done));
            int g = p_->read(sec * r_ + o_ + in, dst + done, n);
            if (g <= 0) break;
            done += g;
        }
        return done;
    }

private:
    fs::ByteStorePtr p_;
    qint64 r_, o_;
};
bool isoAt(const fs::ByteStorePtr& s, qint64 raw, qint64 off) {
    std::uint8_t b[5];
    return s && s->read(16 * raw + off + 1, b, 5) == 5 && std::memcmp(b, "CD001", 5) == 0;
}
QString companion(const QString& p) {
    QFileInfo fi(p);
    QString base = fi.dir().filePath(fi.completeBaseName());
    QString a = base + QStringLiteral(".mdf");
    if (QFileInfo::exists(a)) return a;
    a = base + QStringLiteral(".MDF");
    return QFileInfo::exists(a) ? a : QString();
}
} // namespace
ModulePtr MdfMdsModule::open(const QString& path) {
    auto m = peare::makeUnique<MdfMdsModule>();
    m->info_.filePath = path;
    m->info_.format = ModuleFormat::MDF_MDS;
    QString dataPath = path;
    QFileInfo fi(path);
    if (fi.suffix().compare("mds", Qt::CaseInsensitive) == 0) {
        QFile d(path);
        if (!d.open(QIODevice::ReadOnly) || d.read(16) != "MEDIA DESCRIPTOR") {
            m->info_.error = "Invalid MDS signature";
            return ModulePtr(std::move(m));
        }
        dataPath = companion(path);
        if (dataPath.isEmpty()) {
            m->info_.error = "Matching MDF data file not found";
            return ModulePtr(std::move(m));
        }
    }
    auto raw = storeForFile(dataPath);
    if (!raw) {
        m->info_.error = "Cannot open MDF data file";
        return ModulePtr(std::move(m));
    }
    m->stores_.push_back(raw);
    qint64 sector = 0, offset = 0;
    if (raw->capacity() % 2048 == 0 && isoAt(raw, 2048, 0)) {
        sector = 2048;
        offset = 0;
    } else if (raw->capacity() % 2352 == 0 && isoAt(raw, 2352, 16)) {
        sector = 2352;
        offset = 16;
    } else if (raw->capacity() % 2448 == 0 && isoAt(raw, 2448, 16)) {
        sector = 2448;
        offset = 16;
    } else if (raw->capacity() % 2352 == 0) {
        sector = 2352;
        offset = 16;
    } else if (raw->capacity() % 2448 == 0) {
        sector = 2448;
        offset = 16;
    } else if (raw->capacity() % 2048 == 0) {
        sector = 2048;
        offset = 0;
    } else {
        m->info_.error = "Unsupported MDF sector layout";
        return ModulePtr(std::move(m));
    }
    fs::ByteStorePtr logical =
        (sector == 2048) ? raw : std::make_shared<SectorStore>(raw, sector, offset);
    m->stores_.push_back(logical);
    ResourceEntry r;
    r.type = "MDF_TRACK";
    r.name = "Track 01";
    r.dataSize = logical->capacity();
    r.format = ModuleFormat::MDF_MDS;
    r.isEmbeddedFile = true;
    r.content = logical;
    m->resources_.push_back(r);
    m->info_.description =
        QStringLiteral("Alcohol/DAEMON MDF/MDS optical image — %1-byte sectors").arg(sector);
    return ModulePtr(std::move(m));
}
} // namespace peare
