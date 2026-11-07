#include "XarModule.h"

#include "Bzip2Module.h"
#include "DeflateDecoder.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QXmlStreamReader>

#include <algorithm>
#include <limits>
#include <mutex>
#include <vector>

namespace peare {
namespace {

const std::uint64_t kHeaderSize = 28;
const std::uint64_t kMaxTocSize = 256ull * 1024ull * 1024ull;

std::uint16_t be16(const std::uint8_t* p)
{
    return std::uint16_t((std::uint16_t(p[0]) << 8) | p[1]);
}

std::uint32_t be32(const std::uint8_t* p)
{
    return (std::uint32_t(be16(p)) << 16) | be16(p + 2);
}

std::uint64_t be64(const std::uint8_t* p)
{
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

bool readVector(const fs::ByteStorePtr& source, std::uint64_t offset,
                std::uint64_t length, std::vector<std::uint8_t>* out)
{
    if (!source || offset > std::uint64_t(source->capacity()) ||
        length > std::uint64_t(source->capacity()) - offset ||
        length > std::uint64_t(std::numeric_limits<std::size_t>::max())) return false;
    out->resize(std::size_t(length));
    std::uint64_t done = 0;
    while (done < length) {
        const int request = int(std::min<std::uint64_t>(length - done, 4 * 1024 * 1024));
        const int count = source->read(std::int64_t(offset + done), out->data() + std::size_t(done), request);
        if (count <= 0 || count > request) return false;
        done += std::uint64_t(count);
    }
    return true;
}

QCryptographicHash::Algorithm digestAlgorithm(const QString& style, bool* ok)
{
    const QString normalized = style.trimmed().toLower();
    *ok = true;
    if (normalized == QLatin1String("sha1")) return QCryptographicHash::Sha1;
    if (normalized == QLatin1String("md5")) return QCryptographicHash::Md5;
    if (normalized == QLatin1String("sha256")) return QCryptographicHash::Sha256;
    if (normalized == QLatin1String("sha512")) return QCryptographicHash::Sha512;
    *ok = false;
    return QCryptographicHash::Md5;
}

bool matchesDigest(const QString& style, const QByteArray& expected,
                   const std::uint8_t* bytes, std::size_t length)
{
    bool supported = false;
    const QCryptographicHash::Algorithm algorithm = digestAlgorithm(style, &supported);
    if (!supported || (length && !bytes)) return false;
    QCryptographicHash hash(algorithm);
    std::size_t done = 0;
    while (done < length) {
        const int count = int(std::min<std::size_t>(length - done, 4 * 1024 * 1024));
        hash.addData(reinterpret_cast<const char*>(bytes + done), count);
        done += std::size_t(count);
    }
    const QByteArray actual = hash.result().toHex();
    return actual.compare(expected.trimmed(), Qt::CaseInsensitive) == 0;
}

QString cleanName(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    QStringList parts;
    const QStringList input = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : input) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String("..")) return QString();
        parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

class XarDecodedStore final : public fs::IByteStore {
public:
    XarDecodedStore(fs::ByteStorePtr source, std::uint64_t offset, std::uint64_t length,
                    std::uint64_t size, QString encoding, QString name,
                    QString archivedStyle, QByteArray archivedDigest,
                    QString extractedStyle, QByteArray extractedDigest)
        : source_(std::move(source)), offset_(offset), length_(length), size_(size),
          encoding_(std::move(encoding)), name_(std::move(name)),
          archivedStyle_(std::move(archivedStyle)), archivedDigest_(std::move(archivedDigest)),
          extractedStyle_(std::move(extractedStyle)), extractedDigest_(std::move(extractedDigest)) {}

    std::int64_t capacity() const override
    {
        return size_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(size_) : 0;
    }

    bool cheapRandomAccess() const override { return false; }

