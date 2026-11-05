#include "Rar4Module.h"
#include "Rar4LzDecoder.h"
#include "Compat.h"
#include "../compression/peare/Rar4Crypto.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <vector>

namespace peare {
namespace {

const std::uint8_t kMarker[] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00};
const std::int64_t kMaxMarkerSearch = 1024 * 1024;
const std::uint64_t kMaxEntryCount = 1000000;

std::uint16_t le16(const std::uint8_t* p)
{
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint32_t crc32Update(std::uint32_t crc, const std::uint8_t* data, std::size_t size)
{
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0U);
    }
    return crc;
}

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    return crc32Update(0xffffffffU, data, size) ^ 0xffffffffU;
}

class Rar4JoinedStore final : public fs::IByteStore {
public:
    explicit Rar4JoinedStore(const QStringList& paths)
    {
        for (const QString& path : paths) {
            auto store = std::make_shared<fs::QFileByteStore>(path);
            if (!store->validate() || store->capacity() < 0 ||
                store->capacity() > std::numeric_limits<std::int64_t>::max() - size_) {
                stores_.clear();
                starts_.clear();
                size_ = 0;
                return;
            }
            starts_.push_back(size_);
            size_ += store->capacity();
            stores_.push_back(std::move(store));
        }
    }

    std::int64_t capacity() const override { return size_; }
    bool cheapRandomAccess() const override { return false; }
    bool validate() const override { return !stores_.empty(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (!dst || count <= 0 || pos < 0 || pos >= size_) return 0;
        const int wanted = int(std::min<std::int64_t>(count, size_ - pos));
        int done = 0;
        while (done < wanted) {
            const std::int64_t absolute = pos + done;
            const auto it = std::upper_bound(starts_.begin(), starts_.end(), absolute);
            const std::size_t index = std::size_t(it - starts_.begin() - 1);
            const std::int64_t local = absolute - starts_[index];
            const std::int64_t end = index + 1 < starts_.size()
                ? starts_[index + 1] : size_;
            const int amount = int(std::min<std::int64_t>(wanted - done, end - absolute));
            const int got = stores_[index]->read(local, dst + done, amount);
            if (got <= 0 || got > amount) break;
            done += got;
        }
        return done;
    }

private:
    std::vector<fs::ByteStorePtr> stores_;
    std::vector<std::int64_t> starts_;
    std::int64_t size_ = 0;
};

QStringList rar4VolumePaths(const QString& filePath)
{
    QStringList paths{filePath};
    const QFileInfo info(filePath);
    const QString name = info.fileName();
    const int partMarker = name.lastIndexOf(QStringLiteral(".part"), -1, Qt::CaseInsensitive);
    if (partMarker >= 0 && name.endsWith(QStringLiteral(".rar"), Qt::CaseInsensitive)) {
        const int digitsStart = partMarker + 5;
        const int digitsEnd = name.indexOf(QLatin1Char('.'), digitsStart);
        bool ok = false;
        const int first = name.mid(digitsStart, digitsEnd - digitsStart).toInt(&ok);
        if (!ok || first < 1) return paths;
        const QString prefix = name.left(partMarker + 5);
        const QString suffix = name.mid(digitsEnd);
        for (int part = first + 1; part <= 10000; ++part) {
            const QString next = info.dir().filePath(prefix + QString::number(part) + suffix);
            if (!QFileInfo::exists(next)) break;
            paths.push_back(next);
        }
        return paths;
    }
    if (name.endsWith(QStringLiteral(".rar"), Qt::CaseInsensitive)) {
        const QString stem = filePath.left(filePath.size() - 4);
        for (int part = 0; part < 10000; ++part) {
            const QChar letter = QLatin1Char(part < 100 ? 'r' : 's');
            const QString extension = QStringLiteral(".%1%2")
                .arg(letter).arg(part % 100, 2, 10, QLatin1Char('0'));
            const QString next = stem + extension;
            if (!QFileInfo::exists(next)) break;
            paths.push_back(next);
        }
    }
    return paths;
}

bool readExact(const fs::IByteStore& store, std::int64_t offset,
               std::uint8_t* dst, int count)
{
    const std::int64_t capacity = store.capacity();
    if (capacity < 0 || offset < 0 || count < 0 || count > capacity ||
        offset > capacity - count)
        return false;
    int done = 0;
    while (done < count) {
        const int got = store.read(offset + done, dst + done, count - done);
        if (got <= 0 || got > count - done) return false;
        done += got;
    }
    return true;
}

bool findMarker(const fs::IByteStore& source, std::int64_t* offset)
{
    if (!offset || source.capacity() < std::int64_t(sizeof(kMarker))) return false;
    const std::int64_t limit = std::min<std::int64_t>(
        source.capacity() - std::int64_t(sizeof(kMarker)) + 1, kMaxMarkerSearch + 1);
    std::uint8_t window[4096 + sizeof(kMarker) - 1];
    for (std::int64_t base = 0; base < limit;) {
        const int want = int(std::min<std::int64_t>(4096, limit - base));
        const int tail = base == 0 ? 0 : int(sizeof(kMarker) - 1);
        if (tail && !readExact(source, base - tail, window, tail)) return false;
        if (!readExact(source, base, window + tail, want)) return false;
        const int total = tail + want;
        for (int i = 0; i + int(sizeof(kMarker)) <= total; ++i) {
            if (std::memcmp(window + i, kMarker, sizeof(kMarker)) == 0) {
                *offset = base - tail + i;
                return true;
            }
        }
        base += want;
    }
    return false;
}

QString safeMemberName(const QByteArray& raw, bool unicodeFlag)
{
    QByteArray bytes = raw;
    QString name;
    if (unicodeFlag) {
        const int separator = bytes.indexOf('\0');
        if (separator < 0) {
            name = QString::fromUtf8(bytes.constData(), bytes.size());
            if (name.toUtf8() != bytes) return QString();
        } else {
            // Keep the legacy spelling when the RAR Unicode token stream is not
            // decoded; it remains a safe, deterministic path for older viewers.
            name = QString::fromLocal8Bit(bytes.constData(), separator);
        }
    } else {
        name = QString::fromLocal8Bit(bytes);
    }
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.startsWith(QLatin1Char('/')) || name.contains(QChar(0))) return QString();
    QStringList parts;
    const QStringList rawParts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (QString part : rawParts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) return QString();
        for (QChar& c : part) {
            if (c.unicode() < 0x20 || QStringLiteral("<>:\"|?*").contains(c))
                c = QLatin1Char('_');
        }
        if (!part.isEmpty()) parts.push_back(part);
    }
    return parts.join(QLatin1Char('/'));
}

