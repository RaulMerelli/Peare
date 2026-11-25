#include "TibModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"
#include "../fs/TibLegacyReader.h"
#include "../fs/TibLegacyFileChunks.h"
#include "../fs/TibOldFileDirectory.h"
#include "../fs/TibFileReader.h"
#include "../fs/TibMultiVolumeStore.h"
#include "../fs/TibHeader.h"
#include "../fs/TibMacReader.h"
#include "../fs/TibCbcStore.h"
#include "../fs/TibCbcBounds.h"
#include "../compression/TibCrypto.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace peare {
namespace {

struct VolumeHeader {
    std::uint64_t archiveId;
    std::uint32_t number;
    std::uint32_t dataOffset;
};

bool readExactStore(const fs::ByteStorePtr& store, std::int64_t pos, std::uint8_t* out, int size) {
    if (!store || pos < 0 || size < 0 || pos > store->capacity() ||
        size > store->capacity() - pos) return false;
    int done = 0;
    while (done < size) {
        const int n = store->read(pos + done, out + done, size - done);
        if (n <= 0 || n > size - done) return false;
        done += n;
    }
    return true;
}

bool volumeHeader(const fs::ByteStorePtr& store, VolumeHeader* result) {
    std::uint8_t old[12];
    if (readExactStore(store, 0, old, sizeof(old)) && fs::tibheader::le32(old) == 0x44686eb4U) {
        result->archiveId = fs::tibheader::le32(old + 4);
        result->number = fs::tibheader::le32(old + 8);
        result->dataOffset = sizeof(old);
        return result->number != 0;
    }
    fs::tibheader::Header header;
    if (!store || !store->validate() || !fs::tibheader::read(store, &header, nullptr)) return false;
    result->dataOffset = header.dataOffset;
    if (result->dataOffset < header.size || result->dataOffset > store->capacity()) return false;
    result->archiveId = header.archiveId;
    result->number = header.number;
    return result->number != 0;
}

class TibPrefixedStore final : public fs::IByteStore {
public:
    TibPrefixedStore(fs::ByteStorePtr prefix, fs::ByteStorePtr data, fs::ByteStorePtr first)
        : prefix_(std::move(prefix)), data_(std::move(data)), first_(std::move(first)),
          size_(prefix_->capacity() + data_->capacity()) {}
    std::int64_t capacity() const override { return size_; }
    bool validate() const override { return first_->validate() && data_->validate(); }
    bool cheapRandomAccess() const override { return data_->cheapRandomAccess(); }
    bool isFileBacked() const { return bool(std::dynamic_pointer_cast<fs::QFileByteStore>(first_)); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!dst || pos < 0 || count <= 0 || pos >= size_) return 0;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
        int done = 0;
        if (pos < prefix_->capacity()) {
            const int n = static_cast<int>(std::min<std::int64_t>(amount, prefix_->capacity() - pos));
            if (!readExactStore(prefix_, pos, dst, n)) return -1;
            done = n;
            pos += n;
        }
        if (done < amount && !readExactStore(data_, pos - prefix_->capacity(), dst + done, amount - done))
            return -1;
        return amount;
    }
private:
    fs::ByteStorePtr prefix_;
    fs::ByteStorePtr data_;
    fs::ByteStorePtr first_;
    std::int64_t size_;
};

class TibLogicalStore final : public fs::IByteStore {
public:
    TibLogicalStore(fs::ByteStorePtr source, std::int64_t base, std::int64_t length)
        : source_(std::move(source)), slice_(std::make_shared<fs::SubStore>(source_, base, length)),
          sourceSize_(source_->capacity()) {}
    std::int64_t capacity() const override { return slice_->capacity(); }
    bool validate() const override { return source_->validate() && source_->capacity() == sourceSize_; }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate()) return -1;
        return slice_->read(pos, dst, count);
    }
private:
    fs::ByteStorePtr source_;
    fs::ByteStorePtr slice_;
    std::int64_t sourceSize_;
};

struct TibOpenedContext {
    std::vector<fs::TibFileIndexEntry> entries;
    fs::TibFileMetadataVersion version;
    fs::ByteStorePtr data;
    std::uint64_t dataOffset = 0;
    std::shared_ptr<fs::TibDedupProvider> provider;
};

bool readVaultKey(const QDir& dir, const QString& password, std::vector<std::uint8_t>* key,
                  bool* encrypted, std::string* error) {
    *encrypted = false;
    QByteArray keyInfo;
    QDir meta(dir.filePath(QStringLiteral(".meta")));
    const QFileInfoList files = meta.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks);
    for (const QFileInfo& info : files) {
        if (info.size() > 1024 * 1024) continue;
        QFile input(info.absoluteFilePath());
        if (!input.open(QIODevice::ReadOnly)) continue;
        QXmlStreamReader xml(&input);
        bool location = false;
        QString method, encoded;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == QLatin1String("metainfo"))
                location = xml.attributes().value(QLatin1String("type")) == QLatin1String("location");
            else if (location && xml.isStartElement() && xml.name() == QLatin1String("encryption_method"))
                method = xml.readElementText();
            else if (location && xml.isStartElement() && xml.name() == QLatin1String("key_info"))
                encoded = xml.readElementText();
            else if (xml.isEndElement() && xml.name() == QLatin1String("metainfo"))
                location = false;
        }
        if (!xml.hasError() && !method.isEmpty() && method != QLatin1String("0")) {
            const QByteArray hex = encoded.toLatin1();
            if (*encrypted || hex.isEmpty() || hex.size() % 2 != 0) {
                if (error) *error = "Invalid TIB vault encryption metadata";
                return false;
            }
            *encrypted = true;
            keyInfo = QByteArray::fromHex(hex);
            if (keyInfo.size() * 2 != hex.size() || keyInfo.size() < 2 || keyInfo.at(0) != char(1)) {
                if (error) *error = "Invalid TIB vault key metadata";
                return false;
            }
        }
    }
    if (!*encrypted) return true;
    QByteArray utf8 = password.toUtf8();
    std::vector<std::uint8_t> passwordBytes(utf8.begin(), utf8.end());
    std::vector<std::uint8_t> records(keyInfo.begin() + 1, keyInfo.end());
    std::size_t consumed = 0;
    const fs::TibKeyResult result = fs::tibRecoverKey(passwordBytes, records,
        std::numeric_limits<std::uint64_t>::max(), key, &consumed);
    std::fill(passwordBytes.begin(), passwordBytes.end(), 0);
    utf8.fill(0);
    if (result != fs::TibKeyResult::Success) {
        if (error) *error = password.isEmpty() ? "TIB vault password required" : "Invalid TIB vault password or key metadata";
        return false;
    }
    return true;
}

class TibCombinedDedupProvider final : public fs::TibDedupProvider {
public:
    explicit TibCombinedDedupProvider(const std::vector<std::shared_ptr<fs::TibDedupProvider> >& providers)
        : providers_(providers) {}
    bool contains(const std::array<std::uint8_t, 16>& hash) const override {
        for (const auto& provider : providers_)
            if (provider->contains(hash)) return true;
        return false;
    }
    bool read(const std::array<std::uint8_t, 16>& hash, std::vector<std::uint8_t>* data) const override {
        if (!data) return false;
        std::vector<std::uint8_t> result;
        bool found = false;
        for (const auto& provider : providers_) {
            if (!provider->contains(hash)) continue;
            std::vector<std::uint8_t> candidate;
            if (!provider->read(hash, &candidate) || (found && candidate != result)) return false;
            result.swap(candidate);
            found = true;
        }
        if (found) data->swap(result);
        return found;
    }
private:
    std::vector<std::shared_ptr<fs::TibDedupProvider> > providers_;
};