    bool validate() const override
    {
        if (!source_) return false;
        if ((encoding_.isEmpty() || encoding_ == QLatin1String("application/octet-stream")) &&
            archivedDigest_.isEmpty() && extractedDigest_.isEmpty())
            return length_ == size_ && offset_ <= std::uint64_t(source_->capacity()) &&
                   length_ <= std::uint64_t(source_->capacity()) - offset_;
        load();
        return valid_;
    }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (pos < 0 || count <= 0 || std::uint64_t(pos) >= size_ || !dst) return 0;
        if (!source_) return 0;
        if ((encoding_.isEmpty() || encoding_ == QLatin1String("application/octet-stream")) &&
            archivedDigest_.isEmpty() && extractedDigest_.isEmpty())
            return source_->read(std::int64_t(offset_) + pos, dst,
                                 int(std::min<std::uint64_t>(std::uint64_t(count), size_ - std::uint64_t(pos))));
        load();
        if (!valid_) return 0;
        const std::size_t n = std::size_t(std::min<std::uint64_t>(
            std::uint64_t(count), size_ - std::uint64_t(pos)));
        std::memcpy(dst, decoded_.data() + std::size_t(pos), n);
        return int(n);
    }

private:
    void load() const
    {
        std::call_once(once_, [this] {
            if (size_ > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
                length_ > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
                offset_ > std::uint64_t(source_->capacity()) ||
                length_ > std::uint64_t(source_->capacity()) - offset_) return;
            std::vector<std::uint8_t> packed;
            if (!readVector(source_, offset_, length_, &packed)) return;
            if (!archivedDigest_.isEmpty() && !matchesDigest(
                    archivedStyle_, archivedDigest_, packed.data(), packed.size())) return;
            if (encoding_.isEmpty() || encoding_ == QLatin1String("application/octet-stream")) {
                if (length_ != size_) return;
                decoded_ = std::move(packed);
            } else if (encoding_ == QLatin1String("application/x-gzip")) {
                std::string error;
                if (!compression::inflateZlibExact(packed, std::size_t(size_), &decoded_, &error)) return;
            } else if (encoding_ == QLatin1String("application/x-bzip2")) {
                fs::ByteStorePtr member = std::make_shared<fs::MemoryStore>(std::move(packed));
                ModulePtr decodedModule = Bzip2Module::open(member, name_);
                if (!decodedModule || !decodedModule->info().isValid()) return;
                const IResourceContainer* container = dynamic_cast<const IResourceContainer*>(decodedModule.get());
                if (!container || container->resources().isEmpty()) return;
                const fs::ByteStorePtr content = container->resources().first().content;
                if (!content || std::uint64_t(content->capacity()) != size_) return;
                if (size_ > std::uint64_t(std::numeric_limits<std::size_t>::max())) return;
                decoded_.resize(std::size_t(size_));
                if (!content->copyTo(decoded_.data(), decoded_.size())) return;
            } else {
                return;
            }
            valid_ = decoded_.size() == std::size_t(size_);
            if (valid_ && !extractedDigest_.isEmpty())
                valid_ = matchesDigest(extractedStyle_, extractedDigest_, decoded_.data(), decoded_.size());
        });
    }

