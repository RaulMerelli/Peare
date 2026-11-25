#include "TibFileReader.h"

#include "../../common/compression/DeflateDecoder.h"
#include "../../common/compression/AesInitialization.h"
#include "../compression/Crc32.h"
#include "../compression/third_party/lzma/Aes.h"

#include <QCryptographicHash>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

std::uint16_t u16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}
std::uint32_t u32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t u64(const std::uint8_t* p) {
    return std::uint64_t(u32(p)) | (std::uint64_t(u32(p + 4)) << 32);
}

class Cursor {
public:
    explicit Cursor(const std::vector<std::uint8_t>& data) : data_(data), pos_(0) {}
    bool take(std::size_t size, const std::uint8_t** out) {
        if (size > data_.size() - pos_) return false;
        *out = data_.data() + pos_;
        pos_ += size;
        return true;
    }
    bool empty() const { return pos_ == data_.size(); }
private:
    const std::vector<std::uint8_t>& data_;
    std::size_t pos_;
};

bool readExact(const ByteStorePtr& source, std::int64_t pos, std::uint8_t* data, int count) {
    if (!source || pos < 0 || count < 0 || pos > source->capacity() ||
        count > source->capacity() - pos) return false;
    int done = 0;
    while (done < count) {
        int n = source->read(pos + done, data + done, count - done);
        if (n <= 0 || n > count - done) return false;
        done += n;
    }
    return true;
}

class Input final : public compression::InflateInput {
public:
    Input(ByteStorePtr source, std::int64_t start) : source_(std::move(source)),
        start_(start), pos_(0), bufferStart_(-1), bufferSize_(0), failed_(false) {}
    bool readByte(std::uint8_t* out) override {
        if (!out || start_ < 0 || start_ > source_->capacity() ||
            pos_ >= source_->capacity() - start_) return false;
        const std::int64_t offset = start_ + pos_;
        if (bufferStart_ < 0 || offset < bufferStart_ || offset - bufferStart_ >= bufferSize_) {
            bufferStart_ = offset;
            const int requested = static_cast<int>(std::min<std::int64_t>(buffer_.size(), source_->capacity() - offset));
            bufferSize_ = source_->read(offset, buffer_.data(), requested);
            if (bufferSize_ <= 0 || bufferSize_ > requested) {
                failed_ = true;
                bufferSize_ = 0;
                return false;
            }
        }
        *out = buffer_[static_cast<std::size_t>(offset - bufferStart_)];
        ++pos_;
        return true;
    }
    std::size_t consumed() const override { return static_cast<std::size_t>(pos_); }
    bool rewind(std::size_t count) override {
        if (count > static_cast<std::size_t>(pos_)) return false;
        pos_ -= static_cast<std::int64_t>(count);
        return true;
    }
    bool failed() const { return failed_; }
private:
    ByteStorePtr source_;
    std::int64_t start_, pos_, bufferStart_;
    int bufferSize_;
    bool failed_;
    std::array<std::uint8_t, 4096> buffer_;
};

std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t c : data) { a = (a + c) % 65521U; b = (b + a) % 65521U; }
    return (b << 16) | a;
}

struct DedupRecord {
    ByteStorePtr source;
    std::int64_t offset;
    std::uint64_t rawSize;
    std::uint64_t storedSize;
    std::uint64_t encryptedPlainSize;
    bool compressed;
};

class DedupProvider final : public TibDedupProvider {
public:
    explicit DedupProvider(const std::vector<std::uint8_t>& key) : key_(key) {}
    ~DedupProvider() override { std::fill(key_.begin(), key_.end(), 0); }
    bool contains(const std::array<std::uint8_t, 16>& hash) const override {
        return records_.find(hash) != records_.end();
    }
    bool add(const ByteStorePtr& source, std::int64_t used, std::string* error) {
        if (!source || !source->validate() || used < 4 || used > source->capacity())
            return fail(error, "Invalid TIB deduplication store size");
        std::int64_t end = used;
        while (end > 4) {
            const int footerSize = key_.empty() ? 33 : 41;
            std::uint8_t footer[41];
            if (end - 4 < footerSize ||
                !readExact(source, end - footerSize, footer, footerSize))
                return fail(error, "Truncated TIB deduplication footer");
            const std::uint64_t rawSize = u64(footer + 17);
            const std::uint64_t storedSize = u64(footer + 25);
            if (footer[16] > 1 || storedSize > std::uint64_t(end - 4 - footerSize) ||
                rawSize > std::uint64_t(std::numeric_limits<int>::max()) ||
                storedSize > std::uint64_t(std::numeric_limits<int>::max()))
                return fail(error, "Invalid TIB deduplication record");
            const std::int64_t offset = end - footerSize - static_cast<std::int64_t>(storedSize);
            std::array<std::uint8_t, 16> hash;
            std::copy(footer, footer + hash.size(), hash.begin());
            DedupRecord rec = {source, offset, rawSize, storedSize,
                               key_.empty() ? 0 : u64(footer + 33), footer[16] != 0};
            records_[hash].push_back(rec);
            end = offset;
        }
        return end == 4 || fail(error, "Invalid TIB deduplication store boundary");
    }

