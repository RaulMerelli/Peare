#include "AsarModule.h"
#include "Compat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QSet>
#include <QStringList>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace peare {
namespace {

const quint32 kPickleUInt32PayloadSize = 4;
const quint32 kMaxHeaderSize = 256U * 1024U * 1024U;
const int kMaxDepth = 256;
const int kMaxLinkDepth = 40;
const int kMaxEntries = 1000000;

quint32 le32(const std::uint8_t* p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint32 le32(const QByteArray& data, int offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    return le32(reinterpret_cast<const std::uint8_t*>(data.constData() + offset));
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

QString safeLeaf(QString name, int index)
{
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString leaf = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i) {
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    }
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("asar_file_%1.bin").arg(index, 3, 10, QLatin1Char('0'))
                          : leaf;
}

QStringList hierarchyFor(const QString& path)
{
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

bool validEntryName(const QString& name)
{
    return !name.isEmpty() && name != QStringLiteral(".") && name != QStringLiteral("..") &&
           !name.contains(QLatin1Char('/')) && !name.contains(QLatin1Char('\\')) &&
           !name.contains(QChar(0));
}

bool numericSize(const QJsonObject& node, quint64* out)
{
    if (!out || !node.contains(QStringLiteral("size"))) return false;
    const QJsonValue value = node.value(QStringLiteral("size"));
    if (!value.isDouble()) return false;
    const double d = value.toDouble(-1.0);
    if (!std::isfinite(d) || d < 0.0 || d > 9007199254740991.0 || std::floor(d) != d)
        return false;
    *out = quint64(d);
    return true;
}

bool numericOffset(const QJsonObject& node, quint64* out)
{
    if (!out) return false;
    const QJsonValue value = node.value(QStringLiteral("offset"));
    if (!value.isString()) return false;
    const QString s = value.toString();
    if (s.isEmpty()) return false;
    for (int i = 0; i < s.size(); ++i)
        if (!s.at(i).isDigit()) return false;
    bool ok = false;
    const qulonglong v = s.toULongLong(&ok, 10);
    if (!ok || v > qulonglong(std::numeric_limits<std::int64_t>::max())) return false;
    *out = quint64(v);
    return true;
}

struct HeaderInfo {
    QJsonObject root;
    std::int64_t dataOffset = 0;
};

bool parseHeader(const fs::ByteStorePtr& file, HeaderInfo* out, QString* error)
{
    if (!file || file->capacity() < 16) {
        if (error) *error = QStringLiteral("ASAR header is truncated");
        return false;
    }

    std::uint8_t first[16];
    if (!readExact(*file, 0, first, 16)) {
        if (error) *error = QStringLiteral("Cannot read ASAR header");
        return false;
    }
    const quint32 sizePicklePayload = le32(first);
    const quint32 headerSize = le32(first + 4);
    const quint32 headerPicklePayload = le32(first + 8);
    const quint32 jsonSize = le32(first + 12);

    if (sizePicklePayload != kPickleUInt32PayloadSize || headerSize < 8 ||
        headerSize > kMaxHeaderSize || (headerSize & 3U) != 0 ||
        headerPicklePayload + 4U != headerSize || jsonSize > headerPicklePayload - 4U ||
        std::int64_t(headerSize) > file->capacity() - 8) {
        if (error) *error = QStringLiteral("Invalid ASAR Pickle header");
        return false;
    }

    QByteArray header(int(headerSize), Qt::Uninitialized);
    if (!readExact(*file, 8, reinterpret_cast<std::uint8_t*>(header.data()), int(headerSize))) {
        if (error) *error = QStringLiteral("Cannot read ASAR JSON header");
        return false;
    }
    if (le32(header, 0) != headerPicklePayload || le32(header, 4) != jsonSize ||
        quint64(jsonSize) > quint64(header.size() - 8)) {
        if (error) *error = QStringLiteral("Invalid ASAR JSON Pickle");
        return false;
    }

    QJsonParseError parseError;
    const QByteArray json = header.mid(8, int(jsonSize));
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Invalid ASAR JSON header: %1").arg(parseError.errorString());
        return false;
    }
    const QJsonObject root = document.object();
    if (!root.value(QStringLiteral("files")).isObject()) {
        if (error) *error = QStringLiteral("Invalid ASAR root directory");
        return false;
    }

    if (out) {
        out->root = root;
        out->dataOffset = std::int64_t(8) + headerSize;
    }
    return true;
}

struct NodeTable {
    QHash<QString, QJsonObject> nodes;
    QVector<QString> orderedPaths;
};

bool collectNodes(const QJsonObject& directory, const QString& prefix, int depth,
                  NodeTable* table, QString* error)
{
    if (!table || depth > kMaxDepth) {
        if (error) *error = QStringLiteral("ASAR directory nesting is too deep");
        return false;
    }
    const QJsonValue filesValue = directory.value(QStringLiteral("files"));
    if (!filesValue.isObject()) {
        if (error) *error = QStringLiteral("Invalid ASAR directory entry");
        return false;
    }

    const QJsonObject files = filesValue.toObject();
    for (QJsonObject::const_iterator it = files.constBegin(); it != files.constEnd(); ++it) {
        const QString name = it.key();
        if (!validEntryName(name) || !it.value().isObject()) {
            if (error) *error = QStringLiteral("Invalid ASAR entry name or record");
            return false;
        }
        const QString path = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
        const QJsonObject node = it.value().toObject();
        if (table->nodes.size() >= kMaxEntries) {
            if (error) *error = QStringLiteral("ASAR contains too many entries");
            return false;
        }
        table->nodes.insert(path, node);

        if (node.contains(QStringLiteral("link"))) {
            if (!node.value(QStringLiteral("link")).isString() ||
                node.value(QStringLiteral("link")).toString().isEmpty()) {
                if (error) *error = QStringLiteral("Invalid ASAR symbolic link");
                return false;
            }
            table->orderedPaths.push_back(path);
        } else if (node.contains(QStringLiteral("files"))) {
            if (!collectNodes(node, path, depth + 1, table, error)) return false;
        } else if (node.contains(QStringLiteral("offset")) ||
                   (node.value(QStringLiteral("unpacked")).toBool(false) &&
                    node.contains(QStringLiteral("size")))) {
            quint64 size = 0;
            if (!numericSize(node, &size)) {
                if (error) *error = QStringLiteral("Invalid ASAR file size at %1").arg(path);
                return false;
            }
            if (node.contains(QStringLiteral("offset"))) {
                quint64 offset = 0;
                if (!numericOffset(node, &offset)) {
                    if (error) *error = QStringLiteral("Invalid ASAR file offset at %1").arg(path);
                    return false;
                }
            }
            if (node.contains(QStringLiteral("unpacked")) &&
                !node.value(QStringLiteral("unpacked")).isBool()) {
                if (error) *error = QStringLiteral("Invalid ASAR unpacked flag at %1").arg(path);
                return false;
            }
            table->orderedPaths.push_back(path);
        } else {
            if (error) *error = QStringLiteral("Unknown ASAR entry type at %1").arg(path);
            return false;
        }
    }
    return true;
}

QString normalizedLink(QString link)
{
    link.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (link.startsWith(QLatin1Char('/'))) link.remove(0, 1);
    const QStringList parts = link.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList clean;
    for (const QString& part : parts) {
        if (part == QStringLiteral(".")) continue;
        if (part == QStringLiteral("..")) return QString();
        clean.push_back(part);
    }
    return clean.join(QLatin1Char('/'));
}

bool resolveLink(const QString& originalPath, const NodeTable& table,
                 QString* resolvedPath, QJsonObject* resolvedNode, QString* error)
{
    QString path = originalPath;
    QSet<QString> visited;
    for (int depth = 0; depth <= kMaxLinkDepth; ++depth) {
        if (visited.contains(path)) {
            if (error) *error = QStringLiteral("Circular ASAR symbolic link at %1").arg(originalPath);
            return false;
        }
        visited.insert(path);
        const QHash<QString, QJsonObject>::const_iterator found = table.nodes.constFind(path);
        if (found == table.nodes.constEnd()) {
            if (error) *error = QStringLiteral("Broken ASAR symbolic link at %1").arg(originalPath);
            return false;
        }
        const QJsonObject node = found.value();
        if (!node.contains(QStringLiteral("link"))) {
            if (resolvedPath) *resolvedPath = path;
            if (resolvedNode) *resolvedNode = node;
            return true;
        }
        path = normalizedLink(node.value(QStringLiteral("link")).toString());
        if (path.isEmpty()) {
            if (error) *error = QStringLiteral("Unsafe ASAR symbolic link at %1").arg(originalPath);
            return false;
        }
    }
    if (error) *error = QStringLiteral("Too many ASAR symbolic link levels at %1").arg(originalPath);
    return false;
}

QString sidecarPath(const QString& physicalPath, const QString& archivePath)
{
    if (physicalPath.isEmpty()) return QString();
    const QString root = physicalPath + QStringLiteral(".unpacked");
    QString native = archivePath;
    native.replace(QLatin1Char('/'), QDir::separator());
    return QDir(root).filePath(native);
}

} // namespace

bool AsarModule::isHeader(const QByteArray& data)
{
    // Match Chromium Pickle framing, but do not require JSON.stringify() to
    // place the root "files" property first. @electron/asar itself only
    // requires a JSON object containing "files", so an exact byte-prefix
    // check rejects otherwise valid archives produced by compatible packers.
    if (data.size() < 17) return false;
    const quint32 sizePicklePayload = le32(data, 0);
    const quint32 headerSize = le32(data, 4);
    const quint32 headerPicklePayload = le32(data, 8);
    const quint32 jsonSize = le32(data, 12);
    if (sizePicklePayload != kPickleUInt32PayloadSize || headerSize < 8 ||
        headerSize > kMaxHeaderSize || (headerSize & 3U) != 0 ||
        headerPicklePayload + 4U != headerSize || jsonSize < 2 ||
        jsonSize > headerPicklePayload - 4U)
        return false;

    const int available = qMin<int>(int(jsonSize), data.size() - 16);
    int i = 0;
    while (i < available) {
        const char c = data.at(16 + i);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        ++i;
    }
    return i < available && data.at(16 + i) == '{';
}

std::unique_ptr<AsarModule> AsarModule::open(const QString& filePath)
{
    ModulePtr base = openStore(storeForFile(filePath), filePath, filePath);
    return std::unique_ptr<AsarModule>(static_cast<AsarModule*>(base.release()));
}

std::unique_ptr<AsarModule> AsarModule::open(const QString& physicalPath, const QString& logicalName)
{
    ModulePtr base = openStore(storeForFile(physicalPath), logicalName, physicalPath);
    return std::unique_ptr<AsarModule>(static_cast<AsarModule*>(base.release()));
}

std::unique_ptr<AsarModule> AsarModule::open(const QByteArray& data, const QString& logicalName)
{
    fs::ByteStorePtr store = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
    ModulePtr base = openStore(store, logicalName, QString());
    return std::unique_ptr<AsarModule>(static_cast<AsarModule*>(base.release()));
}

ModulePtr AsarModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    return openStore(file, sourceName, QString());
}

