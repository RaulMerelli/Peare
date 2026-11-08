#include "WadModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"
#include <QFile>
#include <QSet>
#include <cstring>
namespace peare {
namespace {
struct NamespaceMarker {
    bool valid = false;
    bool starts = false;
    QString key;
};

struct NamespaceRange {
    int start = 0;
    int end = -1;
    QString key;

    NamespaceRange(int rangeStart, int rangeEnd, const QString& rangeKey)
        : start(rangeStart), end(rangeEnd), key(rangeKey) {}
};

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(reinterpret_cast<const std::uint8_t*>(mapped),
                                                   std::int64_t(size),
                                                   std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
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
QString wad2LumpName(const std::uint8_t* p) {
    QByteArray a(reinterpret_cast<const char*>(p), 16);
    int z = a.indexOf('\0');
    if (z >= 0) a.truncate(z);
    return QString::fromLatin1(a).trimmed();
}
NamespaceMarker namespaceMarker(const QString& name) {
    NamespaceMarker marker;
    const QString upperName = name.toUpper();
    if (upperName.endsWith(QLatin1String("_START"))) marker.starts = true;
    else if (upperName.endsWith(QLatin1String("_END"))) marker.starts = false;
    else return marker;
    QString prefix = upperName.left(upperName.indexOf(QLatin1Char('_')));
    if (prefix == QLatin1String("PP") || prefix == QLatin1String("SS") ||
        prefix == QLatin1String("FF") || prefix == QLatin1String("TT"))
        prefix.remove(1, 1);
    marker.key = prefix;
    marker.valid = !prefix.isEmpty();
    return marker;
}

bool isMapLump(const QString& name, const QString& mapName) {
    static const QSet<QString> names = {
        QStringLiteral("THINGS"), QStringLiteral("VERTEXES"), QStringLiteral("LINEDEFS"),
        QStringLiteral("SIDEDEFS"), QStringLiteral("SECTORS"), QStringLiteral("SEGS"),
        QStringLiteral("SSECTORS"), QStringLiteral("NODES"), QStringLiteral("BLOCKMAP"),
        QStringLiteral("REJECT"), QStringLiteral("SCRIPTS"), QStringLiteral("BEHAVIOR"),
        QStringLiteral("LEAFS"), QStringLiteral("LIGHTS"), QStringLiteral("MACROS"),
        QStringLiteral("GL_MAP01"),
        QStringLiteral("GL_VERT"), QStringLiteral("GL_SEGS"), QStringLiteral("GL_SSECT"),
        QStringLiteral("GL_NODES"), QStringLiteral("GL_PVS"), QStringLiteral("TEXTMAP"),
        QStringLiteral("ENDMAP"), QStringLiteral("ZNODES")
    };
    return names.contains(name) || name == QStringLiteral("GL_") + mapName;
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
    if (h.size() != 12) {
        m->info_.error = "Truncated WAD header";
        return ModulePtr(std::move(m));
    }
    const bool wad2 = std::memcmp(h.data(), "WAD2", 4) == 0;
    const bool wad3 = std::memcmp(h.data(), "WAD3", 4) == 0;
    const bool quakeWad = wad2 || wad3;
    if (!quakeWad && std::memcmp(h.data(), "IWAD", 4) && std::memcmp(h.data(), "PWAD", 4)) {
        m->info_.error = "Invalid WAD signature";
        return ModulePtr(std::move(m));
    }
    quint32 count = le32(h.data() + 4), dir = le32(h.data() + 8);
    const quint32 entrySize = quakeWad ? 32u : 16u;
    quint64 bytes = quint64(count) * entrySize;
    if (count > 1000000 || dir > quint64(f->capacity()) || bytes > quint64(f->capacity()) - dir) {
        m->info_.error = "Invalid WAD directory";
        return ModulePtr(std::move(m));
    }
    auto table = f->readRange(dir, bytes);
    if (table.size() != bytes) {
        m->info_.error = "Truncated WAD directory";
        return ModulePtr(std::move(m));
    }
    QVector<QString> names;
    QVector<QString> upperNames;
    QVector<quint32> offsets;
    QVector<quint32> sizes;
    names.reserve(int(count));
    upperNames.reserve(int(count));
    offsets.reserve(int(count));
    sizes.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const auto* e = table.data() + i * entrySize;
        quint32 off = le32(e), sz = le32(e + 4);
        if (off > quint64(f->capacity()) || sz > quint64(f->capacity()) - off) {
            m->info_.error = QStringLiteral("Invalid WAD lump %1").arg(i);
            m->resources_.clear();
            return ModulePtr(std::move(m));
        }
        if (quakeWad) {
            const quint32 unpackedSize = le32(e + 8);
            const std::uint8_t compression = e[13];
            if (unpackedSize > quint64(f->capacity()) || compression > 1 ||
                (compression == 0 && unpackedSize != sz)) {
                m->info_.error = QStringLiteral("Invalid Quake WAD lump %1").arg(i);
                m->resources_.clear();
                return ModulePtr(std::move(m));
            }
            names.push_back(wad2LumpName(e + 16));
        } else {
            names.push_back(lumpName(e + 8));
        }
        upperNames.push_back(names.last().toUpper());
        offsets.push_back(off);
        sizes.push_back(sz);
    }

    QVector<NamespaceRange> namespaces;
    for (int i = 0; i < upperNames.size(); ++i) {
        const NamespaceMarker marker = namespaceMarker(upperNames.at(i));
        if (!marker.valid) continue;
        if (marker.starts) {
            namespaces.push_back({i, -1, marker.key});
            continue;
        }
        bool matched = false;
        for (int range = 0; range < namespaces.size(); ++range) {
            if (namespaces.at(range).key != marker.key || namespaces.at(range).end >= 0)
                continue;
            namespaces[range].end = i;
            matched = true;
            break;
        }
        if (!matched && marker.key == QLatin1String("F"))
            namespaces.push_back({0, i, marker.key});
    }

    QVector<int> mapRoles(static_cast<int>(count), -1);
    for (int i = 0; i < names.size(); ++i) {
        const QString mapName = upperNames.at(i);
        if (i + 1 >= names.size()) continue;
        int mapEnd = -1;
        if (upperNames.at(i + 1) == QLatin1String("TEXTMAP")) {
            for (int j = i + 2; j < names.size(); ++j) {
                if (upperNames.at(j) == QLatin1String("ENDMAP")) {
                    mapEnd = j;
                    break;
                }
            }
        } else {
            QSet<QString> found;
            int j = i + 1;
            while (j < names.size() && isMapLump(upperNames.at(j), mapName)) {
                found.insert(upperNames.at(j));
                ++j;
            }
            const bool complete = found.contains(QLatin1String("THINGS")) &&
                found.contains(QLatin1String("LINEDEFS")) &&
                found.contains(QLatin1String("SIDEDEFS")) &&
                found.contains(QLatin1String("VERTEXES")) &&
                found.contains(QLatin1String("SECTORS"));
            if (complete) mapEnd = j - 1;
        }
        if (mapEnd < i + 1) continue;
        mapRoles[i] = 0;
        for (int j = i + 1; j <= mapEnd; ++j) {
            mapRoles[j] = 1;
        }
        i = mapEnd;
    }

    for (quint32 i = 0; i < count; ++i) {
        ResourceEntry r;
        r.name = names.at(int(i));
        if (quakeWad) {
            const auto* e = table.data() + i * entrySize;
            const std::uint8_t type = e[12];
            const std::uint8_t compression = e[13];
            const QString prefix = wad2 ? QStringLiteral("WAD2") : QStringLiteral("WAD3");
            r.type = prefix + (compression == 1 ? QStringLiteral("_LZSS_LUMP")
                                                 : QStringLiteral("_LUMP"));
            r.type += QStringLiteral("_TYPE_%1").arg(type);
            r.dataOffset = offsets.at(int(i));
            r.dataSize = sizes.at(int(i));
            r.baseId = int(i);
            r.format = ModuleFormat::WAD;
            r.isEmbeddedFile = r.dataSize > 0;
            r.content = std::make_shared<fs::SubStore>(f, r.dataOffset, r.dataSize);
            m->resources_.push_back(std::move(r));
            continue;
        }
        const QString& upperName = upperNames.at(int(i));
        const NamespaceMarker marker = namespaceMarker(upperName);
        QString detectedNamespace;
        for (const NamespaceRange& range : namespaces) {
            if (range.end >= 0 && range.start <= int(i) && int(i) <= range.end) {
                detectedNamespace = range.key;
                break;
            }
        }
        r.type = "WAD_LUMP";
        if (marker.valid) r.type = "WAD_MARKER";
        else if (mapRoles.at(int(i)) == 0) r.type = "WAD_MAP_HEADER";
        else if (mapRoles.at(int(i)) == 1) r.type = "WAD_MAP_LUMP";
        else if (upperName == QLatin1String("PLAYPAL")) r.type = "WAD_PALETTE";
        else if (upperName == QLatin1String("PNAMES")) r.type = "WAD_PNAMES";
        else if (upperName == QLatin1String("TEXTURE1") || upperName == QLatin1String("TEXTURE2"))
            r.type = "WAD_TEXTUREX";
        else if (detectedNamespace == QLatin1String("P")) r.type = "WAD_DOOM_PATCH";
        else if (detectedNamespace == QLatin1String("S")) r.type = "WAD_DOOM_SPRITE";
        if (r.name.isEmpty()) r.name = QStringLiteral("LUMP_%1").arg(i);
        r.dataOffset = offsets.at(int(i));
        r.dataSize = sizes.at(int(i));
        r.baseId = int(i);
        r.format = ModuleFormat::WAD;
        r.isEmbeddedFile = r.dataSize > 0 && r.type == QLatin1String("WAD_LUMP");
        r.content = std::make_shared<fs::SubStore>(f, r.dataOffset, r.dataSize);
        m->resources_.push_back(std::move(r));
    }
    if (quakeWad) {
        m->info_.description = QStringLiteral("Quake WAD%1 archive — %2 lumps")
            .arg(wad2 ? 2 : 3).arg(count);
    } else {
        m->info_.description = QStringLiteral("%1 — %2 lumps")
            .arg(std::memcmp(h.data(), "IWAD", 4) ? "Doom patch WAD (PWAD)"
                                                  : "Doom internal WAD (IWAD)")
            .arg(count);
    }
    return ModulePtr(std::move(m));
}
} // namespace peare