    bool read(const std::array<std::uint8_t, 16>& hash,
              std::vector<std::uint8_t>* data) const override {
        const auto found = records_.find(hash);
        if (!data || found == records_.end()) return false;
        std::vector<std::uint8_t> first;
        for (const DedupRecord& rec : found->second) {
            std::vector<std::uint8_t> decoded;
            if (!rec.source->validate()) return false;
            ByteStorePtr source = rec.source;
            std::int64_t offset = rec.offset;
            std::uint64_t storedSize = rec.storedSize;
            if (!key_.empty()) {
                if (storedSize < 52 || (storedSize - 52) % 16) return false;
                std::vector<std::uint8_t> encrypted(static_cast<std::size_t>(storedSize));
                if (!readExact(source, offset, encrypted.data(), static_cast<int>(storedSize))) return false;
                const std::uint8_t* trailer = encrypted.data() + encrypted.size() - 36;
                const std::uint64_t paddedSize = storedSize - 52;
                if (u32(trailer + 4) != 16 || rec.encryptedPlainSize > paddedSize ||
                    paddedSize - rec.encryptedPlainSize != u32(trailer) ||
                    u32(trailer) > 15 || u32(trailer + 32) != 0xf742ebc6U ||
                    u64(trailer + 8) != rec.encryptedPlainSize || u64(trailer + 16) != 0 ||
                    u32(trailer + 24) != 1 || u32(trailer + 28) != 1) return false;
                compression::initializeAesTables();
                alignas(16) UInt32 aes[AES_NUM_IVMRK_WORDS] = {};
                Aes_SetKey_Dec(aes + 4, key_.data(), static_cast<unsigned>(key_.size()));
                AesCbc_Init(aes, encrypted.data());
                AesCbc_Decode(aes, encrypted.data() + 16, static_cast<std::size_t>((storedSize - 52) / 16));
                std::memset(aes, 0, sizeof(aes));
                std::vector<std::uint8_t> clear(encrypted.begin() + 16,
                    encrypted.begin() + 16 + static_cast<std::size_t>(rec.encryptedPlainSize));
                source = std::make_shared<MemoryStore>(std::move(clear));
                offset = 0;
                storedSize = rec.encryptedPlainSize;
            }
            if (!rec.compressed) {
                if (rec.rawSize != storedSize) return false;
                decoded.resize(static_cast<std::size_t>(rec.rawSize));
                if (!readExact(source, offset, decoded.data(), static_cast<int>(storedSize))) return false;
            } else {
                if (storedSize < 6) return false;
                ByteStorePtr bounded = std::make_shared<SubStore>(source, offset,
                    static_cast<std::int64_t>(storedSize));
                Input in(bounded, 0);
                std::uint8_t header[2], checksum[4];
                if (!in.readByte(&header[0]) || !in.readByte(&header[1]) ||
                    (header[0] & 15) != 8 || (header[0] >> 4) > 7 ||
                    ((unsigned(header[0]) << 8) + header[1]) % 31 || (header[1] & 32) ||
                    !compression::inflateRawStream(in, static_cast<std::size_t>(rec.rawSize), &decoded, nullptr))
                    return false;
                for (int i = 0; i < 4; ++i) if (!in.readByte(&checksum[i])) return false;
                const std::uint32_t expected = (std::uint32_t(checksum[0]) << 24) |
                    (std::uint32_t(checksum[1]) << 16) | (std::uint32_t(checksum[2]) << 8) | checksum[3];
                if (in.failed() || in.consumed() != storedSize || adler32(decoded) != expected) return false;
            }
            if (decoded.size() != rec.rawSize) return false;
            const QByteArray digest = QCryptographicHash::hash(
                QByteArray::fromRawData(reinterpret_cast<const char*>(decoded.data()),
                                        static_cast<int>(decoded.size())), QCryptographicHash::Md5);
            if (digest.size() != 16 || std::memcmp(digest.constData(), hash.data(), hash.size()) != 0)
                return false;
            if (!first.empty() && first != decoded) return false;
            first.swap(decoded);
        }
        data->swap(first);
        return true;
    }
private:
    std::vector<std::uint8_t> key_;
    std::map<std::array<std::uint8_t, 16>, std::vector<DedupRecord> > records_;
};

