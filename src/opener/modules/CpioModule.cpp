#include "CpioModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <tuple>

namespace peare {
namespace {

enum class HeaderKind { Newc, Crc, Odc, AfioLarge, BinaryLe, BinaryBe };

struct EntryHeader {
    QString name;
    std::uint64_t inode = 0;
    std::uint64_t deviceMajor = 0;
    std::uint64_t deviceMinor = 0;
    std::uint64_t mode = 0;
    std::uint64_t links = 0;
    std::uint64_t size = 0;
    std::uint64_t checksum = 0;
    std::int64_t dataOffset = 0;
    bool directory = false;
    bool regular = false;
    bool symlink = false;
};

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
}

bool readBytes(const fs::ByteStorePtr& file, std::int64_t offset, std::size_t size,
               std::uint8_t* out) {
    if (!file || offset < 0 || offset > file->capacity() ||
        size > std::size_t(file->capacity() - offset) ||
        size > std::size_t(std::numeric_limits<int>::max())) return false;
    if (size) file->readExactly(offset, out, static_cast<int>(size));
    return true;
}

bool parseNumber(const std::uint8_t* data, std::size_t size, unsigned base,
                 std::uint64_t* value) {
    std::uint64_t result = 0;
    bool digitSeen = false;
    for (std::size_t i = 0; i < size; ++i) {
        const std::uint8_t c = data[i];
        if (c == 0 || c == ' ') continue;
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || result > (UINT64_MAX - digit) / base) return false;
        result = result * base + digit;
        digitSeen = true;
    }
    if (!digitSeen) return false;
    *value = result;
    return true;
}

std::uint16_t read16(const std::uint8_t* p, bool littleEndian) {
    return littleEndian ? std::uint16_t(p[0] | (std::uint16_t(p[1]) << 8))
                        : std::uint16_t((std::uint16_t(p[0]) << 8) | p[1]);
}

std::uint32_t read32(const std::uint8_t* p, bool littleEndian) {
    return (std::uint32_t(read16(p, littleEndian)) << 16) |
           std::uint32_t(read16(p + 2, littleEndian));
}

bool isZero(const fs::ByteStorePtr& file, std::int64_t offset) {
    std::int64_t position = offset;
    while (position < file->capacity()) {
        const auto bytes = file->readRange(position, std::min<std::int64_t>(4096, file->capacity() - position));
        if (bytes.empty()) return false;
        if (std::any_of(bytes.begin(), bytes.end(), [](std::uint8_t b) { return b != 0; }))
            return false;
        position += std::int64_t(bytes.size());
    }
    return true;
}

QString normalizeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    QStringList safe;
    for (const QString& part : name.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String("..")) return QString();
        safe.push_back(part);
    }
    return safe.join(QLatin1Char('/'));
}

QStringList hierarchyFor(const QString& name) {
    QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

QString safeLeaf(const QString& path, int index) {
    QString leaf = path.section(QLatin1Char('/'), -1);
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i)
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("cpio_file_%1").arg(index) : leaf;
}

