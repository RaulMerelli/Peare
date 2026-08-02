#include "XvaModule.h"
#include "Compat.h"

#include "../fs/PartitionTable.h"

#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kTarBlock = 512;
const std::int64_t kChunk = 1024 * 1024;

struct TarEntry {
    QString name;
    std::int64_t offset = 0;
    std::int64_t size = 0;
    char type = '\0';
};

struct DiskInfo {
    QString id;
    QString label;
    QString location;
    std::int64_t capacity = 0;
};

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

std::vector<TarEntry> readTar(const fs::ByteStorePtr& file, QString* error) {
    std::vector<TarEntry> entries;
    for (std::int64_t pos = 0; pos + kTarBlock <= file->capacity();) {
        std::vector<std::uint8_t> h = file->readRange(pos, kTarBlock);
        if (h.size() < kTarBlock) break;
        bool zero = true;
        for (std::uint8_t b : h) {
            if (b != 0) { zero = false; break; }
        }
        if (zero) break;
        const QString name = tarString(h.data(), 100);
        const QString prefix = tarString(h.data() + 345, 155);
        const std::int64_t size = octal(h.data() + 124, 12);
        const char type = char(h[156]);
        if (name.isEmpty() || size < 0 || pos + kTarBlock + size > file->capacity()) {
            if (error) *error = QStringLiteral("Invalid XVA/TAR entry");
            entries.clear();
            return entries;
        }
        TarEntry e;
        e.name = prefix.isEmpty() ? name : prefix + QLatin1Char('/') + name;
        e.offset = pos + kTarBlock;
        e.size = size;
        e.type = type;
        entries.push_back(e);
        pos += kTarBlock + roundUp(size, kTarBlock);
    }
    return entries;
}

QString entryText(const fs::ByteStorePtr& file, const TarEntry& e) {
    const std::int64_t maxRead = std::min<std::int64_t>(e.size, 16 * 1024 * 1024);
    std::vector<std::uint8_t> bytes = file->readRange(e.offset, maxRead);
    return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
}

QString memberValue(const QString& block, const QString& name) {
    const QRegularExpression re(QStringLiteral("<member>\\s*<name>%1</name>\\s*<value>(.*?)</value>\\s*</member>")
                                    .arg(QRegularExpression::escape(name)),
                                QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch m = re.match(block);
    return m.hasMatch() ? m.captured(1).remove(QRegularExpression(QStringLiteral("<[^>]+>"))).trimmed() : QString();
}

std::vector<DiskInfo> parseDisks(const QString& ova, const std::vector<TarEntry>& entries) {
    std::vector<DiskInfo> disks;
    QRegularExpression objRe(QStringLiteral("<value>\\s*<struct>(.*?)</struct>\\s*</value>"),
                             QRegularExpression::DotMatchesEverythingOption);
    QRegularExpressionMatchIterator it = objRe.globalMatch(ova);
    while (it.hasNext()) {
        const QString block = it.next().captured(1);
        if (memberValue(block, QStringLiteral("class")) != QStringLiteral("VDI"))
            continue;
        DiskInfo d;
        d.id = memberValue(block, QStringLiteral("id"));
        d.label = memberValue(block, QStringLiteral("name_label"));
        d.location = memberValue(block, QStringLiteral("location"));
        bool ok = false;
        d.capacity = memberValue(block, QStringLiteral("virtual_size")).toLongLong(&ok);
        if (d.location.isEmpty()) d.location = d.id;
        if (d.label.isEmpty()) d.label = d.location;
        if (!d.location.isEmpty() && ok && d.capacity > 0)
            disks.push_back(d);
    }
    if (!disks.empty())
        return disks;

    // Fallback for sparse/simple XVAs: infer disk directories from chunk names.
    std::map<QString, int> maxChunk;
    QRegularExpression chunkRe(QStringLiteral("^(.+)/([0-9]{8})$"));
    for (const TarEntry& e : entries) {
        QRegularExpressionMatch m = chunkRe.match(e.name);
        if (m.hasMatch())
            maxChunk[m.captured(1)] = std::max(maxChunk[m.captured(1)], m.captured(2).toInt());
    }
    for (const auto& kv : maxChunk) {
        DiskInfo d;
        d.id = kv.first;
        d.location = kv.first;
        d.label = kv.first;
        d.capacity = std::int64_t(kv.second + 1) * kChunk;
        disks.push_back(d);
    }
    return disks;
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

class XvaDiskStore final : public fs::IByteStore {
public:
    XvaDiskStore(fs::ByteStorePtr file, std::int64_t capacity, QString dir,
                 const std::vector<TarEntry>& entries)
        : file_(std::move(file)), capacity_(capacity), dir_(std::move(dir)) {
        const QString prefix = dir_ + QLatin1Char('/');
        for (const TarEntry& e : entries) {
            if (!e.name.startsWith(prefix))
                continue;
            bool ok = false;
            const int idx = e.name.mid(prefix.size()).toInt(&ok);
            if (!ok)
                continue;
            if (e.size == 0)
                skip_.push_back(idx);
            else
                chunks_[idx] = e;
        }
        std::sort(skip_.begin(), skip_.end());
    }

    std::int64_t capacity() const override { return capacity_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= capacity_) return 0;
        const int want = static_cast<int>(std::min<std::int64_t>(count, capacity_ - pos));
        int produced = 0;
        while (produced < want) {
            const std::int64_t p = pos + produced;
            const int rawIndex = static_cast<int>(p / kChunk);
            const int archiveIndex = correctChunkIndex(rawIndex);
            const std::int64_t inChunk = p % kChunk;
            const int chunk = static_cast<int>(std::min<std::int64_t>(want - produced, kChunk - inChunk));
            auto it = chunks_.find(archiveIndex);
            if (it == chunks_.end() || inChunk >= it->second.size) {
                std::fill(dst + produced, dst + produced + chunk, std::uint8_t(0));
            } else {
                const int readable = static_cast<int>(std::min<std::int64_t>(chunk, it->second.size - inChunk));
                int got = file_->read(it->second.offset + inChunk, dst + produced, readable);
                if (got < chunk)
                    std::fill(dst + produced + got, dst + produced + chunk, std::uint8_t(0));
            }
            produced += chunk;
        }
        return produced;
    }

private:
    int correctChunkIndex(int rawIndex) const {
        int index = rawIndex;
        for (int skip : skip_) {
            if (index >= skip) ++index;
            else break;
        }
        return index;
    }

    fs::ByteStorePtr file_;
    std::int64_t capacity_ = 0;
    QString dir_;
    std::map<int, TarEntry> chunks_;
    std::vector<int> skip_;
};

}  // namespace

