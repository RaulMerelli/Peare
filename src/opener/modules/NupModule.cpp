#include "NupModule.h"

#include "Compat.h"
#include "ZstdModule.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::uint64_t kSuperblockSize = 0x80;
const std::uint64_t kTocHeaderSize = 0x30;
const std::uint64_t kPackageSize = 0x40;
const std::uint64_t kDirectorySize = 0x18;
const std::uint64_t kFileSize = 0x50;
const std::uint32_t kNoDirectory = 0xffffffffu;

bool rangeFits(std::uint64_t offset, std::uint64_t length, std::uint64_t limit)
{
    return offset <= limit && length <= limit - offset;
}

std::uint16_t le16(const std::uint8_t* p)
{
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p)
{
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool readExact(const fs::ByteStorePtr& source, std::uint64_t offset,
               std::uint8_t* output, std::size_t length)
{
    if (!source || source->capacity() < 0 ||
        offset > std::uint64_t(source->capacity()) ||
        std::uint64_t(length) > std::uint64_t(source->capacity()) - offset)
        return false;
    std::size_t done = 0;
    while (done < length) {
        const int request = static_cast<int>(std::min<std::size_t>(
            length - done, 1024 * 1024));
        const int count = source->read(static_cast<std::int64_t>(offset + done),
                                       output + done, request);
        if (count <= 0 || count > request)
            return false;
        done += static_cast<std::size_t>(count);
    }
    return true;
}

bool hashRange(const fs::ByteStorePtr& source, std::uint64_t offset,
               std::uint64_t length, QByteArray* digest)
{
    if (!digest || !source || !rangeFits(offset, length,
            static_cast<std::uint64_t>(source->capacity())))
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    std::vector<std::uint8_t> buffer(1024 * 1024);
    std::uint64_t done = 0;
    while (done < length) {
        const int request = static_cast<int>(std::min<std::uint64_t>(
            buffer.size(), length - done));
        if (!readExact(source, offset + done, buffer.data(), request))
            return false;
        hash.addData(reinterpret_cast<const char*>(buffer.data()), request);
        done += static_cast<std::uint64_t>(request);
    }
    *digest = hash.result();
    return true;
}

QString utf8String(const std::vector<std::uint8_t>& pool, std::uint32_t offset,
                   std::uint32_t length, bool allowEmpty)
{
    if ((!allowEmpty && length == 0) ||
        offset > pool.size() || length > pool.size() - offset)
        return QString();
    const char* bytes = reinterpret_cast<const char*>(pool.data() + offset);
    const QString value = QString::fromUtf8(bytes, static_cast<int>(length));
    if (value.toUtf8() != QByteArray(bytes, static_cast<int>(length)) ||
        value.contains(QChar::Null) || value == QStringLiteral(".") ||
        value == QStringLiteral("..") || value.contains(QLatin1Char('/')) ||
        value.contains(QLatin1Char('\\')))
        return QString();
    return value;
}

struct NupDirectory {
    QString name;
    std::uint32_t firstChild = 0;
    std::uint32_t childCount = 0;
};

struct NupFile {
    QString name;
    std::uint64_t size = 0;
    std::uint64_t extentOffset = 0;
    std::uint64_t extentSize = 0;
    std::array<std::uint8_t, 32> digest = {};
    std::uint8_t codec = 0;
};

struct NupExtent {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    std::uint64_t size = 0;
    std::array<std::uint8_t, 32> digest = {};
    std::uint8_t codec = 0;
};

struct NupPackage {
    std::uint64_t titleId = 0;
    std::uint32_t rootDirectory = 0;
    std::uint32_t exefsDirectory = kNoDirectory;
    std::uint32_t romfsDirectory = kNoDirectory;
    std::uint32_t version = 0;
    std::uint8_t kind = 0;
    std::uint8_t flags = 0;
    std::uint64_t metaOffset = 0;
    std::uint32_t metaSize = 0;
    QString title;
    QString publisher;
    QString displayVersion;
};

class NupFileStore final : public fs::IByteStore {
public:
    NupFileStore(fs::ByteStorePtr source, const NupFile& file)
        : source_(std::move(source)), file_(file) {}

    std::int64_t capacity() const override
    {
        return file_.size <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? static_cast<std::int64_t>(file_.size) : 0;
    }

    bool cheapRandomAccess() const override { return file_.codec == 0; }

    bool validate() const override
    {
        std::call_once(validationOnce_, [this] {
            try {
                if (!source_ || file_.size >
                        std::uint64_t(std::numeric_limits<std::size_t>::max()))
                    return;
                if (file_.codec == 0) {
                    QByteArray digest;
                    valid_ = file_.extentSize == file_.size &&
                        hashRange(source_, file_.extentOffset, file_.extentSize, &digest) &&
                        std::memcmp(digest.constData(), file_.digest.data(), file_.digest.size()) == 0;
                    return;
                }
                if (file_.extentSize >
                    std::uint64_t(std::numeric_limits<std::size_t>::max()))
                    return;
                std::vector<std::uint8_t> packed(static_cast<std::size_t>(file_.extentSize));
                if (!packed.empty() && !readExact(source_, file_.extentOffset,
                                                  packed.data(), packed.size()))
                    return;
                if (!ZstdModule::decodePayload(packed, &decoded_) ||
                    decoded_.size() != static_cast<std::size_t>(file_.size)) {
                    decoded_.clear();
                    return;
                }
                QCryptographicHash hash(QCryptographicHash::Sha256);
                std::size_t done = 0;
                while (done < decoded_.size()) {
                    const int count = static_cast<int>(std::min<std::size_t>(
                        1024 * 1024, decoded_.size() - done));
                    hash.addData(reinterpret_cast<const char*>(decoded_.data() + done), count);
                    done += static_cast<std::size_t>(count);
                }
                const QByteArray digest = hash.result();
                valid_ = std::memcmp(digest.constData(), file_.digest.data(),
                                     file_.digest.size()) == 0;
                if (!valid_)
                    decoded_.clear();
            } catch (...) {
                decoded_.clear();
                valid_ = false;
            }
        });
        return valid_;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        const std::int64_t size = capacity();
        if (pos < 0 || count <= 0 || pos >= size || !validate())
            return 0;
        const int length = static_cast<int>(std::min<std::int64_t>(count, size - pos));
        if (file_.codec == 0)
            return source_->read(static_cast<std::int64_t>(file_.extentOffset) + pos,
                                 dst, length);
        std::copy(decoded_.begin() + static_cast<std::ptrdiff_t>(pos),
                  decoded_.begin() + static_cast<std::ptrdiff_t>(pos + length), dst);
        return length;
    }

private:
    fs::ByteStorePtr source_;
    NupFile file_;
    mutable std::once_flag validationOnce_;
    mutable bool valid_ = false;
    mutable std::vector<std::uint8_t> decoded_;
};

QString packageKindName(std::uint8_t kind)
{
    if (kind == 0) return QStringLiteral("base");
    if (kind == 1) return QStringLiteral("update");
    return QStringLiteral("add-on");
}

QString packageLabel(const NupPackage& package, std::size_t index)
{
    QString label = QStringLiteral("Package %1 - %2 - %3")
        .arg(static_cast<qulonglong>(index), 2, 10, QLatin1Char('0'))
        .arg(packageKindName(package.kind))
        .arg(package.titleId, 16, 16, QLatin1Char('0'));
    if (!package.title.isEmpty())
        label += QStringLiteral(" - ") +
            QString(package.title).replace(QLatin1Char('/'), QLatin1Char('_'))
                .replace(QLatin1Char('\\'), QLatin1Char('_'));
    return label;
}

bool parseMetadata(const fs::ByteStorePtr& source, NupPackage* package)
{
    if (!package || package->metaSize == 0)
        return true;
    std::uint64_t offset = package->metaOffset;
    const std::uint64_t end = offset + package->metaSize;
    std::vector<std::pair<std::uint16_t, QString>> textFields;
    while (offset < end) {
        if (end - offset < 8)
            return false;
        std::uint8_t header[8] = {};
        if (!readExact(source, offset, header, sizeof(header)) || le16(header + 2) != 0)
            return false;
        const std::uint16_t type = le16(header);
        const std::uint32_t size = le32(header + 4);
        offset += 8;
        if (size > end - offset)
            return false;
        if (type == 3 || type == 4 || type == 5 || type == 6) {
            std::vector<std::uint8_t> bytes(size);
            if (size && !readExact(source, offset, bytes.data(), bytes.size()))
                return false;
            const QString text = QString::fromUtf8(
                reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()));
            if (text.toUtf8() != QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                            static_cast<int>(bytes.size())))
                return false;
            textFields.emplace_back(type, text);
        }
        offset += size;
    }
    if (offset != end)
        return false;
    const bool zeroBasedIds = std::any_of(textFields.begin(), textFields.end(),
        [](const std::pair<std::uint16_t, QString>& field) { return field.first == 3; });
    for (const auto& field : textFields) {
        if (zeroBasedIds) {
            if (field.first == 3) package->displayVersion = field.second;
            if (field.first == 4) package->title = field.second;
            if (field.first == 5) package->publisher = field.second;
        } else {
            if (field.first == 4) package->displayVersion = field.second;
            if (field.first == 5) package->title = field.second;
            if (field.first == 6) package->publisher = field.second;
        }
    }
    return true;
}

} // namespace

