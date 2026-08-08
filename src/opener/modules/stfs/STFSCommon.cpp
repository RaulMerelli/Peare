#include "STFSCommon.h"

#include <QFileInfo>
#include <QMap>
#include <QStringList>
#include <QtEndian>
#include <utility>

namespace peare {
namespace stfs {
namespace {
quint16 be16(const QByteArray& d, int o) {
    return o >= 0 && o + 2 <= d.size()
               ? qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + o))
               : 0;
}
quint32 be32(const QByteArray& d, int o) {
    return o >= 0 && o + 4 <= d.size()
               ? qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + o))
               : 0;
}
quint16 le16(const QByteArray& d, int o) {
    return o >= 0 && o + 2 <= d.size()
               ? qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(d.constData() + o))
               : 0;
}
quint32 le24(const QByteArray& d, int o) {
    return o >= 0 && o + 3 <= d.size() ? quint32(uchar(d[o])) | (quint32(uchar(d[o + 1])) << 8) |
                                             (quint32(uchar(d[o + 2])) << 16)
                                       : 0;
}
QString cleanUtf16Be(const QByteArray& d, int o, int bytes) {
    if (o < 0 || bytes < 0 || o + bytes > d.size()) return {};
    QVector<ushort> u;
    u.reserve(bytes / 2);
    for (int i = 0; i + 1 < bytes; i += 2) {
        const quint16 c = be16(d, o + i);
        if (!c || c == 0xFFFF) continue;
        u.push_back(c);
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return QString::fromUtf16(reinterpret_cast<const char16_t*>(u.constData()), u.size()).trimmed();
#else
    return QString::fromUtf16(u.constData(), u.size()).trimmed();
#endif
}
QString safePath(QString n) {
    n.replace('\\', '/');
    while (n.startsWith('/')) n.remove(0, 1);
    QStringList out;
    for (const QString& part : n.split('/', Qt::SkipEmptyParts)) {
        if (part == QStringLiteral(".")) continue;
        out.push_back(part == QStringLiteral("..") ? QStringLiteral("_") : part);
    }
    return out.join('/');
}
QString leafName(const QString& path) {
    const int p = path.lastIndexOf('/');
    return p < 0 ? path : path.mid(p + 1);
}
QStringList parentPath(const QString& path) {
    QStringList p = path.split('/', Qt::SkipEmptyParts);
    if (!p.isEmpty()) p.removeLast();
    return p;
}
quint64 clusterSkip(quint32 cluster, quint32 offset) {
    quint64 r = 0;
    while (cluster >= 170) {
        cluster /= 170;
        r += (quint64(cluster) + 1u) * offset;
    }
    return r;
}
bool readStfsFile(const QByteArray& d, quint32 start, quint32 offset, quint32 cluster,
                  quint32 length, QByteArray* out, quint64* firstOffset) {
    out->clear();
    if (firstOffset) *firstOffset = 0;
    if (!length) return true;
    if (cluster < 1) return false;
    out->reserve(int(qMin<quint32>(length, 0x7fffffff)));
    quint32 left = length, current = cluster;
    quint64 advertised = quint64(cluster) * 0x1000u + start;
    while (left) {
        const quint64 pos = advertised + clusterSkip(current, offset);
        if (out->isEmpty() && firstOffset) *firstOffset = pos;
        const quint32 take = qMin<quint32>(0x1000, left);
        if (pos + take > quint64(d.size())) return false;
        out->append(d.constData() + qsizetype(pos), int(take));
        ++current;
        advertised += 0x1000;
        left -= take;
    }
    return true;
}
} // namespace

