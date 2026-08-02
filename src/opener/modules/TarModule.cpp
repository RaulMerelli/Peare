#include "TarModule.h"
#include "Compat.h"

#include <QFile>
#include <QStringList>

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace peare {
namespace {

const std::int64_t kTarBlock = 512;

std::int64_t roundUp(std::int64_t value, std::int64_t unit) {
    return ((value + unit - 1) / unit) * unit;
}

std::int64_t octal(const std::uint8_t* p, int n) {
    std::int64_t value = 0;
    for (int i = 0; i < n; ++i) {
        if (p[i] == 0 || p[i] == ' ') continue;
        if (p[i] < '0' || p[i] > '7') break;
        value = value * 8 + (p[i] - '0');
    }
    return value;
}

QString tarString(const std::uint8_t* p, int n) {
    int len = 0;
    while (len < n && p[len] != 0) ++len;
    return QString::fromUtf8(reinterpret_cast<const char*>(p), len);
}

bool zeroBlock(const std::vector<std::uint8_t>& h) {
    for (std::uint8_t b : h)
        if (b != 0) return false;
    return true;
}

bool validTarHeader(const std::vector<std::uint8_t>& h) {
    if (h.size() < kTarBlock || zeroBlock(h)) return false;
    unsigned int sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? ' ' : h[std::size_t(i)];
    const std::int64_t stored = octal(h.data() + 148, 8);
    if (stored <= 0 || stored != std::int64_t(sum)) return false;
    const QString name = tarString(h.data(), 100);
    return !name.isEmpty();
}

QString normalizeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    return name;
}

QString safeLeaf(QString name, int index) {
    name = normalizeName(name);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString leaf = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i) {
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    }
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("tar_file_%1.bin").arg(index, 3, 10, QLatin1Char('0'))
                          : leaf;
}

QStringList hierarchyFor(QString name) {
    name = normalizeName(name);
    QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

}  // namespace

ModulePtr TarModule::open(const QString& filePath) {
    return open(storeForFile(filePath), filePath);
}

ModulePtr TarModule::open(const QByteArray& data, const QString& logicalName) {
    return open(std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(data.constData()),
                    std::size_t(data.size())),
                logicalName);
}

ModulePtr TarModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<TarModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::TAR;
    module->info_.description = QStringLiteral("TAR archive");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open TAR archive");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    std::map<QString, fs::ByteStorePtr> fileByPath;
    int index = 0;
    for (std::int64_t pos = 0; pos + kTarBlock <= file->capacity();) {
        std::vector<std::uint8_t> h = file->readRange(pos, kTarBlock);
        if (h.size() < kTarBlock) break;
        if (zeroBlock(h)) break;
        if (!validTarHeader(h)) {
            module->info_.error = QStringLiteral("Invalid TAR header");
            return ModulePtr(std::move(module));
        }

        QString name = tarString(h.data(), 100);
        const QString prefix = tarString(h.data() + 345, 155);
        if (!prefix.isEmpty()) name = prefix + QLatin1Char('/') + name;
        name = normalizeName(name);
        const std::int64_t size = octal(h.data() + 124, 12);
        const char type = char(h[156]);
        const QString linkName = normalizeName(tarString(h.data() + 157, 100));
        const std::int64_t dataOffset = pos + kTarBlock;
        if (size < 0 || dataOffset + size > file->capacity()) {
            module->info_.error = QStringLiteral("Truncated TAR file data");
            return ModulePtr(std::move(module));
        }

        if (type == '\0' || type == '0' || type == '7') {
            fs::ByteStorePtr content = std::make_shared<fs::SubStore>(file, dataOffset, size);
            fileByPath[name] = content;
            ResourceEntry entry;
            entry.type = QStringLiteral("TAR_FILE");
            entry.isEmbeddedFile = true;
            entry.name = safeLeaf(name, index++);
            entry.language = QStringLiteral("neutral");
            entry.dataOffset = quint64(dataOffset);
            entry.dataSize = quint64(size);
            entry.format = ModuleFormat::TAR;
            entry.hierarchyPath = hierarchyFor(name);
            entry.content = content;
            module->resources_.push_back(std::move(entry));
        } else if (type == '1' && fileByPath.find(linkName) != fileByPath.end()) {
            fs::ByteStorePtr content = fileByPath[linkName];
            ResourceEntry entry;
            entry.type = QStringLiteral("TAR_FILE");
            entry.isEmbeddedFile = true;
            entry.name = safeLeaf(name, index++);
            entry.language = QStringLiteral("neutral");
            entry.dataOffset = 0;
            entry.dataSize = quint64(content->capacity());
            entry.format = ModuleFormat::TAR;
            entry.hierarchyPath = hierarchyFor(name);
            entry.content = content;
            module->resources_.push_back(std::move(entry));
        }

        pos += kTarBlock + roundUp(size, kTarBlock);
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