std::shared_ptr<fs::TibDedupProvider> findDedupProvider(const QString& name, const QString& password,
                                                        std::string* error, bool disk) {
    const QFileInfo file(name);
    if (!file.isFile()) return std::shared_ptr<fs::TibDedupProvider>();
    QDir dir = file.absoluteDir();
    do {
        std::vector<std::pair<fs::ByteStorePtr, std::int64_t> > stores[2];
        bool invalid[2] = {false, false};
        const QString prefixes[] = {QStringLiteral("unified_data.ds."),
                                    disk ? QStringLiteral("disk_data.ds.") : QStringLiteral("file_data.ds.")};
        for (int group = 0; group != 2; ++group) for (int i = 0; i != 2; ++i) {
            const QString path = dir.filePath(prefixes[group] + QString::number(i));
            if (!QFileInfo::exists(path)) continue;
            fs::ByteStorePtr source = std::make_shared<fs::QFileByteStore>(path);
            const std::int64_t capacity = source->capacity();
            if (!source->validate() || capacity < 0) {
                if (error) *error = "Invalid TIB deduplication store";
                invalid[group] = true;
                continue;
            }
            std::uint64_t used = static_cast<std::uint64_t>(capacity);
            const QString sizePath = path + QStringLiteral(".szh");
            if (QFileInfo::exists(sizePath)) {
                fs::ByteStorePtr sizes = std::make_shared<fs::QFileByteStore>(sizePath);
                std::uint8_t length[8];
                if (!sizes->validate() || sizes->capacity() < 8 || !readExactStore(sizes, 0, length, 8)) {
                    if (error) *error = "Invalid TIB deduplication store size";
                    invalid[group] = true;
                    continue;
                }
                used = std::uint64_t(fs::tibheader::le32(length)) |
                    (std::uint64_t(fs::tibheader::le32(length + 4)) << 32);
            }
            stores[group].push_back(std::make_pair(source,
                used > static_cast<std::uint64_t>(capacity) ? capacity : static_cast<std::int64_t>(used)));
        }
        if (!stores[0].empty() || !stores[1].empty() || invalid[0] || invalid[1]) {
            std::vector<std::uint8_t> key;
            bool encrypted = false;
            if (!readVaultKey(dir, password, &key, &encrypted, error)) return std::shared_ptr<fs::TibDedupProvider>();
            std::vector<std::shared_ptr<fs::TibDedupProvider> > providers;
            for (int group = 0; group != 2; ++group) {
                if (invalid[group] || stores[group].empty()) continue;
                std::string groupError;
                auto provider = fs::openTibDedupProvider(stores[group], &groupError, key);
                if (provider) providers.push_back(provider);
                else if (error) *error = groupError;
            }
            std::fill(key.begin(), key.end(), 0);
            if (providers.empty()) return std::shared_ptr<fs::TibDedupProvider>();
            return std::make_shared<TibCombinedDedupProvider>(providers);
        }
    } while (dir.cdUp());
    if (error) *error = "TIB deduplication store not found";
    return std::shared_ptr<fs::TibDedupProvider>();
}

struct TibLegacyContext {
    fs::ByteStorePtr source;
    fs::ByteStorePtr directorySource;
    std::vector<fs::TibLegacyFileNode> nodes;
    std::uint64_t base;
    std::uint64_t recordLimit;
};

class TibOldContextStore final : public fs::IByteStore {
public:
    TibOldContextStore(fs::ByteStorePtr source, std::shared_ptr<fs::TibOldFileDirectory> directory)
        : source_(std::move(source)), directory_(std::move(directory)) {}
    std::int64_t capacity() const override { return source_->capacity(); }
    bool validate() const override { return source_->validate(); }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return source_->read(pos, dst, count);
    }
    const std::shared_ptr<fs::TibOldFileDirectory>& directory() const { return directory_; }
private:
    fs::ByteStorePtr source_;
    std::shared_ptr<fs::TibOldFileDirectory> directory_;
};

bool openOldDirectory(const std::vector<fs::ByteStorePtr>& parts,
                      std::shared_ptr<fs::TibOldFileDirectory>* directory) {
    if (parts.empty()) return false;
    std::uint8_t first[13];
    if (!readExactStore(parts.front(), 0, first, sizeof(first)) ||
        fs::tibheader::le32(first) != 0x44686eb4U || first[12] != 0x66) return false;
    fs::TibOldFileIndex index;
    fs::TibOldIndexLimits limits;
    const fs::TibOldIndexStatus status = fs::buildTibOldFileIndex(parts, limits, &index);
    bool boundaries = status == fs::TibOldIndexStatus::UnverifiedData && !index.skippedRanges.empty();
    const fs::ByteStorePtr& last = parts.back();
    const std::uint64_t tableSize = std::uint64_t(parts.size()) * 8 + 4;
    if (boundaries && (last->capacity() < 0 || tableSize > std::uint64_t(last->capacity()))) boundaries = false;
    std::uint8_t marker[4];
    if (boundaries && (!readExactStore(last, last->capacity() - 4, marker, 4) ||
                       fs::tibheader::le32(marker) != 0x179631b4U)) boundaries = false;
    for (const auto& skipped : index.skippedRanges) {
        std::uint8_t length[8];
        if (!boundaries || skipped.volumeIndex >= parts.size() || skipped.length != 12 ||
            !readExactStore(last, last->capacity() - static_cast<std::int64_t>(tableSize) +
                            static_cast<std::int64_t>(skipped.volumeIndex * 8), length, 8)) {
            boundaries = false;
            break;
        }
        const std::uint64_t end = std::uint64_t(fs::tibheader::le32(length)) |
            (std::uint64_t(fs::tibheader::le32(length + 4)) << 32);
        if (end != skipped.volumeOffset) { boundaries = false; break; }
    }
    return (status == fs::TibOldIndexStatus::Ok || boundaries) && !index.snapshots.empty() &&
        fs::TibOldFileDirectory::create(index, index.snapshots.size() - 1, parts,
                                        limits.fields, directory) == fs::TibOldDirectoryStatus::Ok;
}

bool listOld(const std::shared_ptr<fs::TibOldFileDirectory>& directory, const fs::ByteStorePtr& source,
             const QString& subPath, QVector<ResourceEntry>* resources, QString* error) {
    std::size_t parent = fs::TibOldFileDirectory::roots();
    if (!subPath.isEmpty()) {
        bool ok = false;
        const qulonglong value = subPath.toULongLong(&ok);
        if (!ok || value >= directory->nodeCount()) { *error = QStringLiteral("Invalid TIB directory"); return false; }
        parent = static_cast<std::size_t>(value);
    }
    std::vector<std::size_t> children;
    if (directory->children(parent, &children) != fs::TibOldDirectoryStatus::Ok) {
        *error = QStringLiteral("Cannot list TIB directory"); return false;
    }
    QSet<QString> names;
    for (std::size_t node : children) {
        fs::TibOldDirectoryEntry item;
        if (directory->entry(node, &item) != fs::TibOldDirectoryStatus::Ok ||
            item.name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            *error = QStringLiteral("Invalid TIB entry"); return false;
        }
        if (item.deleted) continue;
        const QString entryName = QString::fromUtf16(reinterpret_cast<const ushort*>(item.name.data()),
                                                      static_cast<int>(item.name.size()));
        if (entryName.isEmpty() || entryName.contains(QLatin1Char('/')) ||
            entryName.contains(QLatin1Char('\0')) || names.contains(entryName)) {
            *error = QStringLiteral("Invalid or duplicate TIB entry name"); return false;
        }
        names.insert(entryName);
        ResourceEntry entry;
        entry.name = entryName;
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::TIB;
        if (item.directory) {
            entry.type = QStringLiteral("TIB_DIR");
            entry.isDirectory = true;
            entry.content = source;
            entry.containerSubPath = QString::number(static_cast<qulonglong>(node));
        } else {
            entry.type = QStringLiteral("TIB_FILE");
            if (directory->openFile(node, &entry.content) != fs::TibOldDirectoryStatus::Ok) {
                *error = QStringLiteral("Cannot open TIB file"); return false;
            }
            entry.dataSize = static_cast<quint64>(item.size);
            entry.isEmbeddedFile = true;
        }
        resources->push_back(std::move(entry));
    }
    return true;
}

class TibLegacyContextStore final : public fs::IByteStore {
public:
    explicit TibLegacyContextStore(std::shared_ptr<const TibLegacyContext> context) : context_(std::move(context)) {}
    std::int64_t capacity() const override { return context_->source->capacity(); }
    bool validate() const override { return context_->source->validate(); }
    bool cheapRandomAccess() const override { return context_->source->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return validate() ? context_->source->read(pos, dst, count) : -1;
    }
    const std::shared_ptr<const TibLegacyContext>& context() const { return context_; }
private:
    std::shared_ptr<const TibLegacyContext> context_;
};