ModulePtr NupModule::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<NupModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::NUP;
        module->info_.error = QStringLiteral("Could not open NUP source");
        return ModulePtr(std::move(module));
    }
    return open(source, filePath);
}

ModulePtr NupModule::open(const QByteArray& data, const QString& sourceName,
                          const QString& subPath)
{
    return open(std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        static_cast<std::size_t>(data.size())), sourceName, subPath);
}

ModulePtr NupModule::open(const fs::ByteStorePtr& source, const QString& sourceName,
                          const QString& subPath)
{
    auto module = peare::makeUnique<NupModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::NUP;
    module->info_.description = QStringLiteral("Nintendo Switch NUP container");
    if (!source || source->capacity() < static_cast<std::int64_t>(kSuperblockSize)) {
        module->info_.error = QStringLiteral("NUP source is truncated");
        return ModulePtr(std::move(module));
    }

    std::uint8_t header[kSuperblockSize] = {};
    if (!readExact(source, 0, header, sizeof(header)) ||
        std::memcmp(header, "NUP\x01", 4) != 0) {
        module->info_.error = QStringLiteral("Invalid NUP superblock");
        return ModulePtr(std::move(module));
    }
    const std::uint16_t headerSize = le16(header + 4);
    const std::uint16_t headerVersion = le16(header + 6);
    const std::uint64_t tocOffset = le64(header + 8);
    const std::uint64_t tocSize = le64(header + 16);
    const std::uint64_t blobOffset = le64(header + 56);
    const std::uint64_t fileSize = le64(header + 64);
    const std::uint32_t packageCount = le32(header + 72);
    const std::uint32_t flags = le32(header + 76);
    if (headerSize < kSuperblockSize || headerVersion == 0 || tocOffset != headerSize ||
        fileSize != static_cast<std::uint64_t>(source->capacity()) || packageCount == 0 ||
        flags != 0 || (blobOffset & 15u) != 0 || tocOffset > blobOffset ||
        !rangeFits(tocOffset, tocSize, blobOffset) || blobOffset > fileSize ||
        tocSize < kTocHeaderSize ||
        std::any_of(header + 0x60, header + 0x80,
                    [](std::uint8_t value) { return value != 0; })) {
        module->info_.error = QStringLiteral("Invalid NUP superblock fields");
        return ModulePtr(std::move(module));
    }

    QByteArray tocDigest;
    if (!hashRange(source, tocOffset, tocSize, &tocDigest) ||
        std::memcmp(tocDigest.constData(), header + 0x18, 32) != 0) {
        module->info_.error = QStringLiteral("NUP table-of-contents hash does not match");
        return ModulePtr(std::move(module));
    }

    std::uint8_t tocHeader[kTocHeaderSize] = {};
    if (!readExact(source, tocOffset, tocHeader, sizeof(tocHeader)) ||
        std::memcmp(tocHeader, "TOC\x01", 4) != 0 ||
        le32(tocHeader + 4) != packageCount || le64(tocHeader + 40) != 0) {
        module->info_.error = QStringLiteral("Invalid NUP table-of-contents header");
        return ModulePtr(std::move(module));
    }

    const std::uint64_t directoryCount64 = le64(tocHeader + 8);
    const std::uint64_t fileCount64 = le64(tocHeader + 16);
    const std::uint64_t stringPoolOffset = le64(tocHeader + 24);
    const std::uint64_t stringPoolSize = le64(tocHeader + 32);
    if (packageCount > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        directoryCount64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        fileCount64 > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        module->info_.error = QStringLiteral("NUP contains too many package or tree records");
        return ModulePtr(std::move(module));
    }
    const std::size_t directoryCount = static_cast<std::size_t>(directoryCount64);
    const std::size_t fileCount = static_cast<std::size_t>(fileCount64);
    const std::uint64_t packageOffset = tocOffset + kTocHeaderSize;
    const std::uint64_t directoryOffset = packageOffset +
        std::uint64_t(packageCount) * kPackageSize;
    const std::uint64_t fileOffset = directoryOffset + directoryCount64 * kDirectorySize;
    const std::uint64_t childOffset = fileOffset + fileCount64 * kFileSize;
    const std::uint64_t tocEnd = tocOffset + tocSize;
    if (packageOffset < tocOffset || directoryOffset < packageOffset ||
        fileOffset < directoryOffset || childOffset < fileOffset || childOffset > tocEnd ||
        stringPoolOffset < childOffset - tocOffset ||
        !rangeFits(stringPoolOffset, stringPoolSize, tocSize) ||
        stringPoolOffset + stringPoolSize > tocSize) {
        module->info_.error = QStringLiteral("NUP table-of-contents ranges are invalid");
        return ModulePtr(std::move(module));
    }

    std::vector<NupDirectory> directories(directoryCount);
    std::uint64_t childCount = 0;
    for (std::size_t i = 0; i < directoryCount; ++i) {
        std::uint8_t record[kDirectorySize] = {};
        if (!readExact(source, directoryOffset + i * kDirectorySize,
                       record, sizeof(record)) || le64(record + 16) != 0) {
            module->info_.error = QStringLiteral("Invalid NUP directory record");
            return ModulePtr(std::move(module));
        }
        directories[i].firstChild = le32(record + 8);
        directories[i].childCount = le32(record + 12);
        childCount += directories[i].childCount;
        if (childCount > (tocEnd - childOffset) / 4) {
            module->info_.error = QStringLiteral("NUP child table exceeds the TOC");
            return ModulePtr(std::move(module));
        }
    }
    const std::uint64_t childrenEnd = childOffset + childCount * 4;
    if (stringPoolOffset < childrenEnd - tocOffset) {
        module->info_.error = QStringLiteral("NUP string pool overlaps the child table");
        return ModulePtr(std::move(module));
    }

    std::vector<std::uint8_t> stringPool(static_cast<std::size_t>(stringPoolSize));
    if (stringPoolSize && !readExact(source, tocOffset + stringPoolOffset,
                                     stringPool.data(), stringPool.size())) {
        module->info_.error = QStringLiteral("Could not read the NUP string pool");
        return ModulePtr(std::move(module));
    }
    for (std::size_t i = 0; i < directoryCount; ++i) {
        std::uint8_t record[kDirectorySize] = {};
        if (!readExact(source, directoryOffset + i * kDirectorySize,
                       record, sizeof(record))) {
            module->info_.error = QStringLiteral("Could not read NUP directory names");
            return ModulePtr(std::move(module));
        }
        directories[i].name = utf8String(stringPool, le32(record), le32(record + 4), true);
        if (directories[i].name.isNull() ||
            directories[i].firstChild > childCount ||
            directories[i].childCount > childCount - directories[i].firstChild) {
            module->info_.error = QStringLiteral("Invalid NUP directory name or child range");
            return ModulePtr(std::move(module));
        }
    }

    std::vector<std::uint8_t> childBytes(static_cast<std::size_t>(childCount * 4));
    if (!childBytes.empty() && !readExact(source, childOffset,
                                          childBytes.data(), childBytes.size())) {
        module->info_.error = QStringLiteral("Could not read the NUP child table");
        return ModulePtr(std::move(module));
    }
    std::vector<std::uint32_t> children(static_cast<std::size_t>(childCount));
    for (std::size_t i = 0; i < children.size(); ++i)
        children[i] = le32(childBytes.data() + i * 4);
    std::vector<bool> childAssigned(children.size(), false);
    for (const NupDirectory& directory : directories) {
        for (std::uint64_t i = directory.firstChild;
             i < std::uint64_t(directory.firstChild) + directory.childCount; ++i) {
            if (childAssigned[static_cast<std::size_t>(i)]) {
                module->info_.error = QStringLiteral("NUP child ranges overlap");
                return ModulePtr(std::move(module));
            }
            childAssigned[static_cast<std::size_t>(i)] = true;
        }
    }
    if (std::find(childAssigned.begin(), childAssigned.end(), false) != childAssigned.end()) {
        module->info_.error = QStringLiteral("NUP child table contains unreferenced entries");
        return ModulePtr(std::move(module));
    }

    std::vector<NupFile> files(fileCount);
    std::vector<NupExtent> extents;
    extents.reserve(fileCount);
    for (std::size_t i = 0; i < fileCount; ++i) {
        std::uint8_t record[kFileSize] = {};
        if (!readExact(source, fileOffset + i * kFileSize, record, sizeof(record))) {
            module->info_.error = QStringLiteral("Could not read NUP file records");
            return ModulePtr(std::move(module));
        }
        NupFile& file = files[i];
        file.name = utf8String(stringPool, le32(record), le32(record + 4), false);
        file.size = le64(record + 8);
        file.extentOffset = le64(record + 16);
        file.extentSize = le64(record + 24);
        std::copy(record + 32, record + 64, file.digest.begin());
        file.codec = record[64];
        const bool reserved = record[65] != 0 || le16(record + 66) != 0 ||
            std::any_of(record + 68, record + 80,
                        [](std::uint8_t value) { return value != 0; });
        if (file.name.isNull() || file.size >
                std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
            file.codec > 1 || record[65] != 0 || reserved ||
            (file.size == 0 && (file.codec != 0 || file.extentOffset != 0 ||
                                file.extentSize != 0)) ||
            (file.size != 0 && (file.extentOffset < blobOffset ||
                (file.extentOffset & 15u) != 0 || file.extentSize == 0 ||
                !rangeFits(file.extentOffset, file.extentSize, fileSize) ||
                (file.codec == 0 && file.extentSize != file.size)))) {
            module->info_.error = QStringLiteral("Invalid NUP file record or extent");
            return ModulePtr(std::move(module));
        }
        if (file.size) {
            NupExtent extent;
            extent.begin = file.extentOffset;
            extent.end = file.extentOffset + file.extentSize;
            extent.size = file.size;
            extent.digest = file.digest;
            extent.codec = file.codec;
            extents.push_back(extent);
        }
    }

    std::vector<NupPackage> packages(packageCount);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> metadataRanges;
    for (std::size_t i = 0; i < packages.size(); ++i) {
        std::uint8_t record[kPackageSize] = {};
        if (!readExact(source, packageOffset + i * kPackageSize,
                       record, sizeof(record))) {
            module->info_.error = QStringLiteral("Could not read NUP package records");
            return ModulePtr(std::move(module));
        }
        NupPackage& package = packages[i];
        package.titleId = le64(record);
        package.rootDirectory = le32(record + 8);
        package.exefsDirectory = le32(record + 12);
        package.romfsDirectory = le32(record + 16);
        package.version = le32(record + 20);
        package.kind = record[24];
        package.flags = record[25];
        package.metaOffset = le64(record + 32);
        package.metaSize = le32(record + 40);
        if (le16(record + 26) != 0 || le32(record + 28) != 0 ||
            std::any_of(record + 44, record + 64,
                        [](std::uint8_t value) { return value != 0; }) ||
            package.kind > 2 || (package.flags & ~1u) != 0 ||
            package.rootDirectory >= directories.size() ||
            (package.exefsDirectory != kNoDirectory &&
                package.exefsDirectory >= directories.size()) ||
            (package.romfsDirectory != kNoDirectory &&
                package.romfsDirectory >= directories.size()) ||
            (package.metaSize == 0) != (package.metaOffset == 0) ||
            (package.metaSize && (package.metaOffset < blobOffset ||
                !rangeFits(package.metaOffset, package.metaSize, fileSize)))) {
            module->info_.error = QStringLiteral("Invalid NUP package record");
            return ModulePtr(std::move(module));
        }
        if (package.metaSize) {
            metadataRanges.emplace_back(package.metaOffset,
                package.metaOffset + package.metaSize);
            if (!parseMetadata(source, &package)) {
                module->info_.error = QStringLiteral("Invalid NUP package metadata block");
                return ModulePtr(std::move(module));
            }
        }
    }

    std::sort(extents.begin(), extents.end(), [](const NupExtent& left,
                                                  const NupExtent& right) {
        return left.begin < right.begin ||
            (left.begin == right.begin && left.end < right.end);
    });
    for (std::size_t i = 1; i < extents.size(); ++i) {
        const NupExtent& previous = extents[i - 1];
        const NupExtent& current = extents[i];
        if (current.begin >= previous.end)
            continue;
        const bool sameExtent = current.begin == previous.begin &&
            current.end == previous.end && current.size == previous.size &&
            current.codec == previous.codec && current.digest == previous.digest;
        if (!sameExtent) {
            module->info_.error = QStringLiteral("NUP file extents overlap or conflict");
            return ModulePtr(std::move(module));
        }
    }
    std::sort(metadataRanges.begin(), metadataRanges.end());
    for (std::size_t i = 1; i < metadataRanges.size(); ++i) {
        if (metadataRanges[i].first < metadataRanges[i - 1].second) {
            module->info_.error = QStringLiteral("NUP metadata blocks overlap");
            return ModulePtr(std::move(module));
        }
    }
    for (const NupExtent& extent : extents) {
        for (const auto& metadata : metadataRanges) {
            if (extent.begin < metadata.second && metadata.first < extent.end) {
                module->info_.error = QStringLiteral("NUP metadata overlaps a file extent");
                return ModulePtr(std::move(module));
            }
        }
    }

    QStringList metadataLines;
    metadataLines << QStringLiteral("Format: Nintendo Switch NUP")
                  << QStringLiteral("Version: 1")
                  << QStringLiteral("File size: %1 bytes").arg(fileSize)
                  << QStringLiteral("Packages: %1").arg(packageCount)
                  << QStringLiteral("Directories: %1").arg(directoryCount64)
                  << QStringLiteral("Files: %1").arg(fileCount64)
                  << QStringLiteral("TOC SHA-256: verified");
    ResourceEntry summary;
    summary.type = QStringLiteral("NUP_METADATA");
    summary.name = QStringLiteral("NUP metadata.txt");
    summary.language = QStringLiteral("neutral");
    summary.format = ModuleFormat::NUP;
    summary.data = metadataLines.join(QLatin1Char('\n')).toUtf8();
    summary.dataSize = static_cast<quint64>(summary.data.size());
    summary.isEmbeddedFile = true;
    summary.content = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(summary.data.constData()),
        static_cast<std::size_t>(summary.data.size()));
    module->resources_.append(std::move(summary));

    const QString requestedPath = subPath.trimmed();
    bool requestedFound = requestedPath.isEmpty();
    for (std::size_t packageIndex = 0; packageIndex < packages.size(); ++packageIndex) {
        const NupPackage& package = packages[packageIndex];
        const QString packageName = packageLabel(package, packageIndex);
        if (!requestedPath.isEmpty() &&
            requestedPath != packageName && !requestedPath.startsWith(packageName + QLatin1Char('/')))
            continue;
        requestedFound = true;
        struct PendingDirectory { std::uint32_t index; QStringList path; };
        std::vector<PendingDirectory> pending;
        pending.push_back({package.rootDirectory, QStringList() << packageName});
        std::vector<bool> visitedDirectories(directories.size(), false);
        std::vector<bool> visitedFiles(files.size(), false);
        while (!pending.empty()) {
            PendingDirectory current = std::move(pending.back());
            pending.pop_back();
            if (visitedDirectories[current.index]) {
                module->info_.error = QStringLiteral("NUP directory graph contains a cycle or duplicate link");
                return ModulePtr(std::move(module));
            }
            visitedDirectories[current.index] = true;
            const NupDirectory& directory = directories[current.index];
            QSet<QString> childNames;
            for (std::uint64_t childIndex = directory.firstChild;
                 childIndex < std::uint64_t(directory.firstChild) + directory.childCount;
                 ++childIndex) {
                const std::uint32_t child = children[static_cast<std::size_t>(childIndex)];
                if (child < directories.size()) {
                    const NupDirectory& childDirectory = directories[child];
                    if (childDirectory.name.isEmpty() || childNames.contains(childDirectory.name)) {
                        module->info_.error = QStringLiteral("NUP directory has an invalid or duplicate child name");
                        return ModulePtr(std::move(module));
                    }
                    childNames.insert(childDirectory.name);
                    QStringList childPath = current.path;
                    ResourceEntry entry;
                    entry.type = QStringLiteral("NUP_DIR");
                    entry.name = childDirectory.name;
                    entry.language = QStringLiteral("neutral");
                    entry.format = ModuleFormat::NUP;
                    entry.hierarchyPath = childPath;
                    entry.content = source;
                    entry.isDirectory = true;
                    childPath.append(childDirectory.name);
                    entry.containerSubPath = childPath.join(QLatin1Char('/'));
                    module->resources_.append(std::move(entry));
                    if (requestedPath.isEmpty() ||
                        childPath.join(QLatin1Char('/')).startsWith(requestedPath + QLatin1Char('/')) ||
                        requestedPath.startsWith(childPath.join(QLatin1Char('/')) + QLatin1Char('/')))
                        pending.push_back({child, std::move(childPath)});
                } else {
                    const std::uint32_t fileIndex = child - static_cast<std::uint32_t>(directories.size());
                    if (fileIndex >= files.size()) {
                        module->info_.error = QStringLiteral("NUP child table references an invalid file");
                        return ModulePtr(std::move(module));
                    }
                    const NupFile& file = files[fileIndex];
                    if (file.name.isEmpty() || childNames.contains(file.name) || visitedFiles[fileIndex]) {
                        module->info_.error = QStringLiteral("NUP directory has an invalid or duplicate file");
                        return ModulePtr(std::move(module));
                    }
                    childNames.insert(file.name);
                    visitedFiles[fileIndex] = true;
                    QStringList filePath = current.path;
                    filePath.append(file.name);
                    const QString fullPath = filePath.join(QLatin1Char('/'));
                    if (!requestedPath.isEmpty() && !fullPath.startsWith(requestedPath + QLatin1Char('/')))
                        continue;
                    ResourceEntry entry;
                    entry.type = QStringLiteral("NUP_FILE");
                    entry.name = file.name;
                    entry.language = QStringLiteral("neutral");
                    entry.format = ModuleFormat::NUP;
                    entry.hierarchyPath = current.path;
                    entry.dataOffset = file.extentOffset;
                    entry.dataSize = file.size;
                    entry.isEmbeddedFile = true;
                    entry.content = std::make_shared<NupFileStore>(source, file);
                    module->resources_.append(std::move(entry));
                }
            }
        }
    }
    if (!requestedFound) {
        module->resources_.clear();
        return ModulePtr(std::move(module));
    }
    return ModulePtr(std::move(module));
}

} // namespace peare
