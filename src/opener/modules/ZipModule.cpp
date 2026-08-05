#include "ZipModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"

#include <QFile>
#include <QStringList>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace peare {
namespace {

const quint32 kZipLocalHeader = 0x04034b50u;
const quint32 kZipCentralHeader = 0x02014b50u;
const quint32 kZipEndCentralDirectory = 0x06054b50u;
const std::int64_t kMaxZipUncompressed = std::int64_t(512) * 1024 * 1024;
const std::int64_t kMaxZipVariableField = std::int64_t(16) * 1024 * 1024;

quint16 le16(const std::uint8_t* p) {
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::uint8_t* p) {
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool readExact(const fs::IByteStore& store, std::int64_t offset,
               std::uint8_t* dst, int count) {
    if (offset < 0 || count < 0 || offset > store.capacity() - count) return false;
    int done = 0;
    while (done < count) {
        const int n = store.read(offset + done, dst + done, count - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

QByteArray readArray(const fs::IByteStore& store, std::int64_t offset,
                     std::int64_t count) {
    if (count < 0 || count > std::numeric_limits<int>::max() ||
        offset < 0 || offset > store.capacity() - count)
        return QByteArray();
    QByteArray out(int(count), Qt::Uninitialized);
    if (count && !readExact(store, offset,
                            reinterpret_cast<std::uint8_t*>(out.data()), int(count)))
        return QByteArray();
    return out;
}

QString decodeZipName(const QByteArray& bytes, quint16 flags) {
    return (flags & 0x0800) ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

QString normalizeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    return name;
}

bool splitSafeName(QString name, QStringList* hierarchy, QString* leaf) {
    name = normalizeName(name);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
        for (const QChar c : part)
            if (c.unicode() < 0x20) return false;
    }
    *leaf = parts.last();
    *hierarchy = parts;
    hierarchy->removeLast();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf->size(); ++i)
        if (forbidden.contains(leaf->at(i))) (*leaf)[i] = QLatin1Char('_');
    while (leaf->endsWith(QLatin1Char(' ')) || leaf->endsWith(QLatin1Char('.')))
        leaf->chop(1);
    return !leaf->isEmpty();
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return fs::ByteStorePtr();
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

// Deflated members stay compressed while the ZIP is listed. Header probing
// inflates only the requested prefix; selecting/exporting the member inflates
// the complete payload. This avoids the old O(total uncompressed size) archive
// open cost and keeps nested-container detection cheap.

// ZIP method 1 ("Shrink") is decoded only when the member is actually read.
// Ordinary stored/deflated members keep their existing zero-copy/prefix paths.
class ZipShrinkStore final : public fs::IByteStore {
public:
    ZipShrinkStore(fs::ByteStorePtr source, std::int64_t offset,
                   std::int64_t compressed, std::int64_t uncompressed)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDecoded()) return 0;
        const int n = int(std::min<std::int64_t>(count, uncompressed_ - pos));
        std::copy(decoded_.begin() + std::ptrdiff_t(pos),
                  decoded_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    class BitReader {
    public:
        explicit BitReader(const QByteArray& data) : data_(data) {}
        bool read(int bits, quint16* value) {
            while (available_ < bits) {
                if (position_ >= data_.size()) return false;
                buffer_ |= quint64(uchar(data_.at(position_++))) << available_;
                available_ += 8;
            }
            *value = quint16(buffer_ & ((quint64(1) << bits) - 1));
            buffer_ >>= bits;
            available_ -= bits;
            return true;
        }
    private:
        const QByteArray& data_;
        qsizetype position_ = 0;
        quint64 buffer_ = 0;
        int available_ = 0;
    };

    bool ensureDecoded() const {
        if (decodedReady_) return true;
        if (failed_ || compressed_ < 0 || uncompressed_ < 0 ||
            compressed_ > std::numeric_limits<int>::max() ||
            uncompressed_ > std::numeric_limits<int>::max()) {
            failed_ = true;
            return false;
        }
        const QByteArray input = readArray(*source_, offset_, compressed_);
        if (input.size() != compressed_) { failed_ = true; return false; }

        const int kTableSize = 8192;
        const quint16 kBogusCode = 256;
        const quint16 kCodeMask = 8191;
        const quint16 kFreeCode = 8192;
        const quint16 kHasChild = 16384;
        std::vector<quint16> parent(kTableSize, kFreeCode);
        std::vector<std::uint8_t> value(kTableSize, 0);
        std::vector<std::uint8_t> stack;
        stack.reserve(kTableSize);
        for (int code = 0; code < 256; ++code) {
            value[code] = std::uint8_t(code);
            parent[code] = kBogusCode;
        }

        BitReader reader(input);
        int codeSize = 9;
        quint16 oldCode = 0;
        if (!reader.read(codeSize, &oldCode) || oldCode >= 256) {
            failed_ = true;
            return false;
        }
        decoded_.clear();
        decoded_.reserve(std::size_t(uncompressed_));
        decoded_.push_back(std::uint8_t(oldCode));
        std::uint8_t finalValue = std::uint8_t(oldCode);
        int lastFreeCode = kBogusCode;

        while (decoded_.size() < std::size_t(uncompressed_)) {
            quint16 code = 0;
            if (!reader.read(codeSize, &code)) { failed_ = true; decoded_.clear(); return false; }
            if (code == kBogusCode) {
                quint16 command = 0;
                if (!reader.read(codeSize, &command)) { failed_ = true; decoded_.clear(); return false; }
                if (command == 1) {
                    if (++codeSize > 13) { failed_ = true; decoded_.clear(); return false; }
                } else if (command == 2) {
                    for (int i = kBogusCode + 1; i <= lastFreeCode; ++i) {
                        const int p = parent[i] & kCodeMask;
                        if (parent[i] != kFreeCode && p > kBogusCode)
                            parent[p] = quint16(parent[p] | kHasChild);
                    }
                    for (int i = kBogusCode + 1; i <= lastFreeCode; ++i) {
                        if (parent[i] & kHasChild)
                            parent[i] = quint16(parent[i] & ~kHasChild);
                        else
                            parent[i] = kFreeCode;
                    }
                    lastFreeCode = kBogusCode;
                } else {
                    failed_ = true; decoded_.clear(); return false;
                }
                continue;
            }
            if (code >= kTableSize) { failed_ = true; decoded_.clear(); return false; }

            const quint16 currentCode = code;
            stack.clear();
            if (parent[code] == kFreeCode) {
                stack.push_back(finalValue);
                code = oldCode;
            }
            int guard = 0;
            while (code != kBogusCode) {
                if (code >= kTableSize || ++guard > kTableSize) {
                    failed_ = true; decoded_.clear(); return false;
                }
                if (parent[code] == kFreeCode) {
                    stack.push_back(finalValue);
                    code = oldCode;
                } else {
                    stack.push_back(value[code]);
                    code = quint16(parent[code] & kCodeMask);
                }
            }
            if (stack.empty() || decoded_.size() + stack.size() > std::size_t(uncompressed_)) {
                failed_ = true; decoded_.clear(); return false;
            }
            finalValue = stack.back();
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                decoded_.push_back(*it);

            int freeCode = lastFreeCode + 1;
            while (freeCode < kTableSize && parent[freeCode] != kFreeCode) ++freeCode;
            if (freeCode >= kTableSize) { failed_ = true; decoded_.clear(); return false; }
            lastFreeCode = freeCode;
            value[freeCode] = finalValue;
            parent[freeCode] = oldCode;
            oldCode = currentCode;
        }

        if (decoded_.size() != std::size_t(uncompressed_)) {
            failed_ = true; decoded_.clear(); return false;
        }
        decodedReady_ = true;
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool decodedReady_ = false;
    mutable bool failed_ = false;
};

class ZipCompressedInput final : public compression::InflateInput {
public:
    ZipCompressedInput(const fs::IByteStore& source, std::int64_t offset,
                       std::int64_t length)
        : source_(source), offset_(offset), length_(length), consumed_(0),
          bufferPosition_(0), bufferSize_(0) {}

    bool readByte(std::uint8_t* value) override {
        if (!value || consumed_ >= length_) return false;
        if (bufferPosition_ >= bufferSize_) {
            const std::int64_t remaining = length_ - consumed_;
            const int request = int(std::min<std::int64_t>(remaining, sizeof(buffer_)));
            const int got = source_.read(offset_ + consumed_, buffer_, request);
            if (got <= 0) return false;
            bufferPosition_ = 0;
            bufferSize_ = got;
        }
        *value = buffer_[bufferPosition_++];
        ++consumed_;
        return true;
    }

    std::size_t consumed() const override {
        return consumed_ < 0 ? 0 : static_cast<std::size_t>(consumed_);
    }

private:
    const fs::IByteStore& source_;
    std::int64_t offset_;
    std::int64_t length_;
    std::int64_t consumed_;
    std::uint8_t buffer_[64 * 1024];
    int bufferPosition_;
    int bufferSize_;
};

class ZipDeflateStore final : public fs::IByteStore {
public:
    ZipDeflateStore(fs::ByteStorePtr source, std::int64_t offset,
                    std::int64_t compressed, std::int64_t uncompressed)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        const std::int64_t end = std::min<std::int64_t>(uncompressed_, pos + count);
        if (end > std::numeric_limits<int>::max()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensurePrefix(std::size_t(end))) return 0;
        const int n = int(end - pos);
        std::copy(prefix_.begin() + std::ptrdiff_t(pos),
                  prefix_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    bool ensurePrefix(std::size_t wanted) const {
        if (failed_) return false;
        if (prefix_.size() >= wanted) return true;
        if (wanted > std::size_t(uncompressed_)) wanted = std::size_t(uncompressed_);
        if (compressed_ < 0 || offset_ < 0 ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }

        ZipCompressedInput input(*source_, offset_, compressed_);
        std::vector<std::uint8_t> output;
        std::string error;
        const bool ok = wanted == std::size_t(uncompressed_)
            ? compression::inflateRawExact(input, wanted, nullptr, &output, &error)
            : compression::inflateRawPrefix(input, wanted, &output, &error);
        if (!ok) {
            failed_ = true;
            prefix_.clear();
            return false;
        }
        prefix_.swap(output);
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> prefix_;
    mutable bool failed_ = false;
};

std::int64_t findEocd(const fs::IByteStore& store) {
    if (store.capacity() < 22) return -1;
    const std::int64_t tailSize = std::min<std::int64_t>(store.capacity(), 65535 + 22);
    const std::int64_t tailOffset = store.capacity() - tailSize;
    const QByteArray tail = readArray(store, tailOffset, tailSize);
    if (tail.size() != tailSize) return -1;
    for (int pos = tail.size() - 22; pos >= 0; --pos) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(tail.constData() + pos);
        if (le32(p) != kZipEndCentralDirectory) continue;
        const quint16 comment = le16(p + 20);
        if (pos + 22 + comment == tail.size()) return tailOffset + pos;
    }
    return -1;
}

}  // namespace

std::unique_ptr<ZipModule> ZipModule::open(const QString& filePath) {
    ModulePtr base = open(storeForFile(filePath), filePath);
    return std::unique_ptr<ZipModule>(static_cast<ZipModule*>(base.release()));
}

std::unique_ptr<ZipModule> ZipModule::open(const QByteArray& data, const QString& logicalName) {
    ModulePtr base = open(std::make_shared<fs::MemoryStore>(
                              reinterpret_cast<const std::uint8_t*>(data.constData()),
                              std::size_t(data.size())),
                          logicalName);
    return std::unique_ptr<ZipModule>(static_cast<ZipModule*>(base.release()));
}

ModulePtr ZipModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<ZipModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ZIP;
    module->info_.description = QStringLiteral("ZIP archive");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open ZIP archive");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    const std::int64_t eocd = findEocd(*file);
    std::uint8_t end[22];
    if (eocd < 0 || !readExact(*file, eocd, end, sizeof(end))) {
        module->info_.error = QStringLiteral("ZIP end of central directory not found");
        return ModulePtr(std::move(module));
    }
    const quint16 disk = le16(end + 4);
    const quint16 centralDisk = le16(end + 6);
    const quint16 entriesOnDisk = le16(end + 8);
    const quint16 entries = le16(end + 10);
    const quint32 cdSize = le32(end + 12);
    const quint32 cdOffset = le32(end + 16);
    if (disk || centralDisk || entriesOnDisk != entries) {
        module->info_.error = QStringLiteral("Multi-disk ZIP archives are not supported");
        return ModulePtr(std::move(module));
    }
    if (cdOffset == 0xffffffffu || cdSize == 0xffffffffu ||
        std::int64_t(cdOffset) > file->capacity() - std::int64_t(cdSize)) {
        module->info_.error = QStringLiteral("ZIP64 archives are not supported yet");
        return ModulePtr(std::move(module));
    }

    std::int64_t pos = cdOffset;
    for (quint16 i = 0; i < entries; ++i) {
        std::uint8_t central[46];
        if (!readExact(*file, pos, central, sizeof(central)) ||
            le32(central) != kZipCentralHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP central directory");
            return ModulePtr(std::move(module));
        }
        const quint16 flags = le16(central + 8);
        const quint16 method = le16(central + 10);
        const quint32 compressedSize = le32(central + 20);
        const quint32 uncompressedSize = le32(central + 24);
        const quint16 nameLen = le16(central + 28);
        const quint16 extraLen = le16(central + 30);
        const quint16 commentLen = le16(central + 32);
        const quint32 localOffset = le32(central + 42);
        const std::int64_t variable = std::int64_t(nameLen) + extraLen + commentLen;
        if (variable > kMaxZipVariableField || pos > file->capacity() - 46 - variable) {
            module->info_.error = QStringLiteral("Truncated ZIP central directory");
            return ModulePtr(std::move(module));
        }
        const QByteArray rawName = readArray(*file, pos + 46, nameLen);
        const QString fullName = normalizeName(decodeZipName(rawName, flags));
        pos += 46 + variable;
        if (fullName.isEmpty() || fullName.endsWith(QLatin1Char('/'))) continue;
        if (flags & 0x0001) {
            module->info_.error = QStringLiteral("Encrypted ZIP entries are not supported");
            return ModulePtr(std::move(module));
        }
        if (localOffset == 0xffffffffu || compressedSize == 0xffffffffu ||
            uncompressedSize == 0xffffffffu || uncompressedSize > kMaxZipUncompressed) {
            module->info_.error = QStringLiteral("ZIP64 or oversized entries are not supported yet");
            return ModulePtr(std::move(module));
        }

        std::uint8_t local[30];
        if (!readExact(*file, localOffset, local, sizeof(local)) || le32(local) != kZipLocalHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP local header");
            return ModulePtr(std::move(module));
        }
        const quint16 localNameLen = le16(local + 26);
        const quint16 localExtraLen = le16(local + 28);
        const std::int64_t dataOffset = std::int64_t(localOffset) + 30 +
                                        localNameLen + localExtraLen;
        if (dataOffset < 0 || dataOffset > file->capacity() - std::int64_t(compressedSize)) {
            module->info_.error = QStringLiteral("Truncated ZIP file data");
            return ModulePtr(std::move(module));
        }

        fs::ByteStorePtr content;
        if (method == 0) {
            if (compressedSize != uncompressedSize) {
                module->info_.error = QStringLiteral("Invalid stored ZIP entry size");
                return ModulePtr(std::move(module));
            }
            content = std::make_shared<fs::SubStore>(file, dataOffset, uncompressedSize);
        } else if (method == 1) {
            content = std::make_shared<ZipShrinkStore>(file, dataOffset,
                                                       compressedSize, uncompressedSize);
        } else if (method == 8) {
            content = std::make_shared<ZipDeflateStore>(file, dataOffset,
                                                        compressedSize, uncompressedSize);
        } else {
            module->info_.error = QStringLiteral("Unsupported ZIP compression method %1").arg(method);
            return ModulePtr(std::move(module));
        }

        QStringList hierarchy;
        QString leaf;
        if (!splitSafeName(fullName, &hierarchy, &leaf)) continue;
        ResourceEntry entry;
        entry.type = QStringLiteral("ZIP_FILE");
        entry.isEmbeddedFile = true;
        entry.name = leaf;
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(dataOffset);
        entry.dataSize = quint64(uncompressedSize);
        entry.format = ModuleFormat::ZIP;
        entry.hierarchyPath = hierarchy;
        entry.content = content;
        module->resources_.push_back(std::move(entry));
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