bool listLegacy(const std::shared_ptr<const TibLegacyContext>& context, const fs::ByteStorePtr& source,
                const QString& subPath, QVector<ResourceEntry>* resources, QString* error) {
    if (!source || !source->validate()) { *error = QStringLiteral("TIB legacy source became invalid"); return false; }
    const std::size_t root = std::numeric_limits<std::size_t>::max();
    std::size_t parent = root;
    if (!subPath.isEmpty()) {
        bool ok = false;
        const qulonglong index = subPath.toULongLong(&ok);
        if (!ok || index >= context->nodes.size()) { *error = QStringLiteral("Invalid TIB legacy directory"); return false; }
        parent = static_cast<std::size_t>(index);
    }
    QSet<QString> names;
    for (std::size_t i = 0; i < context->nodes.size(); ++i) {
        const fs::TibLegacyFileNode& node = context->nodes[i];
        if (node.parent != parent) continue;
        if (node.name.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            *error = QStringLiteral("TIB legacy name is too long"); return false;
        }
        QString name = QString::fromUtf16(reinterpret_cast<const ushort*>(node.name.data()), static_cast<int>(node.name.size()));
        while (name.startsWith(QLatin1Char('/')) && name.size() > 1) name.remove(0, 1);
        if (name.isEmpty() || name.contains(QLatin1Char('\0')) || (name.contains(QLatin1Char('/')) && name != QLatin1String("/")) ||
            name == QLatin1String(".") || name == QLatin1String("..") || names.contains(name)) {
            *error = QStringLiteral("Invalid or duplicate TIB legacy name"); return false;
        }
        names.insert(name);
        fs::TibLegacyItem item = {};
        if (node.fieldE == 128 || (node.fieldE >= 129 && node.fieldE <= 143))
            item.kind = fs::TibLegacyItemKind::Directory;
        else {
            if (node.fieldE == 144 || node.fieldE == 155) {
                *error = QStringLiteral("Unsupported TIB legacy item type"); return false;
            }
            if (node.recordPosition > std::uint64_t(INT64_MAX) - context->base ||
                context->base + node.recordPosition >= context->recordLimit ||
                context->recordLimit >= static_cast<std::uint64_t>(source->capacity())) {
                *error = QStringLiteral("Invalid TIB legacy item position"); return false;
            }
            fs::TibLegacyFileChunks chunks(context->source,
                static_cast<std::int64_t>(context->base + node.recordPosition),
                static_cast<std::int64_t>(context->recordLimit + 1));
            std::string message;
            if (!fs::readTibLegacyItem(&chunks, &item, &message)) {
                *error = QString::fromStdString(message); return false;
            }
        }
        ResourceEntry entry;
        entry.name = name;
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::TIB;
        if (item.kind == fs::TibLegacyItemKind::Directory) {
            entry.type = QStringLiteral("TIB_DIR");
            entry.isDirectory = true;
            entry.content = context->directorySource ? context->directorySource : source;
            entry.containerSubPath = QString::number(static_cast<qulonglong>(i));
        } else if (item.kind == fs::TibLegacyItemKind::File) {
            entry.type = QStringLiteral("TIB_FILE");
            entry.content = item.stream;
            entry.dataSize = static_cast<quint64>(item.stream->capacity());
            entry.isEmbeddedFile = true;
        } else if (item.kind == fs::TibLegacyItemKind::Symlink) {
            entry.type = QStringLiteral("TIB_SYMLINK_UTF16LE");
            for (char16_t ch : item.linkTarget) {
                entry.data.append(static_cast<char>(ch & 0xff));
                entry.data.append(static_cast<char>(ch >> 8));
            }
            entry.dataSize = static_cast<quint64>(entry.data.size());
        } else {
            switch (item.kind) {
            case fs::TibLegacyItemKind::CharacterDevice: entry.type = QStringLiteral("TIB_DEVICE_CHAR"); break;
            case fs::TibLegacyItemKind::BlockDevice: entry.type = QStringLiteral("TIB_DEVICE_BLOCK"); break;
            case fs::TibLegacyItemKind::Fifo: entry.type = QStringLiteral("TIB_FIFO"); break;
            case fs::TibLegacyItemKind::Socket: entry.type = QStringLiteral("TIB_SOCKET"); break;
            default: *error = QStringLiteral("Unsupported TIB legacy item type"); return false;
            }
            if (item.kind == fs::TibLegacyItemKind::CharacterDevice || item.kind == fs::TibLegacyItemKind::BlockDevice) {
                for (unsigned shift = 0; shift < 32; shift += 8) entry.data.append(static_cast<char>(item.deviceMajor >> shift));
                for (unsigned shift = 0; shift < 32; shift += 8) entry.data.append(static_cast<char>(item.deviceMinor >> shift));
                entry.dataSize = static_cast<quint64>(entry.data.size());
            }
        }
        resources->push_back(std::move(entry));
    }
    return true;
}

struct TibSecretBytes {
    std::vector<std::uint8_t> bytes;
    ~TibSecretBytes() { std::fill(bytes.begin(), bytes.end(), 0); }
};

class TibContextStore final : public fs::IByteStore {
public:
    TibContextStore(fs::ByteStorePtr source, std::shared_ptr<const TibOpenedContext> context)
        : source_(std::move(source)), context_(std::move(context)) {}
    std::int64_t capacity() const override { return source_->capacity(); }
    bool validate() const override { return source_->validate() && context_->data->validate(); }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return source_->read(pos, dst, count);
    }
    const std::shared_ptr<const TibOpenedContext>& context() const { return context_; }
private:
    fs::ByteStorePtr source_;
    std::shared_ptr<const TibOpenedContext> context_;
};

struct TibMacContext {
    fs::ByteStorePtr data;
    fs::ByteStorePtr companion;
    std::uint32_t blockSize;
    std::vector<fs::TibMacSlice> slices;
};