bool record(const ByteStorePtr& source, std::int64_t offset, bool zlib,
            std::size_t maximum, std::vector<std::uint8_t>* output,
            std::int64_t* next, std::string* error) {
    if (offset < 0 || offset >= source->capacity() ||
        std::uint64_t(source->capacity() - offset) > std::numeric_limits<std::size_t>::max())
        return fail(error, "Invalid TIB record position");
    Input in(source, offset + 1);
    std::uint8_t h[2];
    if (zlib) {
        if (!in.readByte(&h[0]) || !in.readByte(&h[1]) ||
            (h[0] & 15) != 8 || (h[0] >> 4) > 7 ||
            ((unsigned(h[0]) << 8) + h[1]) % 31 || (h[1] & 32))
            return fail(error, "Unsupported TIB zlib header");
    }
    if (!compression::inflateRawStream(in, maximum, output, error) || in.failed())
        return fail(error, "Invalid TIB compressed record");
    std::uint8_t check[4];
    for (int i = 0; i < 4; ++i)
        if (!in.readByte(&check[i])) return fail(error, "Truncated TIB record checksum");
    const std::uint32_t expected = zlib
        ? (std::uint32_t(check[0]) << 24) | (std::uint32_t(check[1]) << 16) |
          (std::uint32_t(check[2]) << 8) | std::uint32_t(check[3])
        : u32(check);
    if ((zlib ? adler32(*output) : compression::crc32(output->data(), output->size())) != expected)
        return fail(error, "TIB record checksum mismatch");
    *next = offset + 1 + static_cast<std::int64_t>(in.consumed());
    return true;
}

struct Block {
    std::uint64_t logical, physical;
    std::array<std::uint8_t, 16> hash;
};

class FileStore final : public IByteStore {
public:
    FileStore(ByteStorePtr source, std::int64_t size, std::vector<Block> blocks,
              std::shared_ptr<TibDedupProvider> provider = std::shared_ptr<TibDedupProvider>())
        : source_(std::move(source)), size_(size), blocks_(std::move(blocks)), provider_(std::move(provider)) {}
    std::int64_t capacity() const override { return size_; }
    bool cheapRandomAccess() const override { return blocks_.empty(); }
    bool validate() const override { return source_ && source_->validate(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate()) return -1;
        if (!dst || pos < 0 || count <= 0 || pos >= size_) return 0;
        try {
            const int amount = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
            std::vector<std::uint8_t> result(static_cast<std::size_t>(amount), 0);
            const std::uint64_t end = std::uint64_t(pos) + amount;
            const auto first = std::upper_bound(blocks_.begin(), blocks_.end(), std::uint64_t(pos),
                [](std::uint64_t offset, const Block& block) { return offset < block.logical; });
            const std::size_t start = first == blocks_.begin() ? 0 :
                static_cast<std::size_t>((first - blocks_.begin()) - 1);
            for (std::size_t i = start; i < blocks_.size(); ++i) {
                const std::uint64_t begin = blocks_[i].logical;
                const std::uint64_t limit = i + 1 < blocks_.size() ? blocks_[i + 1].logical : std::uint64_t(size_);
                if (begin >= end) break;
                if (limit <= std::uint64_t(pos)) continue;
                std::shared_ptr<const std::vector<std::uint8_t> > data;
                {
                    std::lock_guard<std::mutex> lock(cacheMutex_);
                    if (cachedBlock_ && cachedIndex_ == i) {
                        data = cachedBlock_;
                    } else {
                        auto decoded = std::make_shared<std::vector<std::uint8_t> >();
                        const std::uint64_t maximum = limit - begin;
                        if (provider_) {
                            if (!provider_->read(blocks_[i].hash, decoded.get()) || decoded->size() != maximum)
                                return -1;
                        } else {
                            std::uint8_t tag = 0;
                            if (!readExact(source_, static_cast<std::int64_t>(blocks_[i].physical), &tag, 1) || tag != 'm') return -1;
                            std::int64_t next = 0;
                            if (maximum > decoded->max_size() ||
                                !record(source_, static_cast<std::int64_t>(blocks_[i].physical), true,
                                        static_cast<std::size_t>(maximum), decoded.get(), &next, nullptr)) return -1;
                        }
                        data = decoded;
                        if (decoded->capacity() <= 32U * 1024U * 1024U) {
                            cachedBlock_ = data;
                            cachedIndex_ = i;
                        } else {
                            cachedBlock_.reset();
                        }
                    }
                }
                const std::uint64_t from = std::max<std::uint64_t>(begin, pos);
                const std::uint64_t to = std::min<std::uint64_t>(end, begin + data->size());
                if (to > from) std::memcpy(result.data() + (from - pos), data->data() + (from - begin), std::size_t(to - from));
            }
            std::memcpy(dst, result.data(), result.size());
            return amount;
        } catch (const std::exception&) { return -1; }
    }
private:
    ByteStorePtr source_;
    std::int64_t size_;
    std::vector<Block> blocks_;
    std::shared_ptr<TibDedupProvider> provider_;
    mutable std::mutex cacheMutex_;
    mutable std::size_t cachedIndex_ = 0;
    mutable std::shared_ptr<const std::vector<std::uint8_t> > cachedBlock_;
};

} // namespace