struct Rar4MemberRecord {
    struct DataPart {
        std::int64_t offset = 0;
        std::uint64_t packed = 0;
    };
    std::int64_t offset = 0;
    std::uint64_t packed = 0;
    std::vector<DataPart> parts;
    std::uint64_t unpacked = 0;
    std::uint32_t crc = 0;
    std::uint8_t method = 0;
    std::size_t dictionarySize = 0;
    bool solid = false;
    bool split = false;
    bool encrypted = false;
    std::array<std::uint8_t, 8> salt = {};
    bool hasSalt = false;
    bool valid = false;
    std::vector<std::uint8_t> decoded;
};

class Rar4ArchiveDecode final {
public:
    Rar4ArchiveDecode(fs::ByteStorePtr source, QString password)
        : source_(std::move(source)), password_(std::move(password)) {}

    std::size_t add(Rar4MemberRecord record)
    {
        records_.push_back(std::move(record));
        return records_.size() - 1;
    }

    bool appendPart(std::size_t index, std::int64_t offset, std::uint64_t packed,
                    std::uint32_t crc, bool finalPart)
    {
        if (index >= records_.size() || offset < 0 ||
            packed > std::numeric_limits<std::uint64_t>::max() - records_[index].packed)
            return false;
        Rar4MemberRecord& record = records_[index];
        record.parts.push_back({offset, packed});
        record.packed += packed;
        if (finalPart) {
            record.crc = crc;
            record.split = false;
        }
        return true;
    }