class TibMacContextStore final : public fs::IByteStore {
public:
    explicit TibMacContextStore(std::shared_ptr<const TibMacContext> context)
        : context_(std::move(context)) {}
    std::int64_t capacity() const override { return context_->data->capacity(); }
    bool validate() const override { return context_->data->validate() && context_->companion->validate(); }
    bool cheapRandomAccess() const override { return context_->data->cheapRandomAccess(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        return validate() ? context_->data->read(pos, dst, count) : -1;
    }
    const std::shared_ptr<const TibMacContext>& context() const { return context_; }
private:
    std::shared_ptr<const TibMacContext> context_;
};

bool listMac(const std::shared_ptr<const TibMacContext>& context, const fs::ByteStorePtr& source,
             const QString& path, QVector<ResourceEntry>* resources, QString* error) {
    if (!source || !source->validate()) {
        *error = QStringLiteral("Mac TIB source became invalid");
        return false;
    }
    if (path.isEmpty()) {
        for (std::size_t i = 0; i < context->slices.size(); ++i) {
            const auto& slice = context->slices[i];
            const QByteArray id(reinterpret_cast<const char*>(slice.id.data()), static_cast<int>(slice.id.size()));
            ResourceEntry entry;
            entry.name = QStringLiteral("Slice %1 (%2)").arg(static_cast<qulonglong>(i + 1)).arg(QString::fromLatin1(id.toHex()));
            entry.type = QStringLiteral("TIB_DIR");
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::TIB;
            entry.content = source;
            entry.isDirectory = true;
            entry.containerSubPath = QStringLiteral("%1:-1").arg(static_cast<qulonglong>(i));
            resources->push_back(std::move(entry));
        }
        return true;
    }
    const QStringList fields = path.split(QLatin1Char(':'));
    bool sliceOk = false, parentOk = false;
    if (fields.size() != 2) { *error = QStringLiteral("Invalid Mac TIB directory"); return false; }
    const qulonglong sliceIndex = fields[0].toULongLong(&sliceOk);
    const qlonglong parent = fields[1].toLongLong(&parentOk);
    if (!sliceOk || !parentOk || sliceIndex >= context->slices.size() || parent < -1) {
        *error = QStringLiteral("Invalid Mac TIB directory"); return false;
    }
    const auto& entries = context->slices[static_cast<std::size_t>(sliceIndex)].entries;
    if (parent >= 0 && (static_cast<std::uint64_t>(parent) >= entries.size() ||
        (entries[static_cast<std::size_t>(parent)].item.type != 0x80 &&
         entries[static_cast<std::size_t>(parent)].item.type != 0x83))) {
        *error = QStringLiteral("Invalid Mac TIB directory"); return false;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& item = entries[i];
        if (item.parent != parent) continue;
        if (item.item.name.size() > std::size_t(std::numeric_limits<int>::max())) {
            *error = QStringLiteral("Mac TIB name is too long"); return false;
        }
        ResourceEntry entry;
        entry.name = QString::fromUtf16(reinterpret_cast<const ushort*>(item.item.name.data()),
                                        static_cast<int>(item.item.name.size()));
        entry.language = QStringLiteral("neutral");
        if (item.item.type == 0x80 || item.item.type == 0x83) {
            entry.type = QStringLiteral("TIB_DIR");
            entry.format = ModuleFormat::TIB;
            entry.content = source;
            entry.isDirectory = true;
            entry.containerSubPath = QStringLiteral("%1:%2").arg(sliceIndex).arg(static_cast<qulonglong>(i));
        } else {
            std::string openError;
            entry.content = fs::openTibMacFile(source, context->blockSize, item.handles,
                                                item.item.size, &openError);
            if (!entry.content) { *error = QString::fromStdString(openError); return false; }
            entry.type = QStringLiteral("TIB_FILE");
            entry.dataSize = item.item.size;
            entry.isEmbeddedFile = true;
        }
        resources->push_back(std::move(entry));
    }
    return true;
}

struct TibFilesystemTag {
    std::uint16_t id;
    std::size_t valueOffset;
    std::size_t valueSize;
};

bool parseTibFilesystemTags(const std::uint8_t* bytes, std::size_t size,
                            std::vector<TibFilesystemTag>* tags) {
    if (!bytes || !tags) return false;
    std::vector<TibFilesystemTag> parsed;
    std::size_t pos = 0;
    while (pos < size) {
        if (size - pos < 3) return false;
        const std::uint16_t id = std::uint16_t(bytes[pos]) |
                                 (std::uint16_t(bytes[pos + 1]) << 8);
        std::size_t length = bytes[pos + 2];
        pos += 3;
        if (length & 0x80) {
            if (pos >= size) return false;
            length = (length & 0x7f) * 256U + bytes[pos++];
        }
        if (length > size - pos) return false;
        TibFilesystemTag tag = {id, pos, length};
        parsed.push_back(tag);
        pos += length;
    }
    tags->swap(parsed);
    return true;
}

bool tibFilesystemTagValue(const std::vector<std::uint8_t>& bytes,
                           const std::vector<TibFilesystemTag>& tags,
                           std::uint16_t id, std::uint64_t* value) {
    bool found = false;
    std::uint64_t parsed = 0;
    for (const TibFilesystemTag& tag : tags) {
        if (tag.id != id) continue;
        if (found || !tag.valueSize || tag.valueSize > 8) return false;
        parsed = 0;
        for (std::size_t i = 0; i < tag.valueSize; ++i)
            parsed |= std::uint64_t(bytes[tag.valueOffset + i]) << (i * 8);
        found = true;
    }
    if (found && value) *value = parsed;
    return found;
}

bool openTibFilesystemV1Catalog(const fs::ByteStorePtr& source, QVector<ResourceEntry>* resources,
                                QString* error) {
    if (!source || !source->validate() || source->capacity() < 24) {
        *error = QStringLiteral("Invalid TIB filesystem-mode archive");
        return false;
    }
    std::uint8_t header[12];
    if (!readExactStore(source, 0, header, sizeof(header)) ||
        fs::tibheader::le32(header) != 0x8f5c36c6U ||
        fs::tibheader::le32(header + 8) != 1) {
        *error = QStringLiteral("Unsupported TIB filesystem-mode stream layout");
        return false;
    }

    const std::uint64_t size = static_cast<std::uint64_t>(source->capacity());
    std::uint8_t table[12];
    if (!readExactStore(source, static_cast<std::int64_t>(size - sizeof(table)), table, sizeof(table)) ||
        fs::tibheader::le32(table + 8) != 0x179631b4U) {
        *error = QStringLiteral("Invalid TIB filesystem-mode stream footer");
        return false;
    }
    const std::uint64_t streamSize = std::uint64_t(fs::tibheader::le32(table)) |
                                     (std::uint64_t(fs::tibheader::le32(table + 4)) << 32);
    if (streamSize > size - sizeof(header) - sizeof(table)) {
        *error = QStringLiteral("Invalid TIB filesystem-mode stream size");
        return false;
    }
    const std::uint64_t streamEnd = sizeof(header) + streamSize;
    if (streamSize < 8) {
        *error = QStringLiteral("Truncated TIB filesystem-mode stream");
        return false;
    }
    std::uint8_t streamFooter[8];
    if (!readExactStore(source, static_cast<std::int64_t>(streamEnd - sizeof(streamFooter)),
                        streamFooter, sizeof(streamFooter)) ||
        fs::tibheader::le32(streamFooter + 4) != 0x94e18a2bU) {
        *error = QStringLiteral("Invalid TIB filesystem-mode catalog footer");
        return false;
    }
    const std::uint32_t footerLength = fs::tibheader::le32(streamFooter);
    if (footerLength > streamEnd - sizeof(header) - sizeof(streamFooter)) {
        *error = QStringLiteral("Invalid TIB filesystem-mode catalog footer length");
        return false;
    }
    const std::uint64_t footerStart = streamEnd - sizeof(streamFooter) - footerLength;
    if (footerLength > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        *error = QStringLiteral("TIB filesystem-mode footer is too large");
        return false;
    }
    std::vector<std::uint8_t> footer(footerLength);
    if (!readExactStore(source, static_cast<std::int64_t>(footerStart), footer.data(),
                        static_cast<int>(footer.size()))) {
        *error = QStringLiteral("Cannot read TIB filesystem-mode catalog footer");
        return false;
    }
    std::vector<TibFilesystemTag> footerTags;
    if (!parseTibFilesystemTags(footer.data(), footer.size(), &footerTags)) {
        *error = QStringLiteral("Invalid TIB filesystem-mode catalog footer fields");
        return false;
    }
    std::uint64_t catalogOffset = 0, catalogLength = 0;
    if (!tibFilesystemTagValue(footer, footerTags, 0, &catalogOffset) ||
        !tibFilesystemTagValue(footer, footerTags, 1, &catalogLength) ||
        catalogOffset > streamSize || catalogLength > streamSize - catalogOffset) {
        *error = QStringLiteral("Invalid TIB filesystem-mode catalog range");
        return false;
    }
    const std::uint64_t catalogStart = sizeof(header) + catalogOffset;
    const std::uint64_t catalogEnd = catalogStart + catalogLength;
    if (catalogEnd > footerStart || catalogLength > std::uint64_t(std::numeric_limits<int>::max())) {
        *error = QStringLiteral("TIB filesystem-mode catalog overlaps its footer");
        return false;
    }

    std::uint64_t pos = catalogStart;
    std::size_t diskNumber = 0, partitionNumber = 0;
    while (pos < catalogEnd) {
        std::uint8_t lengthBytes[2];
        if (catalogEnd - pos < sizeof(lengthBytes) ||
            !readExactStore(source, static_cast<std::int64_t>(pos), lengthBytes, sizeof(lengthBytes))) {
            *error = QStringLiteral("Truncated TIB filesystem-mode catalog record");
            return false;
        }
        const std::size_t recordLength = std::size_t(lengthBytes[0]) |
                                         (std::size_t(lengthBytes[1]) << 8);
        if (recordLength < sizeof(lengthBytes) || recordLength > catalogEnd - pos ||
            recordLength > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            *error = QStringLiteral("Invalid TIB filesystem-mode catalog record length");
            return false;
        }
        std::vector<std::uint8_t> record(recordLength);
        if (!readExactStore(source, static_cast<std::int64_t>(pos), record.data(),
                            static_cast<int>(record.size()))) {
            *error = QStringLiteral("Cannot read TIB filesystem-mode catalog record");
            return false;
        }
        std::vector<TibFilesystemTag> tags;
        if (!parseTibFilesystemTags(record.data() + sizeof(lengthBytes),
                                    record.size() - sizeof(lengthBytes), &tags)) {
            *error = QStringLiteral("Invalid TIB filesystem-mode catalog fields");
            return false;
        }
        const bool disk = std::any_of(tags.begin(), tags.end(), [](const TibFilesystemTag& tag) {
            return tag.id == 72;
        });
        ResourceEntry entry;
        entry.name = disk ? QStringLiteral("Disk record %1").arg(static_cast<qulonglong>(++diskNumber))
                          : QStringLiteral("Partition record %1").arg(static_cast<qulonglong>(++partitionNumber));
        entry.type = disk ? QStringLiteral("TIB_8F_DISK_RECORD")
                          : QStringLiteral("TIB_8F_PARTITION_RECORD");
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = pos;
        entry.dataSize = recordLength;
        entry.data = QByteArray(reinterpret_cast<const char*>(record.data()), static_cast<int>(record.size()));
        resources->push_back(std::move(entry));
        pos += recordLength;
    }
    if (resources->empty() || pos != catalogEnd) {
        *error = QStringLiteral("Empty or incomplete TIB filesystem-mode catalog");
        resources->clear();
        return false;
    }
    return true;
}

} // namespace