bool detectKind(const fs::ByteStorePtr& file, std::int64_t pos, HeaderKind* kind) {
    std::uint8_t prefix[6]{};
    if (!readBytes(file, pos, sizeof(prefix), prefix)) return false;
    const QByteArray magic(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    if (magic == "070701") *kind = HeaderKind::Newc;
    else if (magic == "070702") *kind = HeaderKind::Crc;
    else if (magic == "070707") *kind = HeaderKind::Odc;
    else if (magic == "070727") *kind = HeaderKind::AfioLarge;
    else if (prefix[0] == 0xc7 && prefix[1] == 0x71) *kind = HeaderKind::BinaryLe;
    else if (prefix[0] == 0x71 && prefix[1] == 0xc7) *kind = HeaderKind::BinaryBe;
    else return false;
    return true;
}

bool parseHeader(const fs::ByteStorePtr& file, std::int64_t pos, HeaderKind kind,
                 EntryHeader* entry, std::int64_t* nextOffset) {
    const bool modern = kind == HeaderKind::Newc || kind == HeaderKind::Crc;
    const bool afio = kind == HeaderKind::AfioLarge;
    const bool binary = kind == HeaderKind::BinaryLe || kind == HeaderKind::BinaryBe;
    const bool little = kind == HeaderKind::BinaryLe;
    const std::size_t fixed = modern ? 110 : (afio ? 116 : (binary ? 26 : 76));
    std::array<std::uint8_t, 116> h{};
    if (!readBytes(file, pos, fixed, h.data())) return false;
    std::uint64_t mode = 0, nameSize = 0, size = 0, inode = 0, devMaj = 0, devMin = 0;
    std::uint64_t links = 0, checksum = 0;
    if (modern) {
        std::uint64_t fields[13]{};
        for (int i = 0; i < 13; ++i)
            if (!parseNumber(h.data() + 6 + i * 8, 8, 16, &fields[i])) return false;
        inode = fields[0]; mode = fields[1]; links = fields[4]; size = fields[6];
        devMaj = fields[7]; devMin = fields[8]; nameSize = fields[11]; checksum = fields[12];
    } else if (kind == HeaderKind::Odc) {
        std::uint64_t fields[10]{};
        const int widths[] = {6, 6, 6, 6, 6, 6, 6, 11, 6, 11};
        std::size_t offset = 6;
        for (int i = 0; i < 10; ++i) {
            if (!parseNumber(h.data() + offset, std::size_t(widths[i]), 8, &fields[i])) return false;
            offset += std::size_t(widths[i]);
        }
        devMin = fields[0]; inode = fields[1]; mode = fields[2]; links = fields[5];
        nameSize = fields[8]; size = fields[9];
    } else if (afio) {
        std::uint64_t field = 0;
        if (!parseNumber(h.data() + 6, 8, 16, &devMin) ||
            !parseNumber(h.data() + 14, 16, 16, &inode) || h[30] != 'm' ||
            !parseNumber(h.data() + 31, 6, 8, &mode) ||
            !parseNumber(h.data() + 53, 8, 16, &links) ||
            !parseNumber(h.data() + 86, 4, 16, &nameSize) ||
            !parseNumber(h.data() + 99, 16, 16, &size) || h[115] != ':') return false;
        if (!parseNumber(h.data() + 94, 4, 16, &field)) return false;
    } else {
        inode = read16(h.data() + 4, little);
        mode = read16(h.data() + 6, little);
        links = read16(h.data() + 12, little);
        devMin = read16(h.data() + 2, little);
        nameSize = read16(h.data() + 20, little);
        size = read32(h.data() + 22, little);
    }
    if (nameSize == 0 || nameSize > 1024 * 1024 || size > std::uint64_t(file->capacity())) return false;
    const std::int64_t nameOffset = pos + std::int64_t(fixed);
    if (nameSize > std::uint64_t(file->capacity() - nameOffset)) return false;
    QByteArray rawName(static_cast<int>(nameSize), '\0');
    if (!readBytes(file, nameOffset, std::size_t(nameSize),
                   reinterpret_cast<std::uint8_t*>(rawName.data())) ||
        rawName.at(rawName.size() - 1) != '\0' ||
        std::find(rawName.constData(), rawName.constData() + rawName.size() - 1, '\0') !=
            rawName.constData() + rawName.size() - 1)
        return false;
    const QString raw = QString::fromUtf8(rawName.constData(), rawName.size() - 1);
    const QString name = normalizeName(raw);
    if (raw != QLatin1String("TRAILER!!!") && name.isEmpty() &&
        raw != QLatin1String(".") && raw != QLatin1String("./")) return false;
    std::int64_t dataOffset = nameOffset + std::int64_t(nameSize);
    if (modern) dataOffset = (dataOffset + 3) & ~std::int64_t(3);
    else if (binary) dataOffset = (dataOffset + 1) & ~std::int64_t(1);
    if (dataOffset < 0 || size > std::uint64_t(file->capacity() - dataOffset)) return false;
    std::int64_t next = dataOffset + std::int64_t(size);
    if (modern) next = (next + 3) & ~std::int64_t(3);
    else if (binary) next = (next + 1) & ~std::int64_t(1);
    if (next > file->capacity()) return false;
    entry->name = name;
    entry->inode = inode;
    entry->deviceMajor = devMaj;
    entry->deviceMinor = devMin;
    entry->mode = mode;
    entry->links = links;
    entry->size = size;
    entry->checksum = checksum;
    entry->dataOffset = dataOffset;
    const std::uint64_t type = mode & 0170000;
    entry->directory = type == 0040000;
    entry->regular = type == 0100000 || (binary && type == 0);
    entry->symlink = type == 0120000;
    *nextOffset = next;
    return true;
}

std::uint32_t byteSum(const fs::ByteStorePtr& file, std::int64_t offset, std::int64_t size) {
    std::uint32_t sum = 0;
    for (std::int64_t done = 0; done < size;) {
        const auto bytes = file->readRange(offset + done, std::min<std::int64_t>(65536, size - done));
        if (bytes.empty()) return UINT32_MAX;
        for (std::uint8_t byte : bytes) sum += byte;
        done += std::int64_t(bytes.size());
    }
    return sum;
}

}  // namespace

ModulePtr CpioModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

ModulePtr CpioModule::open(const QByteArray& data, const QString& logicalName) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())), logicalName);
}