    bool validate(std::size_t index) const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!source_ || index >= records_.size()) return false;
        for (; next_ <= index; ++next_) {
            try {
                if (!process(records_[next_])) {
                    chainUsable_ = false;
                    lzState_.resetPpmd();
                }
            } catch (...) {
                records_[next_].valid = false;
                records_[next_].decoded.clear();
                chainUsable_ = false;
                lzState_.resetPpmd();
            }
        }
        return records_[index].valid;
    }

    int read(std::size_t index, std::int64_t pos, std::uint8_t* dst, int count) const
    {
        if (!dst || count <= 0 || pos < 0 || !validate(index)) return 0;
        const Rar4MemberRecord& record = records_[index];
        if (std::uint64_t(pos) >= record.unpacked) return 0;
        const int n = int(std::min<std::uint64_t>(count, record.unpacked - std::uint64_t(pos)));
        if (record.method == 0x30 && record.parts.empty() && !record.encrypted)
            return source_->read(record.offset + pos, dst, n);
        if (std::size_t(pos) > record.decoded.size() ||
            std::size_t(n) > record.decoded.size() - std::size_t(pos))
            return 0;
        std::memcpy(dst, record.decoded.data() + std::size_t(pos), std::size_t(n));
        return n;
    }

private:
    bool process(Rar4MemberRecord& record) const
    {
        record.valid = false;
        record.decoded.clear();
        if (!record.solid) {
            lzState_.reset();
            chainUsable_ = true;
        } else if (!chainUsable_) {
            return false;
        }
        if (record.split || (record.encrypted && (!record.hasSalt || password_.isNull())) ||
            record.offset < 0 ||
            record.packed > std::numeric_limits<std::size_t>::max() ||
            record.unpacked > std::numeric_limits<std::size_t>::max())
            return false;
        const std::int64_t sourceSize = source_->capacity();
        if (sourceSize < 0) return false;
        const auto partInBounds = [&](std::int64_t offset, std::uint64_t packed) {
            return offset >= 0 && packed <= std::uint64_t(sourceSize) &&
                std::uint64_t(offset) <= std::uint64_t(sourceSize) - packed;
        };
        if (record.parts.empty()) {
            if (!partInBounds(record.offset, record.packed)) return false;
        } else {
            for (const Rar4MemberRecord::DataPart& part : record.parts)
                if (!partInBounds(part.offset, part.packed)) return false;
        }

        if (record.method == 0x30 && !record.encrypted && record.parts.empty()) {
            if (record.packed != record.unpacked) return false;
            std::uint8_t buffer[64 * 1024];
            std::uint64_t done = 0;
            std::uint32_t crc = 0xffffffffU;
            while (done < record.packed) {
                const int amount = int(std::min<std::uint64_t>(sizeof(buffer), record.packed - done));
                if (!readExact(*source_, record.offset + std::int64_t(done), buffer, amount))
                    return false;
                crc = crc32Update(crc, buffer, std::size_t(amount));
                for (int i = 0; i < amount; ++i) lzState_.pushHistory(buffer[i]);
                done += std::uint64_t(amount);
            }
            record.valid = (crc ^ 0xffffffffU) == record.crc;
            return record.valid;
        }
        if ((record.method != 0x30 &&
             (record.method < 0x31 || record.method > 0x35)) ||
            record.packed > std::numeric_limits<std::size_t>::max() ||
            record.unpacked > std::numeric_limits<std::size_t>::max())
            return false;

        std::vector<std::uint8_t> packed(static_cast<std::size_t>(record.packed));
        std::uint64_t done = 0;
        const auto readPart = [&](std::int64_t offset, std::uint64_t length) {
            std::uint64_t partDone = 0;
            while (partDone < length) {
                const int amount = int(std::min<std::uint64_t>(64 * 1024, length - partDone));
                if (!readExact(*source_, offset + std::int64_t(partDone),
                        packed.data() + std::size_t(done + partDone), amount)) return false;
                partDone += std::uint64_t(amount);
            }
            done += length;
            return true;
        };
        if (record.parts.empty()) {
            if (!readPart(record.offset, record.packed)) return false;
        } else {
            for (const Rar4MemberRecord::DataPart& part : record.parts)
                if (!readPart(part.offset, part.packed)) return false;
        }
        if (record.encrypted) {
            std::vector<std::uint8_t> decrypted;
            if (!compression::decryptRar4Data(password_, record.salt.data(),
                    packed.data(), packed.size(), &decrypted))
                return false;
            packed.swap(decrypted);
        }
        if (record.method == 0x30) {
            if (record.unpacked > packed.size()) return false;
            record.decoded.assign(packed.begin(), packed.begin() +
                std::size_t(record.unpacked));
            if (crc32(record.decoded.data(), record.decoded.size()) != record.crc) {
                record.decoded.clear();
                return false;
            }
            record.valid = true;
            return true;
        }
        if (!rar4::decodeLz(packed.data(), packed.size(), std::size_t(record.unpacked),
                            record.solid, record.dictionarySize, &lzState_, &record.decoded))
            return false;
        if (crc32(record.decoded.data(), record.decoded.size()) != record.crc) {
            record.decoded.clear();
            return false;
        }
        record.valid = true;
        return true;
    }

    fs::ByteStorePtr source_;
    QString password_;
    mutable std::mutex mutex_;
    mutable std::vector<Rar4MemberRecord> records_;
    mutable std::size_t next_ = 0;
    mutable bool chainUsable_ = true;
    mutable rar4::LzState lzState_;
};