ModulePtr TibModule::open(const QString& path, const QString& password) {
    const fs::ByteStorePtr selected = std::make_shared<fs::QFileByteStore>(path);
    QDir vault = QFileInfo(path).absoluteDir();
    do {
        if (QFileInfo::exists(vault.filePath(QStringLiteral("unified_data.ds.0"))) ||
            QFileInfo::exists(vault.filePath(QStringLiteral("unified_data.ds.1"))) ||
            QFileInfo::exists(vault.filePath(QStringLiteral("disk_data.ds.0"))) ||
            QFileInfo::exists(vault.filePath(QStringLiteral("disk_data.ds.1"))) ||
            QFileInfo::exists(vault.filePath(QStringLiteral("file_data.ds.0"))) ||
            QFileInfo::exists(vault.filePath(QStringLiteral("file_data.ds.1")))) {
            std::vector<std::uint8_t> key;
            bool encrypted = false;
            std::string vaultError;
            if (!readVaultKey(vault, password, &key, &encrypted, &vaultError)) {
                auto module = peare::makeUnique<TibModule>();
                module->info_.filePath = path;
                module->info_.format = ModuleFormat::TIB;
                module->info_.error = QString::fromStdString(vaultError);
                if (password.isEmpty()) module->info_.openError = ModuleOpenError::PasswordRequired;
                else if (vaultError == "Invalid TIB vault password or key metadata")
                    module->info_.openError = ModuleOpenError::PasswordInvalidOrCorrupt;
                return ModulePtr(std::move(module));
            }
            if (encrypted) {
                const std::int64_t size = selected->capacity();
                std::uint8_t footerOffset[8];
                std::uint8_t trailer[36];
                if (size < 160 || !readExactStore(selected, size - 40, footerOffset, sizeof(footerOffset))) {
                    std::fill(key.begin(), key.end(), 0);
                    return ModulePtr();
                }
                const std::uint64_t offset = std::uint64_t(fs::tibheader::le32(footerOffset)) |
                    (std::uint64_t(fs::tibheader::le32(footerOffset + 4)) << 32);
                if (offset < 4 || offset > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                    std::fill(key.begin(), key.end(), 0);
                    return ModulePtr();
                }
                const std::int64_t trailerOffset = static_cast<std::int64_t>(offset - 4);
                if (trailerOffset < 48 || trailerOffset > size - 76 ||
                    !readExactStore(selected, trailerOffset, trailer, sizeof(trailer)) ||
                    fs::tibheader::le32(trailer + 4) != 16 ||
                    fs::tibheader::le32(trailer + 24) != 1 ||
                    fs::tibheader::le32(trailer + 28) != 1 ||
                    fs::tibheader::le32(trailer + 32) != 0xf742ebc6U) {
                    std::fill(key.begin(), key.end(), 0);
                    auto module = peare::makeUnique<TibModule>();
                    module->info_.filePath = path;
                    module->info_.format = ModuleFormat::TIB;
                    module->info_.error = QStringLiteral("Unsupported encrypted TIB vault stream");
                    return ModulePtr(std::move(module));
                }
                const std::uint64_t plain = std::uint64_t(fs::tibheader::le32(trailer + 8)) |
                    (std::uint64_t(fs::tibheader::le32(trailer + 12)) << 32);
                const std::int64_t cipherLength = trailerOffset - 48;
                if (cipherLength < 0 || cipherLength % 16 || plain > std::uint64_t(cipherLength) ||
                    std::uint64_t(cipherLength) - plain != fs::tibheader::le32(trailer) ||
                    fs::tibheader::le32(trailer) > 15) {
                    std::fill(key.begin(), key.end(), 0);
                    auto module = peare::makeUnique<TibModule>();
                    module->info_.filePath = path;
                    module->info_.format = ModuleFormat::TIB;
                    module->info_.error = QStringLiteral("Invalid encrypted TIB vault stream");
                    return ModulePtr(std::move(module));
                }
                fs::ByteStorePtr encryptedData = std::make_shared<fs::SubStore>(selected, 32, 16 + cipherLength);
                fs::ByteStorePtr clear = fs::openTibCbcStore(encryptedData, key, &vaultError);
                std::fill(key.begin(), key.end(), 0);
                if (!clear) return ModulePtr();
                fs::ByteStorePtr prefix = std::make_shared<fs::SubStore>(selected, 0, 32);
                fs::ByteStorePtr content = std::make_shared<fs::SubStore>(clear, 16, static_cast<std::int64_t>(plain));
                const std::int64_t tailOffset = trailerOffset + sizeof(trailer);
                fs::ByteStorePtr tail = std::make_shared<fs::SubStore>(selected, tailOffset, size - tailOffset);
                fs::ByteStorePtr rest = std::make_shared<TibPrefixedStore>(content, tail, selected);
                return open(std::make_shared<TibPrefixedStore>(prefix, rest, selected), path, QString(), password);
            }
            break;
        }
    } while (vault.cdUp());
    std::uint8_t signature[8];
    if (readExactStore(selected, 0, signature, sizeof(signature)) &&
        fs::tibheader::le32(signature) == 0xa2b924ceU && fs::tibheader::le16(signature + 6) == 1) {
        const QString companionPath = path + QStringLiteral(".metadata");
        return openMac(selected, std::make_shared<fs::QFileByteStore>(companionPath), path, companionPath);
    }
    const QFileInfo info(path);
    VolumeHeader selectedHeader;
    if (!volumeHeader(selected, &selectedHeader) ||
        info.suffix().compare(QStringLiteral("tib"), Qt::CaseInsensitive) != 0)
        return open(selected, path, QString(), password);
    const QString stem = info.completeBaseName();
    const QString suffix = QString::number(selectedHeader.number);
    const QFileInfoList files = info.dir().entryInfoList(QDir::Files | QDir::Hidden | QDir::System);
    QString base = stem;
    if (selectedHeader.number == 1) {
        bool implicitSecond = false;
        bool numberedSecond = false;
        const QString numberedBase = stem.endsWith(suffix) ? stem.left(stem.size() - suffix.size()) : QString();
        for (const QFileInfo& file : files) {
            const bool implicitName = file.fileName().compare(stem + QStringLiteral("2.tib"), Qt::CaseInsensitive) == 0;
            const bool numberedName = !numberedBase.isEmpty() &&
                file.fileName().compare(numberedBase + QStringLiteral("2.tib"), Qt::CaseInsensitive) == 0;
            if (!implicitName && !numberedName) continue;
            VolumeHeader header;
            const fs::ByteStorePtr source = std::make_shared<fs::QFileByteStore>(file.absoluteFilePath());
            if (volumeHeader(source, &header) && header.archiveId == selectedHeader.archiveId &&
                header.number == 2) {
                implicitSecond = implicitSecond || implicitName;
                numberedSecond = numberedSecond || numberedName;
            }
        }
        if (implicitSecond && numberedSecond) {
            auto module = peare::makeUnique<TibModule>();
            module->info_.filePath = path;
            module->info_.format = ModuleFormat::TIB;
            module->info_.description = QStringLiteral("Acronis TIB backup");
            module->info_.error = QStringLiteral("Ambiguous TIB volumes");
            return ModulePtr(std::move(module));
        }
        if (!implicitSecond && stem.endsWith(suffix)) base.chop(suffix.size());
    } else if (stem.endsWith(suffix)) {
        base.chop(suffix.size());
    } else {
        return open(selected, path, QString(), password);
    }

    QMap<std::uint32_t, fs::ByteStorePtr> candidates;
    bool ambiguous = false;
    for (const QFileInfo& file : files) {
        if (file.suffix().compare(QStringLiteral("tib"), Qt::CaseInsensitive) != 0) continue;
        const fs::ByteStorePtr source = std::make_shared<fs::QFileByteStore>(file.absoluteFilePath());
        VolumeHeader header;
        if (!volumeHeader(source, &header) || header.archiveId != selectedHeader.archiveId) continue;
        if (candidates.contains(header.number)) { ambiguous = true; break; }
        candidates.insert(header.number, source);
    }
    const bool standaloneFallback = !ambiguous && selectedHeader.number == 1 && candidates.size() <= 1;

    std::string error;
    std::vector<fs::ByteStorePtr> parts;
    std::uint64_t expected = 1;
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it, ++expected) {
        if (it.key() != expected) { error = "Missing TIB volume"; break; }
        parts.push_back(it.value());
    }
    if (ambiguous) error = "Ambiguous TIB volumes";
    if (error.empty() && (parts.empty() || expected <= selectedHeader.number))
        error = "Missing TIB volume";
    if (error.empty() && selectedHeader.number <= parts.size()) {
        std::vector<fs::ByteStorePtr> oldParts(parts.begin(), parts.begin() + selectedHeader.number);
        std::shared_ptr<fs::TibOldFileDirectory> directory;
        bool oldOpened = openOldDirectory(oldParts, &directory);
        std::uint8_t selectedFooter[4];
        const fs::ByteStorePtr& lastSelected = oldParts.back();
        const bool completeSection = lastSelected->capacity() >= 4 &&
            readExactStore(lastSelected, lastSelected->capacity() - 4, selectedFooter, 4) &&
            fs::tibheader::le32(selectedFooter) == 0x179631b4U;
        if (!oldOpened && !completeSection && oldParts.size() < parts.size()) {
            oldParts = parts;
            oldOpened = openOldDirectory(oldParts, &directory);
        }
        if (oldOpened) {
            auto module = peare::makeUnique<TibModule>();
            module->info_.filePath = path;
            module->info_.format = ModuleFormat::TIB;
            module->info_.description = QStringLiteral("Acronis TIB backup");
            fs::ByteStorePtr contextual = std::make_shared<TibOldContextStore>(oldParts.front(), directory);
            if (!listOld(directory, contextual, QString(), &module->resources_, &module->info_.error))
                module->resources_.clear();
            return ModulePtr(std::move(module));
        }
        std::uint8_t legacyPrefix[4];
        if (parts.size() > 1 &&
            readExactStore(oldParts.front(), 0, legacyPrefix, sizeof(legacyPrefix)) &&
            fs::tibheader::le32(legacyPrefix) == 0x44686eb4U) {
            std::string legacyError;
            fs::ByteStorePtr legacyLogical;
            if (oldParts.size() > 1)
                legacyLogical = fs::openTibMultiVolume(oldParts, &legacyError);
            if (!legacyLogical && oldParts.size() < parts.size())
                legacyLogical = fs::openTibMultiVolume(parts, &legacyError);
            if (legacyLogical) return open(legacyLogical, path, QString(), password);
        }
    }
    fs::ByteStorePtr logical;
    if (error.empty() && selectedHeader.number < parts.size()) {
        std::vector<fs::ByteStorePtr> selectedParts(parts.begin(), parts.begin() + selectedHeader.number);
        std::string selectedError;
        logical = fs::openTibMultiVolume(selectedParts, &selectedError);
        if (logical) parts.swap(selectedParts);
    }
    if (error.empty() && !logical) logical = fs::openTibMultiVolume(parts, &error);
    VolumeHeader firstHeader;
    if (logical && (!volumeHeader(parts.front(), &firstHeader) || firstHeader.number != 1 ||
                    logical->capacity() < 0 ||
                    logical->capacity() > std::numeric_limits<std::int64_t>::max() - firstHeader.dataOffset)) {
        logical.reset();
        error = "Invalid TIB assembled size or first volume";
    }
    if (!logical && standaloneFallback) return open(selected, path, QString(), password);
    if (!logical) {
        auto module = peare::makeUnique<TibModule>();
        module->info_.filePath = path;
        module->info_.format = ModuleFormat::TIB;
        module->info_.description = QStringLiteral("Acronis TIB backup");
        module->info_.error = QString::fromStdString(error.empty() ? "Cannot assemble TIB volumes" : error);
        return ModulePtr(std::move(module));
    }
    fs::ByteStorePtr prefix = std::make_shared<fs::SubStore>(parts.front(), 0, firstHeader.dataOffset);
    return open(std::make_shared<TibPrefixedStore>(prefix, logical, parts.front()), path, QString(), password);
}