ModulePtr AsarModule::openStore(const fs::ByteStorePtr& file, const QString& sourceName,
                                const QString& physicalPath)
{
    auto module = peare::makeUnique<AsarModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ASAR;
    module->info_.description = QStringLiteral("Electron ASAR archive");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open ASAR archive");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    HeaderInfo header;
    if (!parseHeader(file, &header, &module->info_.error))
        return ModulePtr(std::move(module));

    NodeTable table;
    if (!collectNodes(header.root, QString(), 0, &table, &module->info_.error))
        return ModulePtr(std::move(module));

    int index = 0;
    int unavailableUnpacked = 0;
    int unavailableLinks = 0;
    for (const QString& path : table.orderedPaths) {
        const QJsonObject original = table.nodes.value(path);
        QJsonObject node = original;
        QString resolvedPath = path;

        const bool originalLink = original.contains(QStringLiteral("link"));
        const bool originalUnpacked = original.value(QStringLiteral("unpacked")).toBool(false);
        if (originalLink && !originalUnpacked) {
            QString linkError;
            if (!resolveLink(path, table, &resolvedPath, &node, &linkError)) {
                ++unavailableLinks;
                continue;
            }
            if (node.contains(QStringLiteral("files"))) {
                ++unavailableLinks;
                continue; // Directory symlinks have no byte payload in Peare's resource model.
            }
        }

        quint64 size = 0;
        if (!numericSize(node, &size)) {
            if (originalLink && originalUnpacked) {
                const QString unpacked = sidecarPath(physicalPath, path);
                const QFileInfo fi(unpacked);
                if (!fi.exists() || !fi.isFile()) {
                    ++unavailableUnpacked;
                    continue;
                }
                size = quint64(fi.size());
            } else {
                ++unavailableLinks;
                continue;
            }
        }

        fs::ByteStorePtr content;
        quint64 dataOffset = 0;
        const bool unpacked = originalUnpacked || node.value(QStringLiteral("unpacked")).toBool(false);
        if (unpacked) {
            const QString unpackedFile = sidecarPath(physicalPath, originalLink ? path : resolvedPath);
            if (unpackedFile.isEmpty()) {
                ++unavailableUnpacked;
                continue;
            }
            content = storeForFile(unpackedFile);
            if (!content) {
                ++unavailableUnpacked;
                continue;
            }
            // @electron/asar extracts the sidecar bytes as they exist on disk;
            // it does not reject an archive because the recorded size drifted.
            size = quint64(content->capacity());
        } else {
            quint64 relativeOffset = 0;
            if (!numericOffset(node, &relativeOffset)) {
                module->info_.error = QStringLiteral("Invalid ASAR file offset at %1").arg(path);
                return ModulePtr(std::move(module));
            }
            if (relativeOffset > quint64(std::numeric_limits<std::int64_t>::max()) ||
                size > quint64(std::numeric_limits<std::int64_t>::max())) {
                module->info_.error = QStringLiteral("ASAR file range is too large at %1").arg(path);
                return ModulePtr(std::move(module));
            }
            const std::int64_t rel = std::int64_t(relativeOffset);
            const std::int64_t len = std::int64_t(size);
            if (header.dataOffset > file->capacity() || rel > file->capacity() - header.dataOffset ||
                len > file->capacity() - header.dataOffset - rel) {
                module->info_.error = QStringLiteral("Truncated ASAR file data at %1").arg(path);
                return ModulePtr(std::move(module));
            }
            const std::int64_t absolute = header.dataOffset + rel;
            dataOffset = quint64(absolute);
            content = std::make_shared<fs::SubStore>(file, absolute, len);
        }

        ResourceEntry entry;
        entry.type = originalLink ? QStringLiteral("ASAR_LINK") : QStringLiteral("ASAR_FILE");
        entry.isEmbeddedFile = true;
        entry.name = safeLeaf(path, index++);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = dataOffset;
        entry.dataSize = size;
        entry.format = ModuleFormat::ASAR;
        entry.hierarchyPath = hierarchyFor(path);
        entry.content = content;
        module->resources_.push_back(std::move(entry));
    }

    if (unavailableUnpacked > 0 || unavailableLinks > 0) {
        QStringList notes;
        if (unavailableUnpacked > 0)
            notes << QStringLiteral("%1 unpacked file(s) unavailable").arg(unavailableUnpacked);
        if (unavailableLinks > 0)
            notes << QStringLiteral("%1 link(s) not exposed").arg(unavailableLinks);
        module->info_.description += QStringLiteral(" (%1)").arg(notes.join(QStringLiteral(", ")));
    }

    return ModulePtr(std::move(module));
}

} // namespace peare
