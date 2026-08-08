#include "OleCompound.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace peare {
namespace {

constexpr quint32 kFreeSect = 0xffffffffu;
constexpr quint32 kEndOfChain = 0xfffffffeu;
constexpr quint32 kFatSect = 0xfffffffdu;
constexpr quint32 kDifSect = 0xfffffffcu;
constexpr quint32 kNoStream = 0xffffffffu;
constexpr quint64 kMaxDirectoryBytes = 64ULL * 1024ULL * 1024ULL;
constexpr quint64 kMaxFatEntries = 16ULL * 1024ULL * 1024ULL;

quint16 le16(const std::uint8_t* p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::uint8_t* p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 le64(const std::uint8_t* p)
{
    return quint64(le32(p)) | (quint64(le32(p + 4)) << 32);
}

bool readExact(const fs::IByteStore& store, std::int64_t offset,
               std::uint8_t* dst, int count)
{
    if (offset < 0 || count < 0 || offset > store.capacity() - count) return false;
    int done = 0;
    while (done < count) {
        const int n = store.read(offset + done, dst + done, count - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

fs::ByteStorePtr storeForFile(const QString& path)
{
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    }
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

QString decodeDirectoryName(const std::uint8_t* entry)
{
    const quint16 byteLength = le16(entry + 64);
    if (byteLength < 2 || byteLength > 64 || (byteLength & 1U) != 0) return {};
    QVector<ushort> units;
    units.reserve(byteLength / 2 - 1);
    for (quint16 pos = 0; pos + 2 < byteLength; pos += 2)
        units.push_back(le16(entry + pos));
    return QString::fromUtf16(units.constData(), units.size());
}

class ChainStore final : public fs::IByteStore {
public:
    ChainStore(fs::ByteStorePtr source, QVector<quint32> chain,
               std::int64_t unitSize, std::int64_t logicalSize,
               std::int64_t sourceBase, bool compoundSectors)
        : source_(std::move(source)), chain_(std::move(chain)), unitSize_(unitSize),
          logicalSize_(logicalSize), sourceBase_(sourceBase),
          compoundSectors_(compoundSectors)
    {
    }

    std::int64_t capacity() const override { return logicalSize_; }
    bool cheapRandomAccess() const override { return source_ && source_->cheapRandomAccess(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (!source_ || pos < 0 || count <= 0 || pos >= logicalSize_ || unitSize_ <= 0)
            return 0;
        const std::int64_t wanted = std::min<std::int64_t>(count, logicalSize_ - pos);
        std::int64_t done = 0;
        while (done < wanted) {
            const std::int64_t absolute = pos + done;
            const std::int64_t chainIndex = absolute / unitSize_;
            const std::int64_t inUnit = absolute % unitSize_;
            if (chainIndex < 0 || chainIndex >= chain_.size()) break;
            const quint32 id = chain_.at(int(chainIndex));
            std::int64_t sourceOffset = sourceBase_;
            if (compoundSectors_) {
                if (id > quint64(std::numeric_limits<std::int64_t>::max() / unitSize_) - 1)
                    break;
                sourceOffset += (std::int64_t(id) + 1) * unitSize_;
            } else {
                if (id > quint64(std::numeric_limits<std::int64_t>::max() / unitSize_))
                    break;
                sourceOffset += std::int64_t(id) * unitSize_;
            }
            sourceOffset += inUnit;
            const int chunk = int(std::min<std::int64_t>(wanted - done, unitSize_ - inUnit));
            const int n = source_->read(sourceOffset, dst + done, chunk);
            if (n <= 0) break;
            done += n;
            if (n != chunk) break;
        }
        return int(done);
    }

private:
    fs::ByteStorePtr source_;
    QVector<quint32> chain_;
    std::int64_t unitSize_ = 0;
    std::int64_t logicalSize_ = 0;
    std::int64_t sourceBase_ = 0;
    bool compoundSectors_ = true;
};

struct DirectoryEntry {
    QString rawName;
    QString displayName;
    quint8 type = 0;
    quint32 left = kNoStream;
    quint32 right = kNoStream;
    quint32 child = kNoStream;
    quint32 startSector = kEndOfChain;
    quint64 size = 0;
};

struct CompoundImage {
    fs::ByteStorePtr file;
    quint32 sectorSize = 0;
    quint32 miniSectorSize = 64;
    quint32 miniCutoff = 4096;
    QVector<quint32> fat;
    QVector<quint32> miniFat;
    QVector<DirectoryEntry> directory;
    fs::ByteStorePtr rootMiniStream;

    bool sectorOffset(quint32 sector, std::int64_t* offset) const
    {
        if (!offset || sector >= kDifSect || sectorSize == 0) return false;
        const quint64 value = (quint64(sector) + 1ULL) * sectorSize;
        if (!file || value > quint64(file->capacity()) ||
            sectorSize > quint64(file->capacity()) - value)
            return false;
        *offset = std::int64_t(value);
        return true;
    }

    bool chain(quint32 start, const QVector<quint32>& table,
               QVector<quint32>* out, quint64 maximum) const
    {
        if (!out) return false;
        out->clear();
        if (start == kEndOfChain) return true;
        QSet<quint32> seen;
        quint32 current = start;
        while (current != kEndOfChain) {
            if (current >= quint32(table.size()) || current >= kDifSect ||
                seen.contains(current) || quint64(out->size()) >= maximum)
                return false;
            seen.insert(current);
            out->push_back(current);
            current = table.at(int(current));
        }
        return true;
    }

    fs::ByteStorePtr regularStream(quint32 start, quint64 size) const
    {
        if (size == 0) return std::make_shared<fs::MemoryStore>(std::vector<std::uint8_t>());
        QVector<quint32> sectors;
        const quint64 needed = (size + sectorSize - 1) / sectorSize;
        if (!chain(start, fat, &sectors, needed + 1) || quint64(sectors.size()) < needed)
            return nullptr;
        return std::make_shared<ChainStore>(file, sectors, sectorSize,
                                            std::int64_t(size), 0, true);
    }

    fs::ByteStorePtr stream(const DirectoryEntry& entry) const
    {
        if (entry.size == 0) return std::make_shared<fs::MemoryStore>(std::vector<std::uint8_t>());
        if (entry.size < miniCutoff && entry.type == 2) {
            if (!rootMiniStream) return nullptr;
            QVector<quint32> minis;
            const quint64 needed = (entry.size + miniSectorSize - 1) / miniSectorSize;
            if (!chain(entry.startSector, miniFat, &minis, needed + 1) ||
                quint64(minis.size()) < needed)
                return nullptr;
            for (const quint32 mini : minis) {
                const quint64 end = (quint64(mini) + 1ULL) * miniSectorSize;
                if (end > quint64(rootMiniStream->capacity())) return nullptr;
            }
            return std::make_shared<ChainStore>(rootMiniStream, minis, miniSectorSize,
                                                std::int64_t(entry.size), 0, false);
        }
        return regularStream(entry.startSector, entry.size);
    }
};

bool parseCompound(const fs::ByteStorePtr& file, CompoundImage* image, QString* error)
{
    const auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!file || !image || file->capacity() < 512)
        return fail(QStringLiteral("Truncated compound file header"));

    std::uint8_t header[512];
    if (!readExact(*file, 0, header, sizeof(header)))
        return fail(QStringLiteral("Cannot read compound file header"));
    static const std::uint8_t magic[8] = {0xd0,0xcf,0x11,0xe0,0xa1,0xb1,0x1a,0xe1};
    if (std::memcmp(header, magic, sizeof(magic)) != 0)
        return fail(QStringLiteral("Not an OLE compound file"));

    const quint16 major = le16(header + 26);
    const quint16 byteOrder = le16(header + 28);
    const quint16 sectorShift = le16(header + 30);
    const quint16 miniShift = le16(header + 32);
    if (byteOrder != 0xfffe || (major != 3 && major != 4) ||
        (sectorShift != 9 && sectorShift != 12) || miniShift != 6 ||
        (major == 3 && sectorShift != 9) || (major == 4 && sectorShift != 12))
        return fail(QStringLiteral("Unsupported compound file version"));

    CompoundImage parsed;
    parsed.file = file;
    parsed.sectorSize = quint32(1U << sectorShift);
    parsed.miniSectorSize = quint32(1U << miniShift);
    parsed.miniCutoff = le32(header + 56);
    if (parsed.miniCutoff == 0 || parsed.miniCutoff > 64U * 1024U * 1024U)
        return fail(QStringLiteral("Invalid compound mini-stream cutoff"));

    const quint32 fatSectorCount = le32(header + 44);
    const quint32 firstDirectorySector = le32(header + 48);
    const quint32 firstMiniFatSector = le32(header + 60);
    const quint32 miniFatSectorCount = le32(header + 64);
    quint32 nextDifatSector = le32(header + 68);
    const quint32 difatSectorCount = le32(header + 72);
    if (file->capacity() < parsed.sectorSize)
        return fail(QStringLiteral("Truncated compound file"));
    const quint64 totalSectors = quint64(file->capacity() / parsed.sectorSize) - 1ULL;
    if (fatSectorCount == 0 || fatSectorCount > totalSectors ||
        totalSectors > kMaxFatEntries)
        return fail(QStringLiteral("Invalid compound FAT size"));

    QVector<quint32> fatSectorIds;
    fatSectorIds.reserve(int(fatSectorCount));
    for (int i = 0; i < 109 && fatSectorIds.size() < int(fatSectorCount); ++i) {
        const quint32 id = le32(header + 76 + i * 4);
        if (id != kFreeSect) fatSectorIds.push_back(id);
    }

    QSet<quint32> difatSeen;
    for (quint32 n = 0; n < difatSectorCount && nextDifatSector != kEndOfChain; ++n) {
        if (nextDifatSector >= totalSectors || difatSeen.contains(nextDifatSector))
            return fail(QStringLiteral("Invalid DIFAT chain"));
        difatSeen.insert(nextDifatSector);
        std::int64_t offset = 0;
        if (!parsed.sectorOffset(nextDifatSector, &offset))
            return fail(QStringLiteral("Invalid DIFAT sector"));
        std::vector<std::uint8_t> sector(parsed.sectorSize);
        if (!readExact(*file, offset, sector.data(), int(sector.size())))
            return fail(QStringLiteral("Truncated DIFAT sector"));
        const int entries = int(parsed.sectorSize / 4) - 1;
        for (int i = 0; i < entries && fatSectorIds.size() < int(fatSectorCount); ++i) {
            const quint32 id = le32(sector.data() + i * 4);
            if (id != kFreeSect) fatSectorIds.push_back(id);
        }
        nextDifatSector = le32(sector.data() + entries * 4);
    }
    if (fatSectorIds.size() != int(fatSectorCount))
        return fail(QStringLiteral("Incomplete compound FAT sector list"));

    const int fatEntriesPerSector = int(parsed.sectorSize / 4);
    parsed.fat.reserve(fatSectorIds.size() * fatEntriesPerSector);
    QSet<quint32> fatSeen;
    for (const quint32 id : fatSectorIds) {
        if (id >= totalSectors || fatSeen.contains(id))
            return fail(QStringLiteral("Invalid compound FAT sector"));
        fatSeen.insert(id);
        std::int64_t offset = 0;
        if (!parsed.sectorOffset(id, &offset))
            return fail(QStringLiteral("Invalid compound FAT offset"));
        std::vector<std::uint8_t> sector(parsed.sectorSize);
        if (!readExact(*file, offset, sector.data(), int(sector.size())))
            return fail(QStringLiteral("Truncated compound FAT"));
        for (int i = 0; i < fatEntriesPerSector; ++i)
            parsed.fat.push_back(le32(sector.data() + i * 4));
    }

    QVector<quint32> directoryChain;
    if (!parsed.chain(firstDirectorySector, parsed.fat, &directoryChain,
                      kMaxDirectoryBytes / parsed.sectorSize) || directoryChain.isEmpty())
        return fail(QStringLiteral("Invalid compound directory chain"));
    const quint64 directoryBytes = quint64(directoryChain.size()) * parsed.sectorSize;
    if (directoryBytes > kMaxDirectoryBytes)
        return fail(QStringLiteral("Compound directory is too large"));
    QByteArray directoryData(int(directoryBytes), Qt::Uninitialized);
    for (int i = 0; i < directoryChain.size(); ++i) {
        std::int64_t offset = 0;
        if (!parsed.sectorOffset(directoryChain.at(i), &offset) ||
            !readExact(*file, offset,
                       reinterpret_cast<std::uint8_t*>(directoryData.data()) +
                           std::int64_t(i) * parsed.sectorSize,
                       int(parsed.sectorSize)))
            return fail(QStringLiteral("Truncated compound directory"));
    }

    for (int pos = 0; pos + 128 <= directoryData.size(); pos += 128) {
        const auto* raw = reinterpret_cast<const std::uint8_t*>(directoryData.constData() + pos);
        DirectoryEntry entry;
        entry.type = raw[66];
        entry.rawName = decodeDirectoryName(raw);
        entry.displayName = entry.rawName;
        entry.left = le32(raw + 68);
        entry.right = le32(raw + 72);
        entry.child = le32(raw + 76);
        entry.startSector = le32(raw + 116);
        entry.size = le64(raw + 120);
        if (major == 3) entry.size &= 0xffffffffULL;
        parsed.directory.push_back(std::move(entry));
    }
    if (parsed.directory.isEmpty() || parsed.directory.first().type != 5)
        return fail(QStringLiteral("Invalid compound root storage"));

    if (miniFatSectorCount) {
        QVector<quint32> miniFatChain;
        if (!parsed.chain(firstMiniFatSector, parsed.fat, &miniFatChain,
                          quint64(miniFatSectorCount) + 1ULL) ||
            miniFatChain.size() < int(miniFatSectorCount))
            return fail(QStringLiteral("Invalid compound mini FAT chain"));
        parsed.miniFat.reserve(int(miniFatSectorCount) * fatEntriesPerSector);
        for (quint32 n = 0; n < miniFatSectorCount; ++n) {
            std::int64_t offset = 0;
            if (!parsed.sectorOffset(miniFatChain.at(int(n)), &offset))
                return fail(QStringLiteral("Invalid mini FAT sector"));
            std::vector<std::uint8_t> sector(parsed.sectorSize);
            if (!readExact(*file, offset, sector.data(), int(sector.size())))
                return fail(QStringLiteral("Truncated mini FAT"));
            for (int i = 0; i < fatEntriesPerSector; ++i)
                parsed.miniFat.push_back(le32(sector.data() + i * 4));
        }
    }

    const DirectoryEntry& root = parsed.directory.first();
    if (root.size)
        parsed.rootMiniStream = parsed.regularStream(root.startSector, root.size);
    if (root.size && !parsed.rootMiniStream)
        return fail(QStringLiteral("Invalid compound mini stream"));

    *image = std::move(parsed);
    if (error) error->clear();
    return true;
}

bool traverseTree(const CompoundImage& image, quint32 sid, const QStringList& parent,
                  QVector<OleCompoundStream>* streams, QSet<quint32>* active,
                  QSet<quint32>* emitted, int depth, int* storageCount, QString* error)
{
    if (sid == kNoStream) return true;
    if (!streams || !active || !emitted || sid >= quint32(image.directory.size()) ||
        depth > 256 || active->contains(sid)) {
        if (error) *error = QStringLiteral("Invalid compound directory tree");
        return false;
    }
    active->insert(sid);
    const DirectoryEntry& entry = image.directory.at(int(sid));
    if (!traverseTree(image, entry.left, parent, streams, active, emitted,
                      depth + 1, storageCount, error))
        return false;

    if (!emitted->contains(sid) && (entry.type == 1 || entry.type == 2)) {
        emitted->insert(sid);
        if (entry.displayName.isEmpty()) {
            if (error) *error = QStringLiteral("Compound entry has no name");
            return false;
        }
        if (entry.type == 1) {
            if (storageCount) ++*storageCount;
            QStringList childParent = parent;
            childParent.push_back(entry.displayName);
            if (!traverseTree(image, entry.child, childParent, streams, active,
                              emitted, depth + 1, storageCount, error))
                return false;
        } else {
            const fs::ByteStorePtr stream = image.stream(entry);
            if (!stream) {
                if (error) *error = QStringLiteral("Invalid compound stream: %1")
                    .arg(entry.displayName);
                return false;
            }
            OleCompoundStream resource;
            resource.name = entry.displayName;
            resource.size = entry.size;
            resource.dataOffset = entry.size >= image.miniCutoff && entry.startSector < kDifSect
                ? (quint64(entry.startSector) + 1ULL) * image.sectorSize : 0;
            resource.miniStream = entry.size < image.miniCutoff;
            resource.hierarchyPath = parent;
            resource.content = stream;
            streams->push_back(std::move(resource));
        }
    }

    if (!traverseTree(image, entry.right, parent, streams, active, emitted,
                      depth + 1, storageCount, error))
        return false;
    active->remove(sid);
    return true;
}


} // namespace

bool hasOleCompoundMagic(const QByteArray& data)
{
    static const char magic[] = "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1";
    return data.size() >= 8 && std::memcmp(data.constData(), magic, 8) == 0;
}

fs::ByteStorePtr oleStoreForFile(const QString& path)
{
    return storeForFile(path);
}

bool enumerateOleCompound(const fs::ByteStorePtr& file,
                          OleCompoundContents* contents,
                          QString* error)
{
    if (!contents) {
        if (error) *error = QStringLiteral("Null compound output");
        return false;
    }
    contents->streams.clear();
    contents->storageCount = 0;

    CompoundImage image;
    QString localError;
    if (!parseCompound(file, &image, &localError)) {
        if (error) *error = localError;
        return false;
    }

    QSet<quint32> active;
    QSet<quint32> emitted;
    if (!traverseTree(image, image.directory.first().child, {}, &contents->streams,
                      &active, &emitted, 0, &contents->storageCount, &localError)) {
        contents->streams.clear();
        contents->storageCount = 0;
        if (error) *error = localError;
        return false;
    }
    if (error) error->clear();
    return true;
}

} // namespace peare