ModulePtr TibModule::openParts(const std::vector<std::pair<fs::ByteStorePtr, QString> >& parts,
                               const QString& password) {
    if (parts.empty()) return ModulePtr();
    std::vector<fs::ByteStorePtr> sources;
    sources.reserve(parts.size());
    for (const auto& part : parts) sources.push_back(part.first);
    std::shared_ptr<fs::TibOldFileDirectory> oldDirectory;
    if (openOldDirectory(sources, &oldDirectory)) {
        auto module = peare::makeUnique<TibModule>();
        module->info_.filePath = parts.front().second;
        module->info_.format = ModuleFormat::TIB;
        module->info_.description = QStringLiteral("Acronis TIB backup");
        fs::ByteStorePtr contextual = std::make_shared<TibOldContextStore>(sources.front(), oldDirectory);
        if (!listOld(oldDirectory, contextual, QString(), &module->resources_, &module->info_.error))
            module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    std::uint8_t legacyPrefix[4];
    if (sources.size() > 1 &&
        readExactStore(sources.front(), 0, legacyPrefix, sizeof(legacyPrefix)) &&
        fs::tibheader::le32(legacyPrefix) == 0x44686eb4U) {
        std::string legacyError;
        fs::ByteStorePtr legacyLogical = fs::openTibMultiVolume(sources, &legacyError);
        if (legacyLogical) return open(legacyLogical, parts.front().second, QString(), password);
    }
    std::string error;
    fs::ByteStorePtr logical = fs::openTibMultiVolume(sources, &error);
    VolumeHeader first;
    if (logical && (!volumeHeader(sources.front(), &first) || first.number != 1 ||
                    logical->capacity() < 0 ||
                    logical->capacity() > std::numeric_limits<std::int64_t>::max() - first.dataOffset)) {
        logical.reset();
        error = "Invalid TIB assembled size or first volume";
    }
    if (!logical) {
        auto module = peare::makeUnique<TibModule>();
        module->info_.filePath = parts.front().second;
        module->info_.format = ModuleFormat::TIB;
        module->info_.description = QStringLiteral("Acronis TIB backup");
        module->info_.error = QString::fromStdString(error.empty() ? "Cannot assemble TIB volumes" : error);
        return ModulePtr(std::move(module));
    }
    fs::ByteStorePtr prefix = std::make_shared<fs::SubStore>(sources.front(), 0, first.dataOffset);
    return open(std::make_shared<TibPrefixedStore>(prefix, logical, sources.front()),
                parts.front().second, QString(), password);
}

ModulePtr TibModule::openMac(const fs::ByteStorePtr& source, const fs::ByteStorePtr& companion,
                              const QString& name, const QString& companionName) {
    auto module = peare::makeUnique<TibModule>();
    module->info_.filePath = name;
    module->info_.format = ModuleFormat::TIB;
    module->info_.description = QStringLiteral("Acronis TIB Mac backup");
    if (QFileInfo(companionName).fileName() != QFileInfo(name).fileName() + QStringLiteral(".metadata")) {
        module->info_.error = QStringLiteral("Mac TIB companion name does not match archive");
        return ModulePtr(std::move(module));
    }
    if (!companion || !companion->validate()) {
        module->info_.error = QStringLiteral("Mac TIB companion is required");
        return ModulePtr(std::move(module));
    }
    auto context = std::make_shared<TibMacContext>();
    context->data = source;
    context->companion = companion;
    std::string error;
    if (!fs::readTibMacIndex(source, companion, &context->blockSize, &context->slices, &error)) {
        module->info_.error = QString::fromStdString(error.empty() ? "Cannot read Mac TIB index" : error);
        return ModulePtr(std::move(module));
    }
    fs::ByteStorePtr contextual = std::make_shared<TibMacContextStore>(context);
    if (!listMac(context, contextual, QString(), &module->resources_, &module->info_.error))
        module->resources_.clear();
    return ModulePtr(std::move(module));
}

ModulePtr TibModule::open(const fs::ByteStorePtr& source, const QString& name,
                          const QString& subPath, const QString& password) {
    auto module = peare::makeUnique<TibModule>();
    module->info_.filePath = name;
    module->info_.format = ModuleFormat::TIB;
    module->info_.description = QStringLiteral("Acronis TIB backup");
    const auto old = std::dynamic_pointer_cast<TibOldContextStore>(source);
    if (old) {
        if (!listOld(old->directory(), source, subPath, &module->resources_, &module->info_.error))
            module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    const auto legacy = std::dynamic_pointer_cast<TibLegacyContextStore>(source);
    if (legacy) {
        if (!listLegacy(legacy->context(), source, subPath, &module->resources_, &module->info_.error))
            module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    const auto mac = std::dynamic_pointer_cast<TibMacContextStore>(source);
    if (mac) {
        module->info_.description = QStringLiteral("Acronis TIB Mac backup");
        if (!listMac(mac->context(), source, subPath, &module->resources_, &module->info_.error))
            module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    std::uint8_t prefix[8];
    if (readExactStore(source, 0, prefix, sizeof(prefix)) &&
        fs::tibheader::le32(prefix) == 0xa2b924ceU && fs::tibheader::le16(prefix + 6) == 1) {
        module->info_.description = QStringLiteral("Acronis TIB Mac backup");
        module->info_.error = QStringLiteral("Mac TIB companion must be supplied as a separate source");
        return ModulePtr(std::move(module));
    }
    if (readExactStore(source, 0, prefix, sizeof(prefix)) &&
        fs::tibheader::le32(prefix) == 0x8f5c36c6U) {
        module->info_.description = QStringLiteral("Acronis TIB filesystem-mode v1 catalog");
        if (!openTibFilesystemV1Catalog(source, &module->resources_, &module->info_.error))
            module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    std::vector<fs::TibLegacyPartition> partitions;
    std::string error;
    const auto existing = std::dynamic_pointer_cast<TibContextStore>(source);
    const auto diskPrefixed = std::dynamic_pointer_cast<TibPrefixedStore>(source);
    const bool fileBacked = std::dynamic_pointer_cast<fs::QFileByteStore>(source) ||
                            (diskPrefixed && diskPrefixed->isFileBacked());
    std::shared_ptr<fs::TibDedupProvider> diskProvider;
    bool needsProviderDuringProbe = false;
    bool openedDisk = !existing && fs::openTibLegacyDisk(source, &partitions, &error,
        std::shared_ptr<fs::TibDedupProvider>(), &needsProviderDuringProbe);
    if (!existing && !openedDisk && fileBacked && needsProviderDuringProbe) {
        std::string providerError;
        diskProvider = findDedupProvider(name, password, &providerError, true);
        if (diskProvider) {
            partitions.clear();
            openedDisk = fs::openTibLegacyDisk(source, &partitions, &error, diskProvider);
        }
    }
    std::uint8_t oldPrefix[13];
    if (!existing && !openedDisk && readExactStore(source, 0, oldPrefix, sizeof(oldPrefix)) &&
        fs::tibheader::le32(oldPrefix) == 0x44686eb4U && oldPrefix[12] == 0x66) {
        fs::TibOldFileIndex oldIndex;
        fs::TibOldIndexLimits limits;
        const std::vector<fs::ByteStorePtr> volumes(1, source);
        const fs::TibOldIndexStatus status = fs::buildTibOldFileIndex(volumes, limits, &oldIndex);
        bool footerBoundary = false;
        if (status == fs::TibOldIndexStatus::UnverifiedData && oldIndex.skippedRanges.size() == 1 &&
            source->capacity() >= 12 && oldIndex.skippedRanges[0].volumeIndex == 0 &&
            oldIndex.skippedRanges[0].length == 12) {
            std::uint8_t footer[12];
            if (readExactStore(source, source->capacity() - 12, footer, 12) &&
                fs::tibheader::le32(footer + 8) == 0x179631b4U) {
                const std::uint64_t end = std::uint64_t(fs::tibheader::le32(footer)) |
                    (std::uint64_t(fs::tibheader::le32(footer + 4)) << 32);
                footerBoundary = end == oldIndex.skippedRanges[0].volumeOffset;
            }
        }
        if ((status == fs::TibOldIndexStatus::Ok || footerBoundary) && !oldIndex.snapshots.empty()) {
            std::shared_ptr<fs::TibOldFileDirectory> directory;
            if (fs::TibOldFileDirectory::create(oldIndex, oldIndex.snapshots.size() - 1,
                                                volumes, limits.fields, &directory) == fs::TibOldDirectoryStatus::Ok) {
                fs::ByteStorePtr contextual = std::make_shared<TibOldContextStore>(source, directory);
                if (!listOld(directory, contextual, subPath, &module->resources_, &module->info_.error))
                    module->resources_.clear();
                return ModulePtr(std::move(module));
            }
        }
    }
    if (existing || !openedDisk) {
        std::vector<fs::TibFileIndexEntry> indexed;
        const std::vector<fs::TibFileIndexEntry>* activeIndex = &indexed;
        fs::ByteStorePtr directorySource = source;
        fs::ByteStorePtr contentSource = source;
        std::uint64_t dataOffset = 0;
        std::shared_ptr<fs::TibDedupProvider> provider;
        std::string providerError;
        fs::TibFileMetadataVersion metadataVersion = {0, 0};
        fs::TibFileIndexFallback fallback;
        if (existing) {
            activeIndex = &existing->context()->entries;
            metadataVersion = existing->context()->version;
            contentSource = existing->context()->data;
            dataOffset = existing->context()->dataOffset;
            provider = existing->context()->provider;
        } else if (!fs::readTibFileIndex(source, &indexed, &dataOffset, &metadataVersion, &error, &fallback)) {
            if (fallback.legacyTreeAvailable) {
                auto context = std::make_shared<TibLegacyContext>();
                std::size_t consumed = 0;
                if (!fs::parseTibLegacyFileTree(fallback.legacyTree, fallback.legacyVersion,
                                                &context->nodes, &consumed, &error)) {
                    module->info_.error = QString::fromStdString(error);
                    return ModulePtr(std::move(module));
                }
                context->source = source;
                context->directorySource = source;
                context->base = fallback.dataOffset;
                context->recordLimit = fallback.metadataStart;
                fs::ByteStorePtr contextual = std::make_shared<TibLegacyContextStore>(context);
                if (!listLegacy(context, contextual, subPath, &module->resources_, &module->info_.error))
                    module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            if (!fallback.metadataAvailable) {
                module->info_.error = QString::fromStdString(
                    fallback.metadataError.empty() ? error : fallback.metadataError);
                return ModulePtr(std::move(module));
            }
            const fs::TibFileMetadata& metadata = fallback.metadata;
            if (!metadata.hasEncryptionVector) {
                module->info_.error = QString::fromStdString(error);
                return ModulePtr(std::move(module));
            }
            if (password.isEmpty()) {
                module->info_.error = QStringLiteral("TIB password required");
                module->info_.openError = ModuleOpenError::PasswordRequired;
                return ModulePtr(std::move(module));
            }
            if (metadata.encryptionVector.size() < 8) {
                module->info_.error = QStringLiteral("Invalid TIB encryption vector");
                return ModulePtr(std::move(module));
            }
            const std::vector<std::uint8_t> records(metadata.encryptionVector.begin() + 1,
                                                    metadata.encryptionVector.end() - 4);
            QByteArray utf8 = password.toUtf8();
            const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(utf8.constData());
            std::vector<std::uint8_t> passwordBytes(bytes, bytes + utf8.size());
            TibSecretBytes key;
            std::size_t consumed = 0;
            const fs::TibKeyResult result = fs::tibRecoverKey(
                passwordBytes, records, std::numeric_limits<std::uint64_t>::max(), &key.bytes, &consumed);
            utf8.fill(0);
            std::fill(passwordBytes.begin(), passwordBytes.end(), 0);
            if (result != fs::TibKeyResult::Success) {
                switch (result) {
                case fs::TibKeyResult::Success:
                    break;
                case fs::TibKeyResult::PasswordMismatch:
                    module->info_.error = QStringLiteral("TIB password is incorrect or the archive is damaged");
                    module->info_.openError = ModuleOpenError::PasswordInvalidOrCorrupt;
                    break;
                case fs::TibKeyResult::Unsupported:
                    module->info_.error = QStringLiteral("Unsupported TIB encryption key derivation or candidate");
                    break;
                case fs::TibKeyResult::IterationLimit:
                    module->info_.error = QStringLiteral("TIB key derivation exceeds the iteration budget");
                    break;
                case fs::TibKeyResult::Truncated:
                    module->info_.error = QStringLiteral("Truncated TIB encryption records");
                    break;
                case fs::TibKeyResult::InvalidArgument:
                case fs::TibKeyResult::InvalidRecord:
                    module->info_.error = QStringLiteral("Invalid TIB encryption records");
                    break;
                }
                return ModulePtr(std::move(module));
            }
            const std::uint64_t base = fallback.dataOffset;
            if (!source || !source->validate() || source->capacity() < 0 ||
                base >= std::uint64_t(source->capacity()) ||
                fallback.metadataStart < base || fallback.metadataEnd < fallback.metadataStart ||
                fallback.trailerStart < fallback.metadataEnd ||
                fallback.trailerStart > std::uint64_t(source->capacity()) ||
                !metadata.hasPositionB ||
                metadata.positionB > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                (metadata.version.major >= 4 && !fs::supportsTibFileMetadataVersion(metadata.version))) {
                module->info_.error = QStringLiteral("Unsupported TIB encrypted index coordinates or version");
                return ModulePtr(std::move(module));
            }
            const std::int64_t logicalLength = source->capacity() - static_cast<std::int64_t>(base);
            fs::ByteStorePtr logical = std::make_shared<TibLogicalStore>(
                source, static_cast<std::int64_t>(base), logicalLength);
            fs::ByteStorePtr cbc = fs::openTibCbcStore(logical, key.bytes, &error);
            std::fill(key.bytes.begin(), key.bytes.end(), 0);
            key.bytes.clear();
            if (!cbc) {
                module->info_.error = QString::fromStdString(error);
                return ModulePtr(std::move(module));
            }
            auto readable = std::make_shared<fs::TibCbcBounds>(cbc);
            const std::int64_t gPosition = static_cast<std::int64_t>(metadata.positionB);
            const std::int64_t gLimit = readable->capacity();
            if (!fs::tibCbcReadableOffset(gPosition, gLimit)) {
                module->info_.error = QStringLiteral("TIB encrypted index lies outside a readable region");
                return ModulePtr(std::move(module));
            }
            std::vector<std::uint8_t> decoded;
            std::int64_t recordEnd = 0;
            bool fatal = false;
            if (!fs::readTibFileGRecord(readable, gPosition, gLimit, &decoded, &recordEnd, &fatal, &error)) {
                module->info_.error = QString::fromStdString(error.empty() ? "Invalid TIB encrypted index" : error);
                return ModulePtr(std::move(module));
            }
            if (metadata.version.major < 4) {
                auto context = std::make_shared<TibLegacyContext>();
                std::size_t consumed = 0;
                if (!fs::parseTibLegacyFileTree(decoded, metadata.version, &context->nodes,
                                                &consumed, &error)) {
                    module->info_.error = QString::fromStdString(error);
                    return ModulePtr(std::move(module));
                }
                context->source = readable;
                context->directorySource = source;
                context->base = 0;
                context->recordLimit = fallback.metadataStart - base;
                fs::ByteStorePtr contextual = std::make_shared<TibLegacyContextStore>(context);
                if (!listLegacy(context, contextual, subPath, &module->resources_, &module->info_.error))
                    module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            if (!fs::parseTibFileGIndex(decoded, &indexed, &error)) {
                module->info_.error = QString::fromStdString(error.empty() ? "Invalid TIB encrypted index" : error);
                return ModulePtr(std::move(module));
            }
            auto context = std::make_shared<TibOpenedContext>();
            context->entries = std::move(indexed);
            context->version = metadata.version;
            context->data = readable;
            directorySource = std::make_shared<TibContextStore>(source, context);
            contentSource = readable;
            activeIndex = &context->entries;
            metadataVersion = context->version;
            dataOffset = 0;
        }
        QSet<QString> paths;
        QSet<QString> streamPaths;
        QSet<QString> directories;
        QSet<QString> implicitDirectories;
        QSet<QString> uncServers;
        QSet<QString> parents;
        bool directoryExists = subPath.isEmpty();
        std::vector<fs::TibFileItem> items;
        std::vector<std::uint64_t> itemOffsets;
        items.reserve(activeIndex->size());
        itemOffsets.reserve(activeIndex->size());
        for (const fs::TibFileIndexEntry& indexedItem : *activeIndex) {
            fs::TibFileItem item;
            if (!fs::parseTibFileItem(indexedItem, metadataVersion, &item, &error) ||
                item.path.size() > std::size_t(std::numeric_limits<int>::max())) {
                module->info_.error = error.empty() ? QStringLiteral("TIB path is too long") : QString::fromStdString(error);
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            const bool uncPath = item.path.size() >= 2 && item.path[0] == u'/' && item.path[1] == u'/';
            if (uncPath) {
                item.path.erase(0, 2);
                const std::size_t separator = item.path.find(u'/');
                const std::size_t serverLength = separator == std::u16string::npos ? item.path.size() : separator;
                if (serverLength > 0) {
                    uncServers.insert(QString::fromUtf16(reinterpret_cast<const ushort*>(item.path.data()),
                                                        static_cast<int>(serverLength)));
                }
            }
            if (item.deleted) continue;
            itemOffsets.push_back(item.itemOffset);
            items.push_back(std::move(item));
        }
        std::sort(itemOffsets.begin(), itemOffsets.end());
        itemOffsets.erase(std::unique(itemOffsets.begin(), itemOffsets.end()), itemOffsets.end());
        const bool needsProvider = std::any_of(items.begin(), items.end(),
            [](const fs::TibFileItem& item) { return item.providerRequired && !item.directory; });
        if (needsProvider && !existing) {
            const auto prefixed = std::dynamic_pointer_cast<TibPrefixedStore>(source);
            if (std::dynamic_pointer_cast<fs::QFileByteStore>(source) || (prefixed && prefixed->isFileBacked()))
                provider = findDedupProvider(name, password, &providerError, false);
            if (provider) {
                const auto contextual = std::dynamic_pointer_cast<TibContextStore>(directorySource);
                if (contextual) {
                    auto context = std::make_shared<TibOpenedContext>(*contextual->context());
                    context->provider = provider;
                    directorySource = std::make_shared<TibContextStore>(source, context);
                } else {
                    auto context = std::make_shared<TibOpenedContext>();
                    context->entries = std::move(indexed);
                    context->version = metadataVersion;
                    context->data = contentSource;
                    context->dataOffset = dataOffset;
                    context->provider = provider;
                    directorySource = std::make_shared<TibContextStore>(source, context);
                }
            }
        }
        for (const fs::TibFileItem& item : items) {
            const QString path = QString::fromUtf16(reinterpret_cast<const ushort*>(item.path.data()),
                                                     static_cast<int>(item.path.size()));
            if (path.isEmpty() || path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/')) ||
                path.contains(QStringLiteral("//")) || path.contains(QLatin1Char('\\')) ||
                path.split(QLatin1Char('/')).contains(QStringLiteral(".")) ||
                path.split(QLatin1Char('/')).contains(QStringLiteral("..")) || paths.contains(path)) {
                module->info_.error = QStringLiteral("Invalid or duplicate TIB path");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            paths.insert(path);
            if (item.directory) directories.insert(path);
            if (item.directory && path == subPath) directoryExists = true;
            const int separator = path.lastIndexOf(QLatin1Char('/'));
            const QString parent = separator < 0 ? QString() : path.left(separator);
            if (!parent.isEmpty()) parents.insert(parent);
            if (parent != subPath) continue;
            if (item.providerRequired && !item.directory && !provider) {
                module->info_.error = providerError.empty()
                    ? QStringLiteral("TIB deduplication store is required") : QString::fromStdString(providerError);
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            ResourceEntry entry;
            entry.name = separator < 0 ? path : path.mid(separator + 1);
            entry.type = item.directory ? QStringLiteral("TIB_DIR") : QStringLiteral("TIB_FILE");
            entry.language = QStringLiteral("neutral");
            if (item.directory) {
                entry.format = ModuleFormat::TIB;
                entry.content = directorySource;
                entry.isDirectory = true;
                entry.containerSubPath = path;
                module->resources_.push_back(std::move(entry));
            } else {
                std::vector<fs::TibFileStream> streams;
                if (!fs::openTibFileStreams(contentSource, dataOffset, item.itemOffset,
                                            item.logicalSize, itemOffsets, &streams, &error,
                                            item.providerRequired ? provider : std::shared_ptr<fs::TibDedupProvider>())) {
                    module->info_.error = QString::fromStdString(error);
                    module->resources_.clear();
                    return ModulePtr(std::move(module));
                }
                for (const fs::TibFileStream& stream : streams) {
                    ResourceEntry resource = entry;
                    if (!stream.name.empty()) {
                        if (stream.name.size() > std::size_t(std::numeric_limits<int>::max())) {
                            module->info_.error = QStringLiteral("TIB stream name is too long");
                            module->resources_.clear();
                            return ModulePtr(std::move(module));
                        }
                        QString suffix = QString::fromUtf16(reinterpret_cast<const ushort*>(stream.name.data()),
                                                            static_cast<int>(stream.name.size()));
                        if (!suffix.startsWith(QLatin1Char(':'))) suffix.prepend(QLatin1Char(':'));
                        resource.name += suffix;
                        const QString streamPath = path + suffix;
                        if (streamPaths.contains(streamPath)) {
                            module->info_.error = QStringLiteral("Duplicate TIB stream path");
                            module->resources_.clear();
                            return ModulePtr(std::move(module));
                        }
                        streamPaths.insert(streamPath);
                    }
                    resource.content = stream.content;
                    resource.dataSize = static_cast<quint64>(resource.content->capacity());
                    resource.isEmbeddedFile = true;
                    module->resources_.push_back(std::move(resource));
                }
            }
        }
        for (const QString& path : paths) {
            int separator = path.indexOf(QLatin1Char('/'));
            while (separator >= 0) {
                const QString ancestor = path.left(separator);
                if (!ancestor.isEmpty() && uncServers.contains(ancestor) && !directories.contains(ancestor))
                    implicitDirectories.insert(ancestor);
                separator = path.indexOf(QLatin1Char('/'), separator + 1);
            }
        }
        for (const QString& path : implicitDirectories) directories.insert(path);
        if (directories.contains(subPath)) directoryExists = true;
        std::vector<QString> orderedImplicitDirectories;
        orderedImplicitDirectories.reserve(static_cast<std::size_t>(implicitDirectories.size()));
        for (const QString& path : implicitDirectories) orderedImplicitDirectories.push_back(path);
        std::sort(orderedImplicitDirectories.begin(), orderedImplicitDirectories.end());
        for (const QString& path : orderedImplicitDirectories) {
            const int separator = path.lastIndexOf(QLatin1Char('/'));
            const QString parent = separator < 0 ? QString() : path.left(separator);
            if (parent != subPath) continue;
            ResourceEntry entry;
            entry.name = separator < 0 ? path : path.mid(separator + 1);
            entry.type = QStringLiteral("TIB_DIR");
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::TIB;
            entry.content = directorySource;
            entry.isDirectory = true;
            entry.containerSubPath = path;
            module->resources_.push_back(std::move(entry));
        }
        for (const QString& streamPath : streamPaths) {
            if (paths.contains(streamPath)) {
                module->info_.error = QStringLiteral("TIB stream path conflicts with an item");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
        }
        for (const QString& parent : parents) {
            if (!directories.contains(parent)) {
                module->info_.error = QStringLiteral("TIB parent directory not found");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
        }
        if (!directoryExists) {
            module->info_.error = QStringLiteral("TIB directory not found");
            module->resources_.clear();
        }
        return ModulePtr(std::move(module));
    }
    if (!subPath.isEmpty()) {
        module->info_.error = QStringLiteral("TIB directory not found");
        return ModulePtr(std::move(module));
    }
    if (fs::tibLegacyNeedsDedupProvider(partitions) && fileBacked) {
        std::string providerError;
        if (!diskProvider) diskProvider = findDedupProvider(name, password, &providerError, true);
        fs::attachTibLegacyDedupProvider(&partitions, diskProvider);
    }
    const std::vector<fs::ByteStorePtr> partitionStores = fs::makeTibLegacyPartitionStores(partitions);
    for (std::size_t i = 0; i < partitions.size(); ++i) {
        const fs::TibLegacyPartition& partition = partitions[i];
        ResourceEntry entry;
        entry.name = QStringLiteral("Partition %1").arg(partition.index);
        entry.type = QStringLiteral("DISK_PARTITION");
        entry.language = QStringLiteral("neutral");
        entry.dataSize = static_cast<quint64>(partition.content->capacity());
        entry.content = partition.fatClusterFilter
            ? fs::ByteStorePtr(std::make_shared<fs::TibLegacyFilteredVolume>(
                  partitionStores[i], partition.fatClusterFilter))
            : partitionStores[i];
        entry.isEmbeddedFile = true;
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