ParsedContainer parse(const QByteArray& d, const QString& expectedSignature) {
    ParsedContainer out;
    if (d.size() < 4) {
        out.error = QStringLiteral("Input too small");
        return out;
    }
    const QString sig = QString::fromLatin1(d.left(4));
    if (sig != QLatin1String("LIVE") && sig != QLatin1String("PIRS") &&
        sig != QLatin1String("CON ")) {
        out.error = QStringLiteral("Unsupported STFS signature");
        return out;
    }
    if (!expectedSignature.isEmpty() && sig != expectedSignature) {
        out.error = QStringLiteral("Unexpected STFS signature");
        return out;
    }
    if (d.size() < 0xD000) {
        out.error = QStringLiteral("Truncated %1 container").arg(sig.trimmed());
        return out;
    }
    const bool con = sig == QLatin1String("CON ");
    const int pngStop = con ? 0xA000 : 0xB000;
    const quint32 contentType = be32(d, 0x340);
    int start = 0xC000;
    if (!con && be16(d, 0xC032) != 0xFFFF) start = 0xD000;
    QString t;
    t += QStringLiteral("Format: %1\nContent type: 0x%2\nDirectory data: 0x%3\n")
             .arg(sig.trimmed())
             .arg(contentType, 8, 16, QLatin1Char('0'))
             .arg(start, 0, 16);
    static const char* langs[] = {"English", "Japanese", "German",  "French",    "Spanish",
                                  "Italian", "Korean",   "Chinese", "Portuguese"};
    for (int group = 0; group < 2; ++group) {
        t += group ? QStringLiteral("\nDescriptions:\n") : QStringLiteral("\nTitles:\n");
        const int base = 0x410 + group * 9 * 0x100;
        for (int i = 0; i < 9; ++i) {
            const QString v = cleanUtf16Be(d, base + i * 0x100, 0x100);
            if (!v.isEmpty())
                t += QString::fromLatin1(langs[i]) + QStringLiteral(": ") + v + QLatin1Char('\n');
        }
    }
    const quint32 png1 = be32(d, 0x1712), png2 = be32(d, 0x1716);
    if (png1 && quint64(0x171A) + png1 <= quint64(d.size())) {
        ParsedFile icon;
        icon.path = QStringLiteral("icon1.png");
        icon.dataOffset = 0x171A;
        icon.data = d.mid(0x171A, int(png1));
        out.icons.push_back(icon);
    }
    if (png2 && quint64(0x571A) + png2 <= quint64(pngStop)) {
        ParsedFile icon;
        icon.path = QStringLiteral("icon2.png");
        icon.dataOffset = 0x571A;
        icon.data = d.mid(0x571A, int(png2));
        out.icons.push_back(icon);
    }
    if (start + 0x31 > d.size()) {
        out.error = QStringLiteral("Directory header outside container");
        return out;
    }
    const quint16 first = le16(d, start + 0x2F);
    const quint64 dirBytes = quint64(first) * 0x1000u;
    if (!first || quint64(start) + dirBytes > quint64(d.size())) {
        out.error = QStringLiteral("Invalid directory span");
        return out;
    }
    const quint32 off = (!con && start == 0xC000) ? 0x1000 : 0x2000;
    QMap<int, QString> paths;
    paths.insert(0xFFFF, QString());
    const int count = int(dirBytes / 64u);
    for (int i = 0; i < count; ++i) {
        const int o = start + i * 64;
        const quint8 nl = quint8(d[o + 40]);
        if (!nl) break;
        const int n = nl & 0x3F;
        if (n < 1 || n > 40) continue;
        const QString name = QString::fromLatin1(d.constData() + o, n);
        const quint32 clusters = le24(d, o + 41), clusters2 = le24(d, o + 44), cl = le24(d, o + 47);
        const quint16 parent = be16(d, o + 50);
        const quint32 len = be32(d, o + 52);
        if (clusters != clusters2 || !paths.contains(parent)) continue;
        const QString full = safePath(paths.value(parent) + name);
        const bool dir = (nl & 0x80) != 0;
        if (dir) {
            paths.insert(i, full + QLatin1Char('/'));
            t += QStringLiteral("DIR  ") + full + QLatin1Char('\n');
            continue;
        }
        t += QStringLiteral("FILE ") + full + QStringLiteral(" (%1 bytes)\n").arg(len);
        if (len > quint64(clusters) * 0x1000u) continue;
        QByteArray bytes;
        quint64 dataOffset = 0;
        if (readStfsFile(d, start, off, cl, len, &bytes, &dataOffset)) {
            ParsedFile file;
            file.path = full;
            file.dataOffset = dataOffset;
            file.data = bytes;
            out.files.push_back(file);
        }
    }
    out.valid = true;
    out.signature = sig;
    out.description = con ? QStringLiteral("Xbox 360 CON STFS container")
                          : QStringLiteral("Xbox 360 %1 STFS container").arg(sig);
    out.metadataText = t;
    return out;
}

void populateResources(const ParsedContainer& parsed, const QByteArray& originalData,
                       const QString& logicalName, ModuleFormat format,
                       QVector<ResourceEntry>* resources) {
    if (!resources) return;
    const QString root = QFileInfo(logicalName).fileName().isEmpty()
                             ? parsed.signature.trimmed()
                             : QFileInfo(logicalName).fileName();
    ResourceEntry container;
    container.type = QStringLiteral("STFS_CONTAINER");
    container.name = root;
    container.dataSize = quint64(originalData.size());
    container.format = format;
    container.hierarchyPath = QStringList() << root;
    container.data = originalData;
    resources->push_back(std::move(container));
    if (!parsed.metadataText.isEmpty()) {
        ResourceEntry meta;
        meta.type = QStringLiteral("STFS_METADATA");
        meta.name = QStringLiteral("metadata.txt");
        meta.data = parsed.metadataText.toUtf8();
        meta.dataSize = quint64(meta.data.size());
        meta.format = format;
        meta.hierarchyPath = QStringList() << root << meta.name;
        resources->push_back(std::move(meta));
    }
    for (const ParsedFile& source : parsed.icons) {
        ResourceEntry e;
        e.type = QStringLiteral("STFS_ICON");
        e.name = leafName(source.path);
        e.dataOffset = source.dataOffset;
        e.dataSize = quint64(source.data.size());
        e.format = format;
        e.hierarchyPath = QStringList() << root << QStringLiteral("icons") << e.name;
        e.data = source.data;
        resources->push_back(std::move(e));
    }
    for (const ParsedFile& source : parsed.files) {
        ResourceEntry e;
        e.type = QStringLiteral("STFS_FILE");
        e.isEmbeddedFile = true;
        e.name = leafName(source.path);
        e.dataOffset = source.dataOffset;
        e.dataSize = quint64(source.data.size());
        e.format = format;
        e.hierarchyPath = QStringList() << root;
        e.hierarchyPath.append(parentPath(source.path));
        e.hierarchyPath.push_back(e.name);
        e.data = source.data;
        resources->push_back(std::move(e));
    }
}

} // namespace stfs
} // namespace peare