ModulePtr CpioModule::open(const fs::ByteStorePtr& file, const QString& sourceName,
                           const QString& subPath) {
    Q_UNUSED(subPath);
    auto module = peare::makeUnique<CpioModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::CPIO;
    module->info_.description = QStringLiteral("CPIO archive");
    module->file_ = file;
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open CPIO archive");
        return ModulePtr(std::move(module));
    }

    using LinkKey = std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>;
    std::map<LinkKey, fs::ByteStorePtr> hardLinks;
    std::map<LinkKey, std::vector<int>> pendingHardLinks;
    QStringList explicitDirectories;
    std::int64_t position = 0;
    int index = 0;
    bool sawEntry = false;
    bool afterTrailer = false;
    while (position < file->capacity()) {
        if (isZero(file, position)) break;
        HeaderKind kind;
        if (!detectKind(file, position, &kind)) {
            if (afterTrailer) break;
            module->info_.error = QStringLiteral("Invalid CPIO header at offset %1").arg(position);
            return ModulePtr(std::move(module));
        }
        EntryHeader parsed;
        std::int64_t next = 0;
        if (!parseHeader(file, position, kind, &parsed, &next)) {
            module->info_.error = QStringLiteral("Invalid or truncated CPIO entry at offset %1").arg(position);
            return ModulePtr(std::move(module));
        }
        sawEntry = true;
        if (parsed.name == QLatin1String("TRAILER!!!")) {
            afterTrailer = true;
            position = next;
            if (isZero(file, position)) break;
            while (position < file->capacity()) {
                const auto bytes = file->readRange(position,
                    std::min<std::int64_t>(4096, file->capacity() - position));
                if (bytes.empty()) break;
                const auto nonzero = std::find_if(bytes.begin(), bytes.end(),
                    [](std::uint8_t byte) { return byte != 0; });
                if (nonzero == bytes.end()) {
                    position += std::int64_t(bytes.size());
                    continue;
                }
                position += std::int64_t(nonzero - bytes.begin());
                break;
            }
            if (position == file->capacity()) break;
            continue;
        }
        afterTrailer = false;
        if (parsed.directory) {
            if (!parsed.name.isEmpty()) explicitDirectories.push_back(parsed.name);
        } else if (parsed.regular || parsed.symlink) {
            fs::ByteStorePtr content = std::make_shared<fs::SubStore>(file, parsed.dataOffset,
                                                                       std::int64_t(parsed.size));
            if (kind == HeaderKind::Crc && parsed.regular) {
                const auto source = content;
                const auto expected = std::uint32_t(parsed.checksum);
                content = std::make_shared<fs::ValidatedStore>(source, [source, expected] {
                    return byteSum(source, 0, source->capacity()) == expected;
                });
            }
            const LinkKey key(parsed.deviceMajor, parsed.deviceMinor, parsed.inode);
            if (parsed.regular && parsed.links > 1) {
                auto found = hardLinks.find(key);
                if (parsed.size == 0 && found != hardLinks.end()) content = found->second;
                else if (parsed.size > 0) {
                    hardLinks[key] = content;
                    auto pending = pendingHardLinks.find(key);
                    if (pending != pendingHardLinks.end()) {
                        for (int resourceIndex : pending->second) {
                            module->resources_[resourceIndex].content = content;
                            module->resources_[resourceIndex].dataSize = quint64(content->capacity());
                        }
                        pendingHardLinks.erase(pending);
                    }
                }
            }
            ResourceEntry entry;
            entry.type = QStringLiteral("CPIO_FILE");
            entry.isEmbeddedFile = parsed.regular;
            entry.name = safeLeaf(parsed.name, index++);
            entry.language = QStringLiteral("neutral");
            entry.dataOffset = quint64(parsed.dataOffset);
            entry.dataSize = quint64(content->capacity());
            entry.format = ModuleFormat::CPIO;
            entry.hierarchyPath = hierarchyFor(parsed.name);
            entry.content = content;
            if (parsed.regular && parsed.links > 1 && parsed.size == 0 &&
                hardLinks.find(key) == hardLinks.end())
                pendingHardLinks[key].push_back(module->resources_.size());
            module->resources_.push_back(std::move(entry));
        } else if (!parsed.name.isEmpty()) {
            ResourceEntry entry;
            entry.type = QStringLiteral("CPIO_SPECIAL");
            entry.name = safeLeaf(parsed.name, index++);
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::CPIO;
            entry.hierarchyPath = hierarchyFor(parsed.name);
            module->resources_.push_back(std::move(entry));
        }
        position = next;
    }
    if (!sawEntry) {
        module->info_.error = QStringLiteral("Empty or invalid CPIO archive");
        return ModulePtr(std::move(module));
    }
    QSet<QString> occupiedPaths;
    for (const ResourceEntry& entry : module->resources_) {
        QString entryPath;
        QStringList parts = entry.hierarchyPath;
        parts.push_back(entry.name);
        for (const QString& part : parts) {
            if (!entryPath.isEmpty()) entryPath += QLatin1Char('/');
            entryPath += part;
            occupiedPaths.insert(entryPath);
        }
    }
    for (const QString& path : explicitDirectories) {
        if (occupiedPaths.contains(path)) continue;
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        ResourceEntry entry;
        entry.type = QStringLiteral("CPIO_DIR");
        entry.name = parts.last();
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::CPIO;
        entry.hierarchyPath = parts;
        entry.hierarchyPath.removeLast();
        entry.content = file;
        entry.isDirectory = true;
        entry.containerSubPath = path;
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