    fs::ByteStorePtr source_;
    std::uint64_t offset_;
    std::uint64_t length_;
    std::uint64_t size_;
    QString encoding_;
    QString name_;
    QString archivedStyle_;
    QByteArray archivedDigest_;
    QString extractedStyle_;
    QByteArray extractedDigest_;
    mutable std::once_flag once_;
    mutable bool valid_ = false;
    mutable std::vector<std::uint8_t> decoded_;
};

struct ParsedFile {
    QString name;
    QString type;
    QString link;
    QString encoding;
    QString archivedStyle;
    QByteArray archivedDigest;
    QString extractedStyle;
    QByteArray extractedDigest;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
    std::uint64_t size = 0;
    bool hasData = false;
    bool hasOffset = false;
    bool hasLength = false;
    bool hasSize = false;
};

bool parseUnsigned(const QString& text, std::uint64_t* value)
{
    bool ok = false;
    const qulonglong parsed = text.trimmed().toULongLong(&ok, 10);
    if (!ok) return false;
    *value = std::uint64_t(parsed);
    return true;
}

bool parseData(QXmlStreamReader& xml, ParsedFile* file)
{
    while (xml.readNextStartElement()) {
        const QString tag = xml.name().toString();
        if (tag == QLatin1String("offset") || tag == QLatin1String("length") ||
            tag == QLatin1String("size")) {
            std::uint64_t value = 0;
            if (!parseUnsigned(xml.readElementText(), &value)) return false;
            if (tag == QLatin1String("offset")) { file->offset = value; file->hasOffset = true; }
            else if (tag == QLatin1String("length")) { file->length = value; file->hasLength = true; }
            else { file->size = value; file->hasSize = true; }
        } else if (tag == QLatin1String("encoding")) {
            file->encoding = xml.attributes().value(QLatin1String("style")).toString();
            xml.skipCurrentElement();
        } else if (tag == QLatin1String("archived-checksum") ||
                   tag == QLatin1String("extracted-checksum")) {
            const QString style = xml.attributes().value(QLatin1String("style")).toString();
            const QByteArray digest = xml.readElementText().toLatin1().trimmed();
            if (tag == QLatin1String("archived-checksum")) {
                file->archivedStyle = style;
                file->archivedDigest = digest;
            } else {
                file->extractedStyle = style;
                file->extractedDigest = digest;
            }
        } else {
            xml.skipCurrentElement();
        }
    }
    file->hasData = !xml.hasError() && file->hasOffset && file->hasLength && file->hasSize;
    return file->hasData;
}

struct TocChecksum {
    QString style;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    bool hasOffset = false;
    bool hasSize = false;
};

bool parseTocChecksum(QXmlStreamReader& xml, TocChecksum* checksum)
{
    checksum->style = xml.attributes().value(QLatin1String("style")).toString();
    while (xml.readNextStartElement()) {
        const QString tag = xml.name().toString();
        if (tag == QLatin1String("offset") || tag == QLatin1String("size")) {
            std::uint64_t value = 0;
            if (!parseUnsigned(xml.readElementText(), &value)) return false;
            if (tag == QLatin1String("offset")) {
                checksum->offset = value;
                checksum->hasOffset = true;
            } else {
                checksum->size = value;
                checksum->hasSize = true;
            }
        } else {
            xml.skipCurrentElement();
        }
    }
    return !xml.hasError() && checksum->hasOffset && checksum->hasSize;
}

bool parseFile(QXmlStreamReader& xml, const QString& parent,
               const fs::ByteStorePtr& source, std::uint64_t heapBase,
               QVector<ResourceEntry>* entries,
               int* index)
{
    ParsedFile file;
    QStringList children;
    while (xml.readNextStartElement()) {
        const QString tag = xml.name().toString();
        if (tag == QLatin1String("name")) file.name = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        else if (tag == QLatin1String("type")) file.type = xml.readElementText().trimmed();
        else if (tag == QLatin1String("link")) file.link = xml.readElementText(QXmlStreamReader::IncludeChildElements);
        else if (tag == QLatin1String("data")) {
            if (!parseData(xml, &file)) return false;
        } else if (tag == QLatin1String("file")) {
            if (!parseFile(xml, parent + QLatin1Char('/') + file.name, source, heapBase,
                           entries, index)) return false;
        } else {
            xml.skipCurrentElement();
        }
    }
    if (xml.hasError()) return false;
    const QString safe = cleanName(parent.isEmpty() ? file.name : parent + QLatin1Char('/') + file.name);
    if (safe.isEmpty()) return true;
    if (file.type == QLatin1String("directory")) {
        ResourceEntry entry;
        entry.type = QStringLiteral("XAR_DIR");
        entry.name = safe.section(QLatin1Char('/'), -1);
        entry.hierarchyPath = safe.section(QLatin1Char('/'), 0, -2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::XAR;
        entry.isDirectory = true;
        entry.containerSubPath = safe;
        entries->push_back(std::move(entry));
    } else if (file.type == QLatin1String("file")) {
        const std::uint64_t heap = std::uint64_t(source->capacity());
        if (file.offset > heap || file.length > heap - file.offset) return false;
        if (file.hasData &&
            (file.encoding.isEmpty() || file.encoding == QLatin1String("application/octet-stream")) &&
            file.length != file.size) return false;
        if (file.size > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
            file.length > std::uint64_t(std::numeric_limits<std::int64_t>::max())) return false;
        ResourceEntry entry;
        entry.type = QStringLiteral("XAR_FILE");
        entry.name = safe.section(QLatin1Char('/'), -1);
        entry.hierarchyPath = safe.section(QLatin1Char('/'), 0, -2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = heapBase + file.offset;
        entry.dataSize = file.size;
        entry.format = ModuleFormat::XAR;
        entry.isEmbeddedFile = true;
        entry.content = std::make_shared<XarDecodedStore>(source, file.offset, file.length,
            file.size, file.encoding, safe, file.archivedStyle, file.archivedDigest,
            file.extractedStyle, file.extractedDigest);
        entries->push_back(std::move(entry));
        ++*index;
    }
    return true;
}

} // namespace

ModulePtr XarModule::open(const QString& filePath)
{
    return open(filePath, QFileInfo(filePath).fileName());
}

ModulePtr XarModule::open(const QString& filePath, const QString& logicalName)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), logicalName);
}

ModulePtr XarModule::open(const QByteArray& data, const QString& logicalName)
{
    const auto source = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
    return open(source, logicalName);
}

