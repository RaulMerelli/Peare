#include "WadModule.h"
#include "Compat.h"
#include <QFile>
#include <cstring>
namespace peare {
namespace {
fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(reinterpret_cast<const std::uint8_t*>(mapped),
                                                   std::int64_t(size),
                                                   std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}
quint32 le32(const std::uint8_t* p) {
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
QString lumpName(const std::uint8_t* p) {
    QByteArray a(reinterpret_cast<const char*>(p), 8);
    int z = a.indexOf('\0');
    if (z >= 0) a.truncate(z);
    return QString::fromLatin1(a).trimmed();
}
} // namespace
ModulePtr WadModule::open(const QString& path) {
    return open(storeForFile(path), path);
}
ModulePtr WadModule::open(const QByteArray& d, const QString& n) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(d.constData()), std::size_t(d.size())),
                n);
}
ModulePtr WadModule::open(const fs::ByteStorePtr& f, const QString& n) {
    auto m = peare::makeUnique<WadModule>();
    m->file_ = f;
    m->info_.filePath = n;
    m->info_.format = ModuleFormat::WAD;
    if (!f || f->capacity() < 12) {
        m->info_.error = "Truncated WAD header";
        return ModulePtr(std::move(m));
    }
    auto h = f->readRange(0, 12);
    if (h.size() != 12 || (std::memcmp(h.data(), "IWAD", 4) && std::memcmp(h.data(), "PWAD", 4))) {
        m->info_.error = "Invalid WAD signature";
        return ModulePtr(std::move(m));
    }
    quint32 count = le32(h.data() + 4), dir = le32(h.data() + 8);
    quint64 bytes = quint64(count) * 16;
    if (count > 1000000 || dir > quint64(f->capacity()) || bytes > quint64(f->capacity()) - dir) {
        m->info_.error = "Invalid WAD directory";
        return ModulePtr(std::move(m));
    }
    auto table = f->readRange(dir, bytes);
    if (table.size() != bytes) {
        m->info_.error = "Truncated WAD directory";
        return ModulePtr(std::move(m));
    }
    for (quint32 i = 0; i < count; ++i) {
        const auto* e = table.data() + i * 16;
        quint32 off = le32(e), sz = le32(e + 4);
        if (off > quint64(f->capacity()) || sz > quint64(f->capacity()) - off) {
            m->info_.error = QStringLiteral("Invalid WAD lump %1").arg(i);
            m->resources_.clear();
            return ModulePtr(std::move(m));
        }
        ResourceEntry r;
        r.type = "WAD_LUMP";
        r.name = lumpName(e + 8);
        if (r.name.isEmpty()) r.name = QStringLiteral("LUMP_%1").arg(i);
        r.dataOffset = off;
        r.dataSize = sz;
        r.baseId = int(i);
        r.format = ModuleFormat::WAD;
        r.isEmbeddedFile = sz > 0;
        r.content = std::make_shared<fs::SubStore>(f, off, sz);
        m->resources_.push_back(std::move(r));
    }
    m->info_.description = QStringLiteral("%1 — %2 lumps")
                               .arg(std::memcmp(h.data(), "IWAD", 4) ? "Doom patch WAD (PWAD)"
                                                                     : "Doom internal WAD (IWAD)")
                               .arg(count);
    return ModulePtr(std::move(m));
}
} // namespace peare