class Rar4MemberStore final : public fs::IByteStore {
public:
    Rar4MemberStore(std::shared_ptr<Rar4ArchiveDecode> archive,
                    std::size_t index, std::uint64_t unpacked,
                    bool cheapRandomAccess)
        : archive_(std::move(archive)), index_(index), unpacked_(unpacked),
          cheapRandomAccess_(cheapRandomAccess) {}

    std::int64_t capacity() const override
    {
        return unpacked_ <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
            ? std::int64_t(unpacked_) : 0;
    }

    bool cheapRandomAccess() const override { return cheapRandomAccess_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        return archive_ ? archive_->read(index_, pos, dst, count) : 0;
    }

    bool validate() const override
    {
        return archive_ && archive_->validate(index_);
    }

private:
    std::shared_ptr<Rar4ArchiveDecode> archive_;
    std::size_t index_;
    std::uint64_t unpacked_;
    bool cheapRandomAccess_;
};

} // namespace

ModulePtr Rar4Module::open(const QString& filePath, const QString& password)
{
    const QStringList volumePaths = rar4VolumePaths(filePath);
    if (volumePaths.size() > 1) {
        auto source = std::make_shared<Rar4JoinedStore>(volumePaths);
        if (!source->validate()) {
            auto module = peare::makeUnique<Rar4Module>();
            module->info_.filePath = filePath;
            module->info_.format = ModuleFormat::RAR4;
            module->info_.description = QStringLiteral(
                "RAR4 archive; stored, Huffman/LZ, PPMd, and filter methods");
            module->info_.error = QStringLiteral("One or more RAR4 volumes are unavailable");
            return module;
        }
        return open(source, filePath, password);
    }
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<Rar4Module>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::RAR4;
        module->info_.description = QStringLiteral(
            "RAR4 archive; stored, Huffman/LZ, PPMd, and filter methods");
        module->info_.error = holder->errorString();
        return module;
    }
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        fs::ByteStorePtr store = std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), size,
            std::static_pointer_cast<void>(holder));
        return open(store, filePath, password);
    }
    auto store = std::make_shared<fs::QFileByteStore>(filePath);
    if (!store->validate() || store->capacity() != size) {
        auto module = peare::makeUnique<Rar4Module>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::RAR4;
        module->info_.description = QStringLiteral(
            "RAR4 archive; stored, Huffman/LZ, PPMd, and filter methods");
        module->info_.error = QStringLiteral("RAR4 source became unavailable");
        return module;
    }
    return open(store, filePath, password);
}