ModulePtr XarModule::open(const fs::ByteStorePtr& file, const QString& sourceName)
{
    std::unique_ptr<XarModule> module(new XarModule());
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::XAR;
    module->info_.description = QStringLiteral("XAR archive");
    const auto fail = [&module](const QString& error) -> ModulePtr {
        module->info_.error = error;
        module->resources_.clear();
        return ModulePtr(module.release());
    };
    if (!file || file->capacity() < std::int64_t(kHeaderSize))
        return fail(QStringLiteral("Truncated XAR header"));
    const std::vector<std::uint8_t> header = file->readRange(0, kHeaderSize);
    if (header.size() != kHeaderSize || std::memcmp(header.data(), "xar!", 4) != 0)
        return fail(QStringLiteral("Invalid XAR signature"));
    const std::uint16_t headerSize = be16(header.data() + 4);
    const std::uint16_t version = be16(header.data() + 6);
    const std::uint64_t compressedSize = be64(header.data() + 8);
    const std::uint64_t expandedSize = be64(header.data() + 16);
    const std::uint32_t checksumAlgorithm = be32(header.data() + 24);
    if (headerSize < kHeaderSize || headerSize > std::uint64_t(file->capacity()) ||
        version != 1 || expandedSize > kMaxTocSize ||
        compressedSize > kMaxTocSize || checksumAlgorithm > 3 ||
        compressedSize > std::uint64_t(file->capacity()) - headerSize ||
        expandedSize > std::uint64_t(std::numeric_limits<std::size_t>::max()))
        return fail(QStringLiteral("Unsupported or invalid XAR header"));
    const std::vector<std::uint8_t> completeHeader = file->readRange(0, headerSize);
    if (completeHeader.size() != headerSize)
        return fail(QStringLiteral("Truncated XAR extended header"));
    QString headerChecksumStyle;
    if (checksumAlgorithm == 1) headerChecksumStyle = QStringLiteral("sha1");
    else if (checksumAlgorithm == 2) headerChecksumStyle = QStringLiteral("md5");
    else if (checksumAlgorithm == 3) {
        if (headerSize <= kHeaderSize) return fail(QStringLiteral("Missing XAR checksum name"));
        headerChecksumStyle = QString::fromLatin1(
            reinterpret_cast<const char*>(completeHeader.data() + kHeaderSize), int(headerSize - kHeaderSize));
        const int nul = headerChecksumStyle.indexOf(QLatin1Char('\0'));
        if (nul >= 0) headerChecksumStyle.truncate(nul);
        bool supported = false;
        digestAlgorithm(headerChecksumStyle, &supported);
        if (!supported) return fail(QStringLiteral("Unsupported XAR checksum algorithm"));
    }
    std::vector<std::uint8_t> compressed = file->readRange(headerSize, std::int64_t(compressedSize));
    if (compressed.size() != std::size_t(compressedSize))
        return fail(QStringLiteral("Truncated XAR table of contents"));
    std::vector<std::uint8_t> toc;
    std::string inflateError;
    if (!compression::inflateZlibExact(compressed, std::size_t(expandedSize), &toc, &inflateError))
        return fail(QStringLiteral("Invalid XAR table of contents: %1").arg(QString::fromStdString(inflateError)));
    const std::uint64_t heapStart = std::uint64_t(headerSize) + compressedSize;
    const std::int64_t heapSize = file->capacity() - std::int64_t(heapStart);
    const fs::ByteStorePtr heap = std::make_shared<fs::SubStore>(file, std::int64_t(heapStart), heapSize);
    const QByteArray xmlBytes(reinterpret_cast<const char*>(toc.data()), int(toc.size()));
    QXmlStreamReader xml(xmlBytes);
    int count = 0;
    bool foundToc = false;
    TocChecksum tocChecksum;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("toc")) {
            foundToc = true;
            while (xml.readNextStartElement()) {
                if (xml.name() == QLatin1String("checksum")) {
                    if (!parseTocChecksum(xml, &tocChecksum))
                        return fail(QStringLiteral("Invalid XAR table checksum"));
                } else if (xml.name() == QLatin1String("file")) {
                    if (!parseFile(xml, QString(), heap, heapStart,
                                   &module->resources_, &count))
                        return fail(QStringLiteral("Invalid XAR file table"));
                } else {
                    xml.skipCurrentElement();
                }
            }
        }
    }
    if (xml.hasError() || !foundToc)
        return fail(QStringLiteral("Malformed XAR table of contents"));
    if (checksumAlgorithm != 0) {
        bool supported = false;
        const QCryptographicHash::Algorithm algorithm = digestAlgorithm(headerChecksumStyle, &supported);
        if (!supported || tocChecksum.style != headerChecksumStyle ||
            tocChecksum.offset > std::uint64_t(heap->capacity()) ||
            tocChecksum.size > std::uint64_t(heap->capacity()) - tocChecksum.offset ||
            tocChecksum.size > std::uint64_t(std::numeric_limits<int>::max()))
            return fail(QStringLiteral("Invalid or unsupported XAR table checksum"));
        const std::vector<std::uint8_t> expected = heap->readRange(
            std::int64_t(tocChecksum.offset), std::int64_t(tocChecksum.size));
        const QByteArray actual = QCryptographicHash::hash(
            QByteArray(reinterpret_cast<const char*>(compressed.data()), int(compressed.size())), algorithm);
        if (expected.size() != std::size_t(actual.size()) ||
            !std::equal(expected.begin(), expected.end(),
                        reinterpret_cast<const std::uint8_t*>(actual.constData())))
            return fail(QStringLiteral("XAR table checksum mismatch"));
    }
    module->info_.headerOffset = 0;
    return ModulePtr(module.release());
}

} // namespace peare