std::shared_ptr<TibDedupProvider> openTibDedupProvider(
    const std::vector<std::pair<ByteStorePtr, std::int64_t> >& stores, std::string* error,
    const std::vector<std::uint8_t>& key) {
    if (stores.empty()) {
        fail(error, "Missing TIB deduplication store");
        return std::shared_ptr<TibDedupProvider>();
    }
    if (!key.empty() && key.size() != 16 && key.size() != 24 && key.size() != 32) {
        fail(error, "Invalid TIB deduplication key");
        return std::shared_ptr<TibDedupProvider>();
    }
    try {
        std::shared_ptr<DedupProvider> provider = std::make_shared<DedupProvider>(key);
        for (const auto& store : stores) if (!provider->add(store.first, store.second, error))
            return std::shared_ptr<TibDedupProvider>();
        if (error) error->clear();
        return provider;
    } catch (const std::exception&) {
        fail(error, "TIB deduplication index allocation failed");
        return std::shared_ptr<TibDedupProvider>();
    }
}

bool parseTibFileItem(const TibFileIndexEntry& entry, const TibFileMetadataVersion& metadataVersion,
                      TibFileItem* item, std::string* error) {
    if (!item) return fail(error, "Missing TIB item output");
    if (!supportsTibFileMetadataVersion(metadataVersion))
        return fail(error, "Unsupported TIB metadata version");
    Cursor c(entry.metadata);
    const std::uint8_t* p = nullptr;
    for (int i = 0; i < 2; ++i) {
        if (!c.take(4, &p)) return fail(error, "Truncated TIB item name");
        const std::uint32_t units = u32(p);
        if (std::uint64_t(units) * 2 > std::numeric_limits<std::size_t>::max() ||
            !c.take(static_cast<std::size_t>(units) * 2, &p))
            return fail(error, "Invalid TIB item name");
    }
    if (!c.take(56, &p)) return fail(error, "Truncated TIB item metadata");
    const std::uint32_t type = u32(p + 36);
    if (type != 0 && type != 0x80 && type != 0x83 && type != 0x8e)
        return fail(error, "Unsupported TIB item type");
    const std::uint64_t logicalSize = u64(p + 12);
    const std::uint64_t itemOffset = u64(p + 40);
    const std::uint32_t state = u32(p + 48);
    if (!c.take(4, &p)) return fail(error, "Truncated TIB metadata string length");
    const std::uint32_t units = u32(p);
    if (std::uint64_t(units) * 2 > std::numeric_limits<std::size_t>::max() ||
        !c.take(static_cast<std::size_t>(units) * 2, &p))
        return fail(error, "Invalid TIB metadata string length");
    if (!c.take(4, &p) || !c.take(4, &p))
        return fail(error, "Truncated TIB metadata field");
    const std::uint32_t byteLength = u32(p);
    if (!c.take(byteLength, &p) || !c.take(2, &p))
        return fail(error, "Truncated TIB provider metadata");
    TibFileItem result;
    result.path = entry.path;
    result.directory = type == 0x80 || type == 0x83 || type == 0x8e;
    if (type == 0x8e && logicalSize != 0)
        return fail(error, "Invalid TIB SMB share directory size");
    // Incremental indexes retain both newly and previously deleted entries.
    result.deleted = state == 3 || state == 4;
    result.itemOffset = itemOffset;
    result.logicalSize = logicalSize;
    result.providerRequired = p[0] != 0;
    *item = std::move(result);
    return true;
}

