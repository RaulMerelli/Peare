#include "EAppxModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>
#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

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
bool rangeOk(quint64 off, quint64 len, quint64 cap) {
    return off <= cap && len <= cap - off;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(reinterpret_cast<const std::uint8_t*>(mapped),
                                                   std::int64_t(size),
                                                   std::static_pointer_cast<void>(holder));
    const QByteArray b = holder->readAll();
    return std::make_shared<fs::MemoryStore>(reinterpret_cast<const std::uint8_t*>(b.constData()),
                                             std::size_t(b.size()));
}
class StoreInput final : public compression::InflateInput {
public:
    StoreInput(const fs::IByteStore& s, qint64 o, qint64 n) : s_(s), o_(o), n_(n) {}
    bool readByte(std::uint8_t* v) override {
        if (!v || p_ >= n_) return false;
        return s_.read(o_ + p_++, v, 1) == 1;
    }
    std::size_t consumed() const override { return std::size_t(p_); }

private:
    const fs::IByteStore& s_;
    qint64 o_, n_, p_ = 0;
};
class DeflateStore final : public fs::IByteStore {
public:
    DeflateStore(fs::ByteStorePtr s, qint64 o, qint64 c, qint64 u)
        : s_(std::move(s)), o_(o), c_(c), u_(u) {}
    std::int64_t capacity() const override { return u_; }
    bool cheapRandomAccess() const override { return false; }
    int read(std::int64_t p, std::uint8_t* d, int n) const override {
        if (!s_ || p < 0 || n <= 0 || p >= u_) return 0;
        qint64 e = std::min<qint64>(u_, p + n);
        std::lock_guard<std::mutex> g(m_);
        if (!ensure(std::size_t(e))) return 0;
        int z = int(e - p);
        std::copy(buf_.begin() + p, buf_.begin() + p + z, d);
        return z;
    }

private:
    bool ensure(std::size_t want) const {
        if (fail_) return false;
        if (buf_.size() >= want) return true;
        StoreInput in(*s_, o_, c_);
        std::vector<std::uint8_t> out;
        std::string err;
        bool ok = want == std::size_t(u_)
                      ? compression::inflateRawExact(in, want, nullptr, &out, &err)
                      : compression::inflateRawPrefix(in, want, &out, &err);
        if (!ok) {
            fail_ = true;
            return false;
        }
        buf_.swap(out);
        return true;
    }
    fs::ByteStorePtr s_;
    qint64 o_, c_, u_;
    mutable std::mutex m_;
    mutable std::vector<std::uint8_t> buf_;
    mutable bool fail_ = false;
};
struct Part {
    qint16 flags = 0, zipped = 0;
    qint32 id = 0;
    quint64 pos = 0, orig = 0, len = 0;
    QString path;
    bool package = false;
};
QByteArray readAll(const fs::ByteStorePtr& s, quint64 o, quint64 n, bool zipped, quint64 orig) {
    if (!s || !rangeOk(o, n, quint64(s->capacity())) || orig > 64 * 1024 * 1024ULL) return {};
    if (!zipped) {
        auto v = s->readRange(qint64(o), qint64(n));
        return QByteArray(reinterpret_cast<const char*>(v.data()), int(v.size()));
    }
    StoreInput in(*s, qint64(o), qint64(n));
    std::vector<std::uint8_t> out;
    std::string err;
    if (!compression::inflateRawExact(in, std::size_t(orig), nullptr, &out, &err)) return {};
    return QByteArray(reinterpret_cast<const char*>(out.data()), int(out.size()));
}
QString cleanPath(QString p) {
    p.replace('\\', '/');
    while (p.startsWith('/')) p.remove(0, 1);
    while (p.contains("//")) p.replace("//", "/");
    if (p.contains("../") || p == "..") return {};
    return p;
}
void addResource(QVector<ResourceEntry>& out, const fs::ByteStorePtr& store, quint64 base,
                 const Part& p, const QStringList& root) {
    QString path = cleanPath(p.path);
    if (path.isEmpty()) path = QStringLiteral("part_%1.dat").arg(p.id, 8, 16, QLatin1Char('0'));
    QStringList bits = path.split('/', Qt::SkipEmptyParts);
    if (bits.isEmpty()) return;
    ResourceEntry r;
    r.name = bits.takeLast();
    r.hierarchyPath = root + bits;
    r.type = QStringLiteral("EAPPX_FILE");
    r.language =
        (p.flags == 0 ? QStringLiteral("encrypted payload") : QStringLiteral("clear payload"));
    if (p.zipped) r.language += QStringLiteral("; deflated");
    r.dataOffset = base + p.pos;
    r.dataSize = p.orig;
    r.format = ModuleFormat::EAPPX;
    r.isEmbeddedFile = true;
    if (p.zipped)
        r.content = std::make_shared<DeflateStore>(store, qint64(base + p.pos), qint64(p.len),
                                                   qint64(p.orig));
    else
        r.content = std::make_shared<fs::SubStore>(store, qint64(base + p.pos), qint64(p.len));
    out.push_back(std::move(r));
}
bool parseContainer(const fs::ByteStorePtr& store, quint64 base, QVector<ResourceEntry>& out,
                    QStringList root, QString* error, int depth = 0) {
    if (!store || depth > 8 || !rangeOk(base, 6, quint64(store->capacity()))) {
        if (error) *error = "Invalid EAppX range";
        return false;
    }
    auto hv = store->readRange(qint64(base), 6);
    if (hv.size() != 6) {
        if (error) *error = "Truncated EAppX header";
        return false;
    }
    QByteArray magic(reinterpret_cast<const char*>(hv.data()), 4);
    bool bundle = magic == "EXBH";
    if (!bundle && magic != "EXPH" && magic != "EXSH") {
        if (error) *error = "Invalid EAppX magic";
        return false;
    }
    quint16 hs = le16(hv.data() + 4);
    if (hs < 80 || hs > 1024 * 1024 || !rangeOk(base, hs, quint64(store->capacity()))) {
        if (error) *error = "Invalid EAppX header size";
        return false;
    }
    auto h = store->readRange(qint64(base), hs);
    const std::uint8_t* p = h.data();
    quint64 pos = 6;
    auto need = [&](quint64 n) { return pos <= quint64(h.size()) && n <= quint64(h.size()) - pos; };
    auto r16 = [&]() {
        quint16 v = le16(p + pos);
        pos += 2;
        return v;
    };
    auto r32 = [&]() {
        quint32 v = le32(p + pos);
        pos += 4;
        return v;
    };
    auto r64 = [&]() {
        quint64 v = le64(p + pos);
        pos += 8;
        return v;
    };
    if (!need(8 * 7 + 2 + 4)) {
        if (error) *error = "Truncated EAppX fixed header";
        return false;
    }
    quint64 version = r64();
    quint64 footerOff = r64(), footerLen = r64(), footerCount = r64();
    quint64 sigOff = r64();
    quint16 sigZip = r16();
    quint32 sigOrig = r32(), sigLen = r32();
    quint64 catOff = r64();
    quint16 catZip = r16();
    quint32 catOrig = r32(), catLen = r32();
    (void)r64();
    (void)r32();
    quint16 hashCount = r16();
    if (!need(quint64(hashCount) * 32 + 2)) {
        if (error) *error = "Truncated EAppX hash list";
        return false;
    }
    pos += quint64(hashCount) * 32;
    (void)r16();
    auto skipUtf16 = [&](QString* value) -> bool {
        if (!need(2)) return false;
        quint16 bytes = r16();
        if (!need(bytes) || bytes % 2) return false;
        if (value) *value = QString::fromUtf16(reinterpret_cast<const ushort*>(p + pos), bytes / 2);
        pos += bytes;
        return true;
    };
    QString packageName, algo, hashMethod;
    if (!skipUtf16(&packageName) || !skipUtf16(&algo) || !need(2)) {
        if (error) *error = "Truncated EAppX string header";
        return false;
    }
    (void)r16();
    if (!skipUtf16(&hashMethod) || !need(2)) {
        if (error) *error = "Truncated EAppX metadata";
        return false;
    }
    quint16 blob = r16();
    if (!need(blob)) {
        if (error) *error = "Truncated EAppX metadata blob";
        return false;
    }
    pos += blob;
    if (!rangeOk(base + footerOff, footerLen, quint64(store->capacity())) || footerLen % 40) {
        if (error) *error = "Invalid EAppX footer";
        return false;
    }
    auto f = store->readRange(qint64(base + footerOff), qint64(footerLen));
    QVector<Part> parts;
    for (quint64 i = 0; i < footerLen / 40; i++) {
        const std::uint8_t* e = f.data() + i * 40;
        Part q;
        q.flags = qint16(le16(e + 4));
        q.zipped = qint16(le16(e + 6));
        q.id = qint32(le32(e + 8));
        q.pos = le64(e + 16);
        q.orig = le64(e + 24);
        q.len = le64(e + 32);
        if (!rangeOk(base + q.pos, q.len, quint64(store->capacity()))) {
            if (error) *error = "Invalid EAppX part range";
            return false;
        }
        q.path = QStringLiteral("part_%1.dat").arg(i);
        parts.push_back(q);
    }
    if (parts.isEmpty()) {
        if (error) *error = "Empty EAppX footer";
        return false;
    }
    Part& bm = parts.last();
    bm.path = "AppxBlockMap.xml";
    QByteArray xml = readAll(store, base + bm.pos, bm.len, bm.zipped == 1, bm.orig);
    QXmlStreamReader xr(xml);
    while (!xr.atEnd()) {
        xr.readNext();
        if (xr.isStartElement() &&
            xr.name().compare(QStringLiteral("File"), Qt::CaseInsensitive) == 0) {
            auto a = xr.attributes();
            bool ok = false;
            int id = a.value("Id").toInt(&ok, 16);
            QString n = a.value("Name").toString();
            if (ok && !n.isEmpty())
                for (Part& z : parts)
                    if (z.id == id) {
                        z.path = n;
                        break;
                    }
        }
    }
    if (bundle) {
        for (Part& z : parts)
            if (cleanPath(z.path).compare("AppxMetadata/AppxBundleManifest.xml",
                                          Qt::CaseInsensitive) == 0) {
                QByteArray bx = readAll(store, base + z.pos, z.len, z.zipped == 1, z.orig);
                QXmlStreamReader br(bx);
                while (!br.atEnd()) {
                    br.readNext();
                    if (br.isStartElement() &&
                        br.name().compare(QStringLiteral("Package"), Qt::CaseInsensitive) == 0) {
                        auto a = br.attributes();
                        bool ok = false;
                        quint64 off = a.value("Offset").toULongLong(&ok);
                        QString fn = a.value("FileName").toString();
                        if (ok && !fn.isEmpty())
                            for (Part& t : parts)
                                if (t.pos == off) {
                                    t.path = fn;
                                    t.package = true;
                                    break;
                                }
                    }
                }
                break;
            }
    }
    if (sigOff && sigLen && rangeOk(base + sigOff, sigLen, quint64(store->capacity()))) {
        Part s;
        s.flags = 1;
        s.zipped = sigZip;
        s.pos = sigOff;
        s.orig = sigOrig;
        s.len = sigLen;
        s.path = "AppxSignature.p7x";
        addResource(out, store, base, s, root);
    }
    if (catOff && catLen && rangeOk(base + catOff, catLen, quint64(store->capacity()))) {
        Part c;
        c.flags = 1;
        c.zipped = catZip;
        c.pos = catOff;
        c.orig = catOrig;
        c.len = catLen;
        c.path = "AppxMetadata/CodeIntegrity.cat";
        addResource(out, store, base, c, root);
    }
    for (const Part& z : parts) {
        if (z.package) {
            QStringList nr = root;
            QString d = QFileInfo(cleanPath(z.path)).completeBaseName();
            if (!d.isEmpty()) nr << d;
            QString nestedErr;
            if (!parseContainer(store, base + z.pos, out, nr, &nestedErr, depth + 1)) {
                Part copy = z;
                addResource(out, store, base, copy, root);
            }
        } else
            addResource(out, store, base, z, root);
    }
    Q_UNUSED(version);
    Q_UNUSED(footerCount);
    Q_UNUSED(packageName);
    Q_UNUSED(algo);
    Q_UNUSED(hashMethod);
    return true;
}
ModuleFormat formatForName(const QString& n, const QByteArray& m) {
    QString s = QFileInfo(n).suffix().toLower();
    if (m == "EXBH")
        return s.contains("msix") ? ModuleFormat::EMSIXBUNDLE : ModuleFormat::EAPPXBUNDLE;
    return s.contains("msix") ? ModuleFormat::EMSIX : ModuleFormat::EAPPX;
}
} // namespace
bool EAppxModule::isHeader(const QByteArray& d) {
    return d.startsWith("EXPH") || d.startsWith("EXBH") || d.startsWith("EXSH");
}
ModulePtr EAppxModule::open(const QString& p) {
    return open(storeForFile(p), p);
}
ModulePtr EAppxModule::open(const QByteArray& d, const QString& n) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(d.constData()), std::size_t(d.size())),
                n);
}
ModulePtr EAppxModule::open(const fs::ByteStorePtr& f, const QString& n) {
    auto m = peare::makeUnique<EAppxModule>();
    m->file_ = f;
    m->info_.filePath = n;
    if (!f || f->capacity() < 6) {
        m->info_.error = "Cannot open encrypted AppX package";
        return ModulePtr(std::move(m));
    }
    auto h = f->readRange(0, 6);
    QByteArray mg(reinterpret_cast<const char*>(h.data()), 4);
    m->info_.format = formatForName(n, mg);
    QString err;
    if (!parseContainer(f, 0, m->resources_, {}, &err)) {
        m->info_.error = err;
        return ModulePtr(std::move(m));
    }
    m->info_.description =
        QStringLiteral(
            "Encrypted AppX/MSIX container — %1 mapped payloads; payload encryption preserved")
            .arg(m->resources_.size());
    return ModulePtr(std::move(m));
}
} // namespace peare