ModulePtr XvaModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<XvaModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::XVA;
    module->info_.description = QStringLiteral("Xen Virtual Appliance");
    module->file_ = file;

    QString err;
    std::vector<TarEntry> entries = readTar(file, &err);
    if (!err.isEmpty()) {
        module->info_.error = err;
        return ModulePtr(std::move(module));
    }
    if (entries.empty()) {
        module->info_.error = QStringLiteral("Empty or invalid XVA archive");
        return ModulePtr(std::move(module));
    }

    QString ova;
    for (const TarEntry& e : entries) {
        if (e.name == QStringLiteral("ova.xml")) {
            ova = entryText(file, e);
            break;
        }
    }
    if (ova.isEmpty()) {
        module->info_.error = QStringLiteral("XVA ova.xml not found");
        return ModulePtr(std::move(module));
    }

    const std::vector<DiskInfo> disks = parseDisks(ova, entries);
    if (disks.empty()) {
        module->info_.error = QStringLiteral("No XVA disks found");
        return ModulePtr(std::move(module));
    }

    for (std::size_t di = 0; di < disks.size(); ++di) {
        const DiskInfo& diskInfo = disks[di];
        fs::ByteStorePtr disk = std::make_shared<XvaDiskStore>(file, diskInfo.capacity, diskInfo.location, entries);
        const std::vector<fs::PartitionInfo> parts = fs::readPartitionTable(disk);
        auto addEntry = [&](const QString& name, std::int64_t off, std::int64_t len) {
            ResourceEntry entry;
            entry.type = QStringLiteral("DISK_PARTITION");
            entry.isEmbeddedFile = true;
            entry.name = name;
            entry.language = QStringLiteral("neutral");
            entry.dataOffset = quint64(off);
            entry.dataSize = quint64(len);
            entry.content = std::make_shared<fs::SubStore>(disk, off, len);
            module->resources_.push_back(std::move(entry));
        };
        const QString diskName = diskInfo.label.isEmpty()
            ? QStringLiteral("Disk %1").arg(di + 1)
            : diskInfo.label;
        if (parts.empty()) {
            addEntry(QStringLiteral("%1 - Whole disk").arg(diskName), 0, disk->capacity());
        } else {
            for (std::size_t pi = 0; pi < parts.size(); ++pi) {
                const fs::PartitionInfo& p = parts[pi];
                addEntry(QStringLiteral("%1 - Partition %2 (%3)")
                             .arg(diskName)
                             .arg(pi + 1)
                             .arg(QString::fromStdString(p.typeName)),
                         p.offset, p.length);
            }
        }
    }
    return ModulePtr(std::move(module));
}

ModulePtr XvaModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<XvaModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::XVA;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