bool openTibFileStreams(const ByteStorePtr& source, std::uint64_t dataOffset,
                        std::uint64_t itemOffset, std::uint64_t logicalSize,
                        const std::vector<std::uint64_t>& itemOffsets,
                        std::vector<TibFileStream>* streams, std::string* error,
                        const std::shared_ptr<TibDedupProvider>& provider) {
    if (!streams) return fail(error, "Missing TIB stream output");
    streams->clear();
    try {
        if (!source || source->capacity() < 0 || dataOffset >= std::uint64_t(source->capacity()) ||
            itemOffset >= std::uint64_t(source->capacity()) - dataOffset) {
            return fail(error, "Invalid TIB file record offset");
        }
        std::int64_t pos = static_cast<std::int64_t>(itemOffset + dataOffset);
        std::uint8_t tag = 0;
        if (!readExact(source, pos, &tag, 1) || tag != 'f') {
            return fail(error, "Missing TIB file record");
        }
        std::vector<std::uint8_t> data;
        std::int64_t next = 0;
        if (!record(source, pos, false, data.max_size(), &data, &next, error)) return false;
        pos = next;
        for (;;) {
            if (!readExact(source, pos, &tag, 1)) return fail(error, "Missing TIB block map");
            if (tag == 'l') break;
            if (tag != 'n' && tag > 0x63) {
                if (logicalSize != 0 || tag != 'f' || std::uint64_t(pos) < dataOffset ||
                    !std::binary_search(itemOffsets.begin(), itemOffsets.end(),
                                        std::uint64_t(pos) - dataOffset)) {
                    return fail(error, "Unsupported TIB file subrecord");
                }
                TibFileStream primary;
                primary.content = std::make_shared<FileStore>(source, 0, std::vector<Block>());
                streams->push_back(std::move(primary));
                if (error) error->clear();
                return true;
            }
            if (!record(source, pos, tag == 'n', data.max_size(), &data, &next, error)) return false;
            pos = next;
        }
        if (!record(source, pos, true, data.max_size(), &data, &next, error)) return false;
        Cursor c(data);
        const std::uint8_t* p = nullptr;
        if (!c.take(6, &p)) return fail(error, "Truncated TIB block map header");
        const std::uint8_t hashSize = p[3];
        if (provider && hashSize != 16) return fail(error, "Unsupported TIB deduplication hash size");
        if (p[0] != 1) {
            return fail(error, "Unsupported TIB block map variant");
        }
        const std::uint16_t streamCount = u16(p + 4);
        bool found = false;
        std::vector<TibFileStream> result;
        for (std::uint16_t i = 0; i < streamCount; ++i) {
            if (!c.take(2, &p)) return fail(error, "Truncated TIB stream name");
            const std::uint16_t nameBytes = u16(p);
            if ((nameBytes & 1) || !c.take(nameBytes, &p))
                return fail(error, "Invalid TIB stream name");
            TibFileStream stream;
            for (std::size_t n = 0; n < nameBytes; n += 2) {
                const std::uint16_t ch = u16(p + n);
                if (!ch || ch == '/' || ch == '\\') return fail(error, "Invalid TIB stream name");
                stream.name.push_back(static_cast<char16_t>(ch));
            }
            for (const TibFileStream& existing : result)
                if (existing.name == stream.name) return fail(error, "Duplicate TIB stream name");
            if (!c.take(12, &p)) return fail(error, "Invalid TIB stream header");
            const std::uint64_t length = u64(p);
            const std::uint32_t count = u32(p + 8);
            if (length > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
                return fail(error, "TIB stream too large");
            }
            const bool isPrimary = nameBytes == 0;
            if (isPrimary) found = true;
            std::vector<Block> blocks;
            std::uint64_t previous = 0;
            for (std::uint32_t b = 0; b < count; ++b) {
                if (!c.take(16, &p)) return fail(error, "Truncated TIB block map");
                const std::uint64_t logical = u64(p), relative = u64(p + 8);
                if (logical >= length || (b && logical <= previous) ||
                    (!provider && relative >= std::uint64_t(source->capacity()) - dataOffset)) {
                    return fail(error, "Invalid TIB block map");
                }
                // Provider records use the hash; direct records use the stored position.
                if (hashSize && !c.take(hashSize, &p)) return fail(error, "Truncated TIB block hash");
                previous = logical;
                Block block = {};
                block.logical = logical;
                block.physical = relative + dataOffset;
                if (provider) std::copy(p, p + block.hash.size(), block.hash.begin());
                blocks.push_back(block);
            }
            stream.content = std::make_shared<FileStore>(source, static_cast<std::int64_t>(length), std::move(blocks), provider);
            result.push_back(std::move(stream));
        }
        if (!found) return fail(error, "Invalid TIB primary stream");
        *streams = std::move(result);
        return true;
    } catch (const std::exception&) { return fail(error, "TIB file allocation or size error"); }
}

} // namespace fs
} // namespace peare
