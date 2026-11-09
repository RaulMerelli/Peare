#include "IsoImageWriter.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <vector>

namespace peare {
namespace writer {
namespace {

const quint32 kSectorSize = 2048;
const quint32 kFirstDataSector = 16;

struct Node {
    QString name;
    QString path;
    QString sourcePath;
    QByteArray primaryName;
    bool directory = true;
    bool explicitEntry = false;
    quint32 size = 0;
    quint32 primaryExtent = 0;
    quint32 jolietExtent = 0;
    quint32 primarySize = 0;
    quint32 jolietSize = 0;
    quint32 fileExtent = 0;
    quint32 primaryId = 0;
    Node* parent = nullptr;
    std::map<QString, std::unique_ptr<Node>> children;
};

quint32 sectorsFor(quint64 bytes) {
    return quint32((bytes + kSectorSize - 1) / kSectorSize);
}

void put16le(QByteArray* out, int offset, quint16 value) {
    (*out)[offset] = char(value);
    (*out)[offset + 1] = char(value >> 8);
}

void put16be(QByteArray* out, int offset, quint16 value) {
    (*out)[offset] = char(value >> 8);
    (*out)[offset + 1] = char(value);
}

void put32le(QByteArray* out, int offset, quint32 value) {
    for (int i = 0; i < 4; ++i) (*out)[offset + i] = char(value >> (i * 8));
}

void put32be(QByteArray* out, int offset, quint32 value) {
    for (int i = 0; i < 4; ++i) (*out)[offset + i] = char(value >> ((3 - i) * 8));
}

void putBoth16(QByteArray* out, int offset, quint16 value) {
    put16le(out, offset, value);
    put16be(out, offset + 2, value);
}

void putBoth32(QByteArray* out, int offset, quint32 value) {
    put32le(out, offset, value);
    put32be(out, offset + 4, value);
}

bool validJolietName(const QString& name, int maxUnits) {
    if (name.isEmpty() || name.size() > maxUnits || name == QStringLiteral(".") ||
        name == QStringLiteral("..")) return false;
    for (int i = 0; i < name.size(); ++i) {
        const ushort ch = name.at(i).unicode();
        if (ch == 0 || ch == '/' || ch == '\\' || ch < 0x20) return false;
        if (ch >= 0xd800 && ch <= 0xdbff) {
            if (i + 1 >= name.size()) return false;
            const ushort low = name.at(++i).unicode();
            if (low < 0xdc00 || low > 0xdfff) return false;
            return false;
        }
        if (ch >= 0xdc00 && ch <= 0xdfff) return false;
    }
    return true;
}

QByteArray jolietIdentifier(const QString& name, bool directory) {
    QString value = name;
    if (!directory) value += QStringLiteral(";1");
    QByteArray out;
    out.reserve(value.size() * 2);
    for (QChar ch : value) {
        out.append(char(ch.unicode() >> 8));
        out.append(char(ch.unicode()));
    }
    return out;
}

QByteArray primaryIdentifier(const Node& node) {
    if (node.path.isEmpty()) return QByteArray(1, '\0');
    if (node.path == QStringLiteral("..")) return QByteArray(1, '\1');
    QByteArray name = node.primaryName;
    if (!node.directory) name.append(";1");
    return name;
}

QByteArray primaryName(const QString& name, bool directory,
                       const std::map<QString, std::unique_ptr<Node>>& siblings) {
    QString stem = name;
    QString extension;
    if (!directory) {
        const int dot = name.lastIndexOf(QLatin1Char('.'));
        if (dot > 0) {
            stem = name.left(dot);
            extension = name.mid(dot + 1);
        }
    }
    auto clean = [](const QString& value) {
        QByteArray result;
        for (QChar ch : value.toUpper()) {
            const ushort code = ch.unicode();
            result.append((code >= 'A' && code <= 'Z') ||
                          (code >= '0' && code <= '9') || code == '_'
                              ? char(code) : '_');
        }
        if (result.isEmpty()) result = "_";
        return result;
    };
    QByteArray base = clean(stem).left(8);
    QByteArray ext = clean(extension).left(3);
    auto make = [&](quint32 suffix) {
        QByteArray candidateBase = base;
        if (suffix) {
            const QByteArray digits = QByteArray::number(suffix);
            candidateBase = base.left(qMax(1, 8 - digits.size())) + digits;
        }
        return directory || ext.isEmpty() ? candidateBase
                                           : candidateBase + '.' + ext;
    };
    for (quint32 suffix = 0; suffix < UINT32_MAX; ++suffix) {
        const QByteArray candidate = make(suffix);
        bool collision = false;
        for (const auto& item : siblings) {
            if (item.second->primaryName == candidate) {
                collision = true;
                break;
            }
        }
        if (!collision) return candidate;
    }
    return QByteArray();
}

int recordSize(int identifierLength) {
    return 33 + identifierLength + ((identifierLength & 1) == 0 ? 1 : 0);
}

QByteArray directoryRecord(const QByteArray& identifier, quint32 extent,
                           quint32 size, bool directory) {
    QByteArray record(recordSize(identifier.size()), '\0');
    record[0] = char(record.size());
    record[1] = 0;
    putBoth32(&record, 2, extent);
    putBoth32(&record, 10, size);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const int year = now.date().year() - 1900;
    record[18] = char(year < 0 ? 0 : year > 255 ? 255 : year);
    record[19] = char(now.date().month());
    record[20] = char(now.date().day());
    record[21] = char(now.time().hour());
    record[22] = char(now.time().minute());
    record[23] = char(now.time().second());
    record[24] = 0;
    record[25] = directory ? char(2) : char(0);
    putBoth16(&record, 28, 1);
    record[32] = char(identifier.size());
    std::copy(identifier.cbegin(), identifier.cend(), record.begin() + 33);
    return record;
}

QByteArray directoryIdentifier(const Node& node, bool joliet) {
    if (node.path.isEmpty()) return QByteArray(1, '\0');
    if (node.path == QStringLiteral("..")) return QByteArray(1, '\1');
    return joliet ? jolietIdentifier(node.name, node.directory)
                  : primaryIdentifier(node);
}

QByteArray serializeDirectory(const Node& node, bool joliet,
                              quint32 extent, quint32 parentExtent,
                              quint32 directorySize) {
    QByteArray out;
    auto append = [&](const QByteArray& record) {
        const int offset = out.size() % int(kSectorSize);
        if (offset + record.size() > int(kSectorSize))
            out.append(QByteArray(int(kSectorSize) - offset, '\0'));
        out.append(record);
    };
    append(directoryRecord(QByteArray(1, '\0'), extent, directorySize, true));
    append(directoryRecord(QByteArray(1, '\1'), parentExtent, directorySize, true));
    for (const auto& item : node.children) {
        const Node& child = *item.second;
        const quint32 childExtent = child.directory
            ? (joliet ? child.jolietExtent : child.primaryExtent) : child.fileExtent;
        const quint32 childSize = child.directory
            ? (joliet ? child.jolietSize : child.primarySize) : child.size;
        append(directoryRecord(directoryIdentifier(child, joliet), childExtent,
                              childSize, child.directory));
    }
    const int tailPadding = int((kSectorSize -
        (quint64(out.size()) % kSectorSize)) % kSectorSize);
    if (tailPadding) out.append(QByteArray(tailPadding, '\0'));
    return out;
}

QByteArray pathTable(const std::vector<Node*>& nodes, bool joliet, bool bigEndian) {
    QByteArray out;
    std::map<const Node*, quint16> indices;
    indices[nodes.front()] = 1;
    for (std::size_t i = 1; i < nodes.size(); ++i)
        indices[nodes[i]] = quint16(i + 1);
    for (Node* node : nodes) {
        const QByteArray identifier = node == nodes.front()
            ? QByteArray(1, '\0')
            : (joliet ? jolietIdentifier(node->name, true)
                      : primaryIdentifier(*node));
        const int length = 8 + identifier.size() + ((identifier.size() & 1) ? 1 : 0);
        const int start = out.size();
        out.append(QByteArray(length, '\0'));
        out[start] = char(identifier.size());
        const quint32 extent = joliet ? node->jolietExtent : node->primaryExtent;
        if (bigEndian) {
            put32be(&out, start + 2, extent);
            put16be(&out, start + 6, node == nodes.front()
                ? 1 : indices[node->parent]);
        } else {
            put32le(&out, start + 2, extent);
            put16le(&out, start + 6, node == nodes.front()
                ? 1 : indices[node->parent]);
        }
        std::copy(identifier.cbegin(), identifier.cend(), out.begin() + start + 8);
    }
    return out;
}

void collectDirectories(Node* node, Node* parent, std::vector<Node*>* output) {
    node->parent = parent;
    if (!node->directory) return;
    output->push_back(node);
    for (auto& item : node->children)
        if (item.second->directory)
            collectDirectories(item.second.get(), node, output);
}

quint64 directoryBytes(const Node& node, bool joliet) {
    quint64 position = 0;
    auto account = [&](int size) {
        const quint64 offset = position % kSectorSize;
        if (offset + quint64(size) > kSectorSize)
            position += kSectorSize - offset;
        position += quint64(size);
    };
    account(recordSize(1));
    account(recordSize(1));
    for (const auto& item : node.children) {
        const Node& child = *item.second;
        const int idLength = directoryIdentifier(child, joliet).size();
        account(recordSize(idLength));
    }
    return ((position + kSectorSize - 1) / kSectorSize) * kSectorSize;
}

void setVolumeIdentifier(QByteArray* descriptor, const QString& label, bool joliet) {
    const QString value = label.left(16).toUpper();
    for (int i = 0; i < 32; ++i) (*descriptor)[40 + i] = ' ';
    if (joliet) {
        for (int i = 0; i < 32; i += 2) {
            (*descriptor)[40 + i] = 0;
            (*descriptor)[41 + i] = ' ';
        }
        for (int i = 0; i < value.size(); ++i) {
            (*descriptor)[40 + i * 2] = char(value.at(i).unicode() >> 8);
            (*descriptor)[41 + i * 2] = char(value.at(i).unicode());
        }
    } else {
        const QByteArray ascii = value.toLatin1();
        std::copy(ascii.cbegin(), ascii.cend(), descriptor->begin() + 40);
    }
}

QByteArray volumeDescriptor(quint8 type, quint32 volumeSectors,
                            quint32 pathTableSize, quint32 pathL,
                            quint32 pathM, quint32 rootExtent,
                            quint32 rootSize, bool joliet) {
    QByteArray out(int(kSectorSize), '\0');
    out[0] = char(type);
    const QByteArray standardId = QByteArrayLiteral("CD001");
    std::copy(standardId.cbegin(), standardId.cend(), out.begin() + 1);
    out[6] = char(1);
    for (int i = 8; i < 40; ++i) out[i] = ' ';
    if (joliet) {
        out[88] = '%'; out[89] = '/'; out[90] = 'E';
    }
    setVolumeIdentifier(&out, QStringLiteral("PEARE"), joliet);
    putBoth32(&out, 80, volumeSectors);
    putBoth16(&out, 120, 1);
    putBoth16(&out, 124, 1);
    putBoth16(&out, 128, quint16(kSectorSize));
    putBoth32(&out, 132, pathTableSize);
    put32le(&out, 140, pathL);
    put32be(&out, 148, pathM);
    const QByteArray root = directoryRecord(QByteArray(1, '\0'), rootExtent,
                                            rootSize, true);
    std::copy(root.cbegin(), root.cend(), out.begin() + 156);
    return out;
}

bool appendSectorPadded(QIODevice* output, const QByteArray& bytes) {
    if (output->write(bytes) != bytes.size()) return false;
    const int padding = int((kSectorSize - (bytes.size() % kSectorSize)) % kSectorSize);
    return !padding || output->write(QByteArray(padding, '\0')) == padding;
}

bool insertEntry(Node* root, const IsoImageEntry& entry, quint32* id,
                 QString* error) {
    QString normalized = entry.path;
    normalized.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (normalized.startsWith(QStringLiteral("./"))) normalized.remove(0, 2);
    if (normalized.startsWith(QLatin1Char('/')) || normalized.isEmpty()) {
        if (error) *error = QStringLiteral("ISO path is absolute or empty.");
        return false;
    }
    const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    Node* node = root;
    QString full;
    for (int i = 0; i < parts.size(); ++i) {
        const QString& part = parts[i];
        const bool last = i + 1 == parts.size();
        const int maxUnits = last && !entry.directory ? 62 : 64;
        if (!validJolietName(part, maxUnits)) {
            if (error) *error = QStringLiteral("ISO Joliet file names must fit 62 UTF-16 units and directory names 64.");
            return false;
        }
        full = full.isEmpty() ? part : full + QLatin1Char('/') + part;
        const QString key = part.toCaseFolded();
        auto found = node->children.find(key);
        if (found == node->children.end()) {
            std::unique_ptr<Node> child(new Node);
            child->name = part;
            child->path = full;
            child->primaryId = ++*id;
            child->directory = !last || entry.directory;
            child->primaryName = primaryName(part, child->directory, node->children);
            if (child->primaryName.isEmpty()) {
                if (error) *error = QStringLiteral("Could not assign a unique ISO 9660 name.");
                return false;
            }
            found = node->children.emplace(key, std::move(child)).first;
        }
        node = found->second.get();
        if (!last && !node->directory) {
            if (error) *error = QStringLiteral("An ISO file blocks a parent directory.");
            return false;
        }
        if (last) {
            if (node->path != full || node->explicitEntry ||
                (!entry.directory && !node->children.empty()) ||
                (entry.directory && !node->directory)) {
                if (error) *error = QStringLiteral("The ISO path is duplicated or conflicts with a file.");
                return false;
            }
            node->directory = entry.directory;
            node->sourcePath = entry.sourcePath;
            node->path = full;
            node->explicitEntry = true;
            if (entry.directory && !entry.sourcePath.isEmpty()) {
                if (error) *error = QStringLiteral("Invalid ISO directory source.");
                return false;
            }
        }
    }
    return true;
}

bool writeZeros(QIODevice* output, quint64 count) {
    const QByteArray zeros(int(kSectorSize), '\0');
    while (count) {
        const qint64 chunk = qMin<quint64>(count, quint64(zeros.size()));
        if (output->write(zeros.constData(), chunk) != chunk) return false;
        count -= quint64(chunk);
    }
    return true;
}

}  // namespace

bool IsoImageWriter::write(const QString& outputPath,
                           const std::vector<IsoImageEntry>& entries,
                           QString* error) {
    Node root;
    quint32 nextId = 0;
    for (const IsoImageEntry& entry : entries)
        if (!insertEntry(&root, entry, &nextId, error)) return false;

    std::vector<Node*> directories;
    collectDirectories(&root, nullptr, &directories);
    std::vector<Node*> files;
    std::function<void(Node*)> collectFiles = [&](Node* node) {
        for (auto& item : node->children) {
            if (item.second->directory) collectFiles(item.second.get());
            else files.push_back(item.second.get());
        }
    };
    collectFiles(&root);
    if (directories.size() > std::numeric_limits<quint16>::max() ||
        nextId > 0x0fffffffu) {
        if (error) *error = QStringLiteral("ISO directory or identifier limits were exceeded.");
        return false;
    }
    for (Node* file : files) {
        QFile input(file->sourcePath);
        const QFileInfo info(file->sourcePath);
        if (!info.isFile() || !input.open(QIODevice::ReadOnly) ||
            input.size() < 0 || quint64(input.size()) > UINT32_MAX) {
            if (error) *error = input.errorString().isEmpty()
                ? QStringLiteral("ISO files must be regular files smaller than 4 GiB.")
                : input.errorString();
            return false;
        }
        file->size = quint32(input.size());
    }

    quint64 sector = kFirstDataSector + 3;
    const quint32 primaryPathSize = [&] {
        QByteArray bytes = pathTable(directories, false, false);
        return quint32(bytes.size());
    }();
    const quint32 jolietPathSize = quint32(pathTable(directories, true, false).size());
    const quint64 primaryL64 = sector;
    sector += sectorsFor(primaryPathSize);
    const quint64 primaryM64 = sector;
    sector += sectorsFor(primaryPathSize);
    const quint64 jolietL64 = sector;
    sector += sectorsFor(jolietPathSize);
    const quint64 jolietM64 = sector;
    sector += sectorsFor(jolietPathSize);
    if (sector > UINT32_MAX) {
        if (error) *error = QStringLiteral("ISO path tables exceed the 32-bit sector limit.");
        return false;
    }

    for (Node* directory : directories) {
        if (sector > UINT32_MAX) {
            if (error) *error = QStringLiteral("ISO directory extents exceed the 32-bit sector limit.");
            return false;
        }
        directory->primaryExtent = quint32(sector);
        directory->primarySize = quint32(directoryBytes(*directory, false));
        sector += sectorsFor(directory->primarySize);
    }
    for (Node* directory : directories) {
        if (sector > UINT32_MAX) {
            if (error) *error = QStringLiteral("Joliet directory extents exceed the 32-bit sector limit.");
            return false;
        }
        directory->jolietExtent = quint32(sector);
        directory->jolietSize = quint32(directoryBytes(*directory, true));
        sector += sectorsFor(directory->jolietSize);
    }
    for (Node* file : files) {
        if (sector > UINT32_MAX) {
            if (error) *error = QStringLiteral("ISO file extents exceed the 32-bit sector limit.");
            return false;
        }
        file->fileExtent = quint32(sector);
        sector += qMax<quint32>(1, sectorsFor(file->size));
    }
    if (sector < kFirstDataSector || sector > UINT32_MAX) {
        if (error) *error = QStringLiteral("ISO image size exceeds supported limits.");
        return false;
    }
    const quint32 volumeSectors = quint32(sector);
    const quint32 primaryL = quint32(primaryL64);
    const quint32 primaryM = quint32(primaryM64);
    const quint32 jolietL = quint32(jolietL64);
    const quint32 jolietM = quint32(jolietM64);

    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        if (error) *error = output.errorString();
        return false;
    }
    QString failureContext = QStringLiteral("Could not write the ISO system area.");
    if (!writeZeros(&output, quint64(kFirstDataSector) * kSectorSize))
        goto io_error;
    {
        const QByteArray pvd = volumeDescriptor(1, volumeSectors, primaryPathSize,
            primaryL, primaryM, root.primaryExtent, root.primarySize, false);
        const QByteArray svd = volumeDescriptor(2, volumeSectors, jolietPathSize,
            jolietL, jolietM, root.jolietExtent, root.jolietSize, true);
        QByteArray terminator(int(kSectorSize), '\0');
        terminator[0] = char(255);
        const QByteArray standardId = QByteArrayLiteral("CD001");
        std::copy(standardId.cbegin(), standardId.cend(), terminator.begin() + 1);
        terminator[6] = char(1);
        failureContext = QStringLiteral("Could not write the ISO volume descriptors.");
        if (output.write(pvd) != pvd.size() || output.write(svd) != svd.size() ||
            output.write(terminator) != terminator.size()) goto io_error;
    }
    failureContext = QStringLiteral("Could not write the ISO path tables.");
    if (!appendSectorPadded(&output, pathTable(directories, false, false)) ||
        !appendSectorPadded(&output, pathTable(directories, false, true)) ||
        !appendSectorPadded(&output, pathTable(directories, true, false)) ||
        !appendSectorPadded(&output, pathTable(directories, true, true))) goto io_error;
    for (Node* directory : directories) {
        failureContext = QStringLiteral("Could not write the primary ISO directory tree.");
        const quint32 parent = directory->parent
            ? directory->parent->primaryExtent : directory->primaryExtent;
        const QByteArray bytes = serializeDirectory(*directory, false,
            directory->primaryExtent, parent, directory->primarySize);
        if (output.write(bytes) != bytes.size()) goto io_error;
    }
    for (Node* directory : directories) {
        failureContext = QStringLiteral("Could not write the Joliet directory tree.");
        const quint32 parent = directory->parent
            ? directory->parent->jolietExtent : directory->jolietExtent;
        const QByteArray bytes = serializeDirectory(*directory, true,
            directory->jolietExtent, parent, directory->jolietSize);
        if (output.write(bytes) != bytes.size()) goto io_error;
    }
    for (Node* file : files) {
        failureContext = QStringLiteral("The ISO file extent does not match its assigned sector.");
        if (output.pos() != qint64(file->fileExtent) * kSectorSize) {
            failureContext = QStringLiteral("ISO file extent mismatch: output %1, assigned %2.")
                .arg(output.pos()).arg(qint64(file->fileExtent) * kSectorSize);
            goto io_error;
        }
        QFile input(file->sourcePath);
        failureContext = QStringLiteral("Could not reopen staged ISO file data.");
        if (!input.open(QIODevice::ReadOnly)) goto io_error;
        QByteArray buffer(int(kSectorSize * 256), '\0');
        qint64 remaining = file->size;
        failureContext = QStringLiteral("Could not write staged ISO file data.");
        while (remaining) {
            const qint64 request = qMin<qint64>(remaining, buffer.size());
            const qint64 count = input.read(buffer.data(), request);
            if (count != request || output.write(buffer.constData(), count) != count)
                goto io_error;
            remaining -= count;
        }
        const qint64 padding = qint64(qMax<quint32>(1, sectorsFor(file->size))) *
                               kSectorSize - file->size;
        failureContext = QStringLiteral("Could not pad the ISO file extent.");
        if (padding && !writeZeros(&output, quint64(padding))) goto io_error;
    }
    if (!output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;

io_error:
    if (error) *error = output.errorString().isEmpty() ||
        output.errorString() == QStringLiteral("Unknown error")
        ? failureContext : output.errorString();
    output.cancelWriting();
    return false;
}

}  // namespace writer
}  // namespace peare