ModulePtr Rar4Module::open(const fs::ByteStorePtr& source, const QString& sourceName,
                          const QString& password)
{
    auto module = peare::makeUnique<Rar4Module>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::RAR4;
    module->info_.description = QStringLiteral(
        "RAR4 archive; stored, Huffman/LZ, PPMd, and filter methods");
    if (!source) {
        module->info_.error = QStringLiteral("Null RAR4 source");
        return module;
    }
    if (source->capacity() < 0) {
        module->info_.error = QStringLiteral("Invalid RAR4 source capacity");
        return module;
    }
    std::int64_t marker = 0;
    if (!findMarker(*source, &marker)) {
        module->info_.error = QStringLiteral("RAR4 marker not found in bounded prefix");
        return module;
    }
    module->info_.headerOffset = std::uint64_t(marker);
    std::int64_t pos = marker + std::int64_t(sizeof(kMarker));
    std::uint64_t entryCount = 0;
    bool sawMain = false;
    bool sawEnd = false;
    bool encryptedHeaders = false;
    bool encryptedMembers = false;
    bool archiveSolid = false;
    bool hasPriorFileMember = false;
    std::size_t pendingSplitRecord = std::numeric_limits<std::size_t>::max();
    QString pendingSplitName;
    auto archiveDecode = std::make_shared<Rar4ArchiveDecode>(source, password);

    while (pos >= 0 && pos <= source->capacity() - 7) {
        std::uint8_t prefix[11] = {};
        if (!readExact(*source, pos, prefix, 7)) break;
        if (!encryptedHeaders && std::memcmp(prefix, kMarker, sizeof(kMarker)) == 0) {
            pos += std::int64_t(sizeof(kMarker));
            continue;
        }
        QByteArray header;
        std::uint64_t physicalHeaderSize = 0;
        std::uint16_t headerSize = 0;
        if (encryptedHeaders) {
            std::uint8_t salt[8];
            std::uint8_t firstCipherBlock[16];
            if (!readExact(*source, pos, salt, sizeof(salt)) ||
                !readExact(*source, pos + 8, firstCipherBlock, sizeof(firstCipherBlock))) {
                module->info_.error = QStringLiteral("Truncated encrypted RAR4 header");
                return module;
            }
            std::vector<std::uint8_t> firstPlain;
            if (!compression::decryptRar4Data(password, salt, firstCipherBlock,
                    sizeof(firstCipherBlock), &firstPlain)) {
                module->info_.error = QStringLiteral("RAR4 header decryption failed");
                return module;
            }
            headerSize = le16(firstPlain.data() + 5);
            if (headerSize < 7 || headerSize > 1024 * 1024) {
                module->info_.openError = ModuleOpenError::PasswordInvalidOrCorrupt;
                module->info_.error = QStringLiteral("Invalid decrypted RAR4 header size");
                return module;
            }
            const std::size_t cipherSize =
                (std::size_t(headerSize) + 15U) & ~std::size_t(15U);
            if (std::uint64_t(pos) + 8 + cipherSize > std::uint64_t(source->capacity())) {
                module->info_.error = QStringLiteral("Truncated encrypted RAR4 header body");
                return module;
            }
            std::vector<std::uint8_t> ciphertext(cipherSize);
            if (!readExact(*source, pos + 8, ciphertext.data(), int(ciphertext.size()))) {
                module->info_.error = QStringLiteral("Truncated encrypted RAR4 header body");
                return module;
            }
            std::vector<std::uint8_t> plaintext;
            if (!compression::decryptRar4Data(password, salt, ciphertext.data(),
                    ciphertext.size(), &plaintext)) {
                module->info_.error = QStringLiteral("RAR4 header decryption failed");
                return module;
            }
            header = QByteArray(reinterpret_cast<const char*>(plaintext.data()), headerSize);
            physicalHeaderSize = 8 + cipherSize;
        } else {
            headerSize = le16(prefix + 5);
            if (headerSize < 7 || std::uint64_t(pos) + headerSize >
                    std::uint64_t(source->capacity())) {
                module->info_.error = QStringLiteral("Invalid or truncated RAR4 block header");
                return module;
            }
            header.resize(headerSize);
            if (!readExact(*source, pos, reinterpret_cast<std::uint8_t*>(header.data()), header.size())) {
                module->info_.error = QStringLiteral("Truncated RAR4 block header");
                return module;
            }
            physicalHeaderSize = headerSize;
        }
        const auto* h = reinterpret_cast<const std::uint8_t*>(header.constData());
        const std::uint8_t type = h[2];
        const std::uint16_t flags = le16(h + 3);
        if (std::uint16_t(crc32(h + 2, header.size() - 2)) != le16(h)) {
            if (encryptedHeaders)
                module->info_.openError = ModuleOpenError::PasswordInvalidOrCorrupt;
            module->info_.error = QStringLiteral("RAR4 block header CRC mismatch");
            return module;
        }

        std::uint64_t addSize = 0;
        if (flags & 0x8000) {
            if (headerSize < 11) {
                module->info_.error = QStringLiteral("RAR4 add-size field is truncated");
                return module;
            }
            addSize = le32(h + 7);
        }
        if (!sawMain) {
            if (type != 0x73 || headerSize < 13) {
                module->info_.error = QStringLiteral("RAR4 main header is missing or malformed");
                return module;
            }
            sawMain = true;
            const std::uint16_t mainFlags = le16(h + 3);
            archiveSolid = (mainFlags & 0x0008) != 0;
            if (mainFlags & 0x0080) {
                encryptedHeaders = true;
                if (password.isNull()) {
                    module->info_.openError = ModuleOpenError::PasswordRequired;
                    module->info_.error = QStringLiteral("RAR4 encrypted headers require a password");
                    return module;
                }
            }
        }

        if (type == 0x74) {
            if (headerSize < 32 || !(flags & 0x8000)) {
                module->info_.error = QStringLiteral("Malformed RAR4 file header");
                return module;
            }
            const std::uint32_t packedLow = le32(h + 7);
            const std::uint32_t unpackedLow = le32(h + 11);
            const std::uint32_t expectedCrc = le32(h + 16);
            const std::uint8_t method = h[25];
            const std::uint16_t nameLength = le16(h + 26);
            const bool large = (flags & 0x0100) != 0;
            const std::size_t nameOffset = large ? 40 : 32;
            if (large && headerSize < 40) {
                module->info_.error = QStringLiteral("Truncated RAR4 large-file fields");
                return module;
            }
            if (std::size_t(headerSize) < nameOffset + nameLength) {
                module->info_.error = QStringLiteral("RAR4 filename exceeds its header");
                return module;
            }
            std::uint64_t packed = packedLow;
            std::uint64_t unpacked = unpackedLow;
            if (large) {
                packed |= std::uint64_t(le32(h + 32)) << 32;
                unpacked |= std::uint64_t(le32(h + 36)) << 32;
            }
            if (addSize != packedLow ||
                unpacked > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                packed > std::uint64_t(source->capacity()) ||
                std::uint64_t(pos) + physicalHeaderSize >
                    std::uint64_t(source->capacity()) - packed) {
                module->info_.error = QStringLiteral("RAR4 member payload exceeds the source bounds");
                return module;
            }
            const QByteArray rawName(reinterpret_cast<const char*>(h + nameOffset), nameLength);
            QString normalized = safeMemberName(rawName, (flags & 0x0200) != 0);
            if (normalized.isEmpty()) {
                module->info_.error = QStringLiteral("Invalid or unsafe RAR4 filename");
                return module;
            }
            const bool directory = (flags & 0x00e0) == 0x00e0;
            const unsigned dictionaryCode = (flags >> 5) & 7U;
            const std::size_t dictionarySize =
                std::size_t(64 * 1024) << std::min(dictionaryCode, 6U);
            const bool split = (flags & 0x0003) != 0;
            const bool splitBefore = (flags & 0x0001) != 0;
            const bool splitAfter = (flags & 0x0002) != 0;
            const bool encrypted = (flags & 0x0004) != 0;
            const bool hasSalt = (flags & 0x0400) != 0;
            const std::size_t saltOffset = nameOffset + nameLength;
            if (hasSalt && saltOffset + 8 > std::size_t(headerSize)) {
                module->info_.error = QStringLiteral("Truncated RAR4 encryption salt");
                return module;
            }
            const bool solid = (flags & 0x0010) != 0 ||
                (archiveSolid && hasPriorFileMember);
            encryptedMembers = encryptedMembers || encrypted;
            if (++entryCount > kMaxEntryCount) {
                module->info_.error = QStringLiteral("RAR4 entry limit exceeded");
                return module;
            }
            const QStringList parts = normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts);
            ResourceEntry entry;
            entry.type = directory ? QStringLiteral("RAR4_DIR") : QStringLiteral("RAR4_FILE");
            entry.name = parts.last();
            entry.hierarchyPath = parts;
            entry.hierarchyPath.removeLast();
            entry.dataOffset = std::uint64_t(pos) + physicalHeaderSize;
            entry.dataSize = unpacked;
            entry.isOs2 = h[15] == 1;
            entry.isEmbeddedFile = !directory;
            if (!directory) {
                if (splitBefore) {
                    if (pendingSplitRecord == std::numeric_limits<std::size_t>::max() ||
                        pendingSplitName != normalized ||
                        !archiveDecode->appendPart(pendingSplitRecord,
                            pos + headerSize, packed, expectedCrc, !splitAfter)) {
                        module->info_.error = QStringLiteral(
                            "RAR4 split member has a missing or mismatched volume part");
                        return module;
                    }
                    if (!splitAfter) {
                        pendingSplitRecord = std::numeric_limits<std::size_t>::max();
                        pendingSplitName.clear();
                    }
                    pos += std::int64_t(headerSize) + std::int64_t(packed);
                    continue;
                }
                if (pendingSplitRecord != std::numeric_limits<std::size_t>::max()) {
                    module->info_.error = QStringLiteral(
                        "RAR4 split member continuation is missing");
                    return module;
                }
                Rar4MemberRecord record;
                record.offset = pos + std::int64_t(physicalHeaderSize);
                record.packed = packed;
                record.unpacked = unpacked;
                record.crc = expectedCrc;
                record.method = method;
                record.dictionarySize = dictionarySize;
                record.solid = solid;
                record.split = splitAfter;
                record.encrypted = encrypted;
                record.hasSalt = hasSalt;
                if (hasSalt)
                    std::copy(h + saltOffset, h + saltOffset + 8, record.salt.begin());
                if (splitAfter) record.packed = 0;
                const std::size_t memberIndex = archiveDecode->add(std::move(record));
                if (splitAfter) {
                    if (!archiveDecode->appendPart(memberIndex,
                                                   pos + std::int64_t(physicalHeaderSize), packed,
                                                   expectedCrc, false)) {
                        module->info_.error = QStringLiteral("Invalid first RAR4 split part");
                        return module;
                    }
                    pendingSplitRecord = memberIndex;
                    pendingSplitName = normalized;
                }
                entry.content = std::make_shared<Rar4MemberStore>(archiveDecode,
                    memberIndex, unpacked, method == 0x30 && !solid && !split && !encrypted);
                hasPriorFileMember = true;
            }
            module->resources_.push_back(std::move(entry));
            pos += std::int64_t(physicalHeaderSize) + std::int64_t(packed);
            continue;
        }
        if (type == 0x7b) {
            if (flags & 0x0001) {
                pos += std::int64_t(physicalHeaderSize);
                continue;
            }
            sawEnd = true;
            break;
        }
        if (addSize > std::uint64_t(source->capacity()) ||
            std::uint64_t(pos) + physicalHeaderSize >
                std::uint64_t(source->capacity()) - addSize) {
            module->info_.error = QStringLiteral("RAR4 ancillary block exceeds the source bounds");
            return module;
        }
        pos += std::int64_t(physicalHeaderSize) + std::int64_t(addSize);
    }

    if (!sawMain) module->info_.error = QStringLiteral("RAR4 main header not found");
    else if (pendingSplitRecord != std::numeric_limits<std::size_t>::max())
        module->info_.error = QStringLiteral("RAR4 split member is missing a later volume");
    else if (!sawEnd && pos < source->capacity() && source->capacity() - pos < 7)
        module->info_.error = QStringLiteral("Truncated RAR4 block header at end of source");
    if (module->info_.error.isEmpty() && encryptedMembers) {
        if (password.isNull()) {
            module->info_.openError = ModuleOpenError::PasswordRequired;
            module->info_.error = QStringLiteral("RAR4 encrypted members require a password");
        }
    }
    return module;
}

} // namespace peare
