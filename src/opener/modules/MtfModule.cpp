#include "MtfModule.h"
#include "Compat.h"
#include <QFile>
#include <QHash>
#include <QStringList>
#include <algorithm>
#include <cstring>
#include <utility>
namespace peare {
namespace {
quint16 le16(const std::uint8_t* p) {
    return quint16(p[0]) | (quint16(p[1]) << 8);
}
quint32 le32(const std::uint8_t* p) {
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
quint64 le64(const std::uint8_t* p) {
    return quint64(le32(p)) | (quint64(le32(p + 4)) << 32);
}
quint64 alignUp(quint64 v, quint64 a) {
    return a ? ((v + a - 1) / a) * a : v;
}
fs::ByteStorePtr storeForFile(const QString& p) {
    auto h = std::make_shared<QFile>(p);
    if (!h->open(QIODevice::ReadOnly)) return {};
    qint64 s = h->size();
    uchar* m = s > 0 ? h->map(0, s) : nullptr;
    if (m)
        return std::make_shared<fs::ExternalStore>(reinterpret_cast<const std::uint8_t*>(m), s,
                                                   std::static_pointer_cast<void>(h));
    QByteArray b = h->readAll();
    return std::make_shared<fs::MemoryStore>(reinterpret_cast<const std::uint8_t*>(b.constData()),
                                             std::size_t(b.size()));
}
bool tag(const std::vector<std::uint8_t>& b, const char* s) {
    return b.size() >= 4 && !std::memcmp(b.data(), s, 4);
}
bool known(const std::vector<std::uint8_t>& b) {
    static const char* t[] = {"TAPE", "SSET", "VOLB", "DIRB", "FILE",
                              "CFIL", "ESPB", "ESET", "EOTM", "SFMB"};
    for (auto x : t)
        if (tag(b, x)) return true;
    return false;
}
QString decodeString(const fs::ByteStorePtr& f, quint64 base, const std::vector<std::uint8_t>& h,
                     int aoff, quint8 st) {
    if (aoff + 4 > int(h.size())) return {};
    quint16 size = le16(h.data() + aoff), off = le16(h.data() + aoff + 2);
    if (!size || base + off > quint64(f->capacity()) ||
        size > quint64(f->capacity()) - (base + off))
        return {};
    auto v = f->readRange(base + off, size);
    QByteArray a(reinterpret_cast<const char*>(v.data()), int(v.size()));
    bool u = st == 1;
    if (!u) {
        int z = 0;
        for (int i = 1; i < a.size(); i += 2)
            if (a.at(i) == '\0') ++z;
        u = a.size() > 3 && z * 3 > a.size() / 2;
    }
    QString s = u ? QString::fromUtf16(reinterpret_cast<const ushort*>(a.constData()), a.size() / 2)
                  : QString::fromLocal8Bit(a);
    s.replace(QChar(0), QChar('/'));
    while (s.endsWith('/')) s.chop(1);
    while (s.startsWith('/')) s.remove(0, 1);
    return s;
}
QStringList parts(QString s) {
    s.replace('\\', '/');
    return s.split('/', Qt::SkipEmptyParts);
}
} // namespace
ModulePtr MtfModule::open(const QString& p) {
    return open(storeForFile(p), p);
}
ModulePtr MtfModule::open(const QByteArray& d, const QString& n) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(d.constData()), std::size_t(d.size())),
                n);
}
ModulePtr MtfModule::open(const fs::ByteStorePtr& f, const QString& n) {
    auto m = peare::makeUnique<MtfModule>();
    m->info_.filePath = n;
    m->info_.format = ModuleFormat::MTF;
    m->info_.description = "Microsoft Tape Format / NTBackup BKF";
    if (!f || f->capacity() < 52) {
        m->info_.error = "Truncated MTF header";
        return ModulePtr(std::move(m));
    }
    m->file_ = f;
    auto th = f->readRange(0, std::min<std::int64_t>(512, f->capacity()));
    if (!tag(th, "TAPE") || le64(th.data() + 20) != 0) {
        m->info_.error = "Invalid MTF TAPE descriptor";
        return ModulePtr(std::move(m));
    }
    quint64 flb = th.size() >= 88 ? le16(th.data() + 86) : 512;
    if (flb != 512 && flb != 1024) flb = 512;
    QHash<quint32, QString> dirs;
    QString volume;
    int count = 0;
    quint64 pos = 0;
    while (pos + 52 <= quint64(f->capacity())) {
        auto h = f->readRange(pos, std::min<quint64>(flb, quint64(f->capacity()) - pos));
        if (h.size() < 52) break;
        if (!known(h)) {
            pos += flb;
            continue;
        }
        quint16 first = le16(h.data() + 8);
        quint8 st = h[48];
        if (tag(h, "VOLB") && h.size() >= 69) {
            QString v = decodeString(f, pos, h, 60, st);
            if (!v.isEmpty()) volume = v;
        } else if (tag(h, "DIRB") && h.size() >= 84) {
            dirs[le32(h.data() + 76)] = decodeString(f, pos, h, 80, st);
        }
        QString fn;
        quint32 did = 0;
        if (tag(h, "FILE") && h.size() >= 88) {
            did = le32(h.data() + 76);
            fn = decodeString(f, pos, h, 84, st);
        }
        quint64 cur = pos + first;
        if (first < 52 || cur + 22 > quint64(f->capacity())) {
            pos += flb;
            continue;
        }
        bool spad = false;
        while (cur + 22 <= quint64(f->capacity())) {
            auto sh = f->readRange(cur, 22);
            QByteArray id(reinterpret_cast<const char*>(sh.data()), 4);
            quint64 len = le64(sh.data() + 8), dataOff = cur + 22;
            quint16 enc = le16(sh.data() + 16), comp = le16(sh.data() + 18);
            if (dataOff > quint64(f->capacity()) || len > quint64(f->capacity()) - dataOff) {
                m->info_.error = QStringLiteral("Truncated MTF stream at 0x%1").arg(cur, 0, 16);
                return ModulePtr(std::move(m));
            }
            if (id == "FNAM" && fn.isEmpty()) {
                auto v = f->readRange(dataOff, std::min<quint64>(len, 65535));
                QByteArray a(reinterpret_cast<const char*>(v.data()), int(v.size()));
                fn = QString::fromUtf16(reinterpret_cast<const ushort*>(a.constData()),
                                        a.size() / 2);
                fn.replace(QChar(0), QChar('/'));
                while (fn.endsWith('/')) fn.chop(1);
            }
            if (id == "STAN" && !fn.isEmpty()) {
                QString full = dirs.value(did);
                if (!full.isEmpty()) full += '/';
                full += fn;
                QStringList pp = parts(full), hp;
                if (!volume.isEmpty()) hp << volume;
                QString leaf =
                    pp.isEmpty() ? QStringLiteral("file_%1.bin").arg(count) : pp.takeLast();
                hp += pp;
                ResourceEntry r;
                r.type = (enc || comp) ? "MTF_ENCODED_FILE" : "MTF_FILE";
                r.name = leaf;
                r.hierarchyPath = hp;
                r.dataOffset = dataOff;
                r.dataSize = len;
                r.format = ModuleFormat::MTF;
                r.isEmbeddedFile = true;
                r.content = std::make_shared<fs::SubStore>(f, dataOff, len);
                m->resources_.push_back(std::move(r));
                ++count;
            }
            cur = alignUp(dataOff + len, 4);
            if (id == "SPAD") {
                spad = true;
                break;
            }
        }
        pos = spad ? alignUp(cur, flb) : pos + flb;
    }
    m->info_.description =
        QStringLiteral("Microsoft Tape Format / NTBackup BKF — %1 files").arg(count);
    return ModulePtr(std::move(m));
}
} // namespace peare
