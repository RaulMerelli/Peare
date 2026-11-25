#include "WimReader.h"

#include "BlockDecompressor.h"

#include <QCryptographicHash>
#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <utility>

namespace peare {
namespace fs {
namespace {

// WIM header / resource flag constants (DiscUtils FileFlags / ResourceFlags).
const std::uint32_t kFlagXpress = 0x00020000;
const std::uint32_t kFlagLzx = 0x00040000;
const std::uint8_t kResMetaData = 0x02;
const std::uint8_t kResCompressed = 0x04;
const std::uint32_t kAttrDirectory = 0x00000010;

std::uint16_t le16(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 2 > b.size()) return 0;
    return std::uint16_t(b[o]) | (std::uint16_t(b[o + 1]) << 8);
}
std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t o) {
    if (o + 4 > b.size()) return 0;
    return std::uint32_t(b[o]) | (std::uint32_t(b[o + 1]) << 8) |
           (std::uint32_t(b[o + 2]) << 16) | (std::uint32_t(b[o + 3]) << 24);
}
std::int64_t le64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return std::int64_t(le32(b, o)) | (std::int64_t(le32(b, o + 4)) << 32);
}

std::int64_t roundUp(std::int64_t v, std::int64_t a) { return ((v + a - 1) / a) * a; }

// UTF-16LE (n bytes) -> UTF-8, stopping at a NUL code unit.
std::string utf16ToUtf8(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t bytes) {
    std::string out;
    for (std::size_t i = 0; i + 1 < bytes && off + i + 1 < b.size(); i += 2) {
        const std::uint32_t c = std::uint32_t(b[off + i]) | (std::uint32_t(b[off + i + 1]) << 8);
        if (c == 0) break;
        if (c < 0x80) {
            out.push_back(char(c));
        } else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// A compressed WIM resource: chunk table + LZX/XPRESS decode per chunk, one
// chunk cached (ported from DiscUtils FileResourceStream).
class WimChunkStore : public IByteStore {
public:
    WimChunkStore(ByteStorePtr disc, std::int64_t fileOffset, std::int64_t compressedSize,
                  std::int64_t originalSize, std::int32_t chunkSize, bool compressed,
                  BlockDecompressorPtr decomp)
        : disc_(std::move(disc)), fileOffset_(fileOffset), compressedSize_(compressedSize),
          originalSize_(originalSize), chunkSize_(chunkSize), compressed_(compressed),
          decomp_(std::move(decomp)) {
        if (!disc_ || fileOffset_ < 0 || compressedSize_ < 0 || originalSize_ < 0 ||
            chunkSize_ <= 0 || fileOffset_ > disc_->capacity() ||
            compressedSize_ > disc_->capacity() - fileOffset_) return;
        const std::int64_t chunks = originalSize_ / chunkSize_ +
            (originalSize_ % chunkSize_ != 0 ? 1 : 0);
        entrySize_ = static_cast<std::uint64_t>(originalSize_) > UINT32_MAX ? 8 : 4;
        numChunks_ = chunks > 0 ? static_cast<std::uint64_t>(chunks) : 1;
        if (numChunks_ - 1 > static_cast<std::uint64_t>(compressedSize_) / entrySize_) return;
        offsetDelta_ = static_cast<std::int64_t>((numChunks_ - 1) * entrySize_);
        std::uint8_t buffer[65536];
        std::uint64_t previous = 0;
        for (std::int64_t at = 0; at < offsetDelta_;) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::int64_t>(sizeof(buffer), offsetDelta_ - at));
            if (!readFull(fileOffset_ + at, buffer, n)) return;
            for (std::size_t pos = 0; pos < n; pos += entrySize_) {
                std::uint64_t value = 0;
                for (unsigned int i = 0; i < entrySize_; ++i) value |= std::uint64_t(buffer[pos + i]) << (i * 8);
                if (value < previous || value > static_cast<std::uint64_t>(compressedSize_ - offsetDelta_)) return;
                previous = value;
            }
            at += static_cast<std::int64_t>(n);
        }
        valid_ = true;
    }

    std::int64_t capacity() const override { return originalSize_; }
    bool validate() const override { return valid_ && disc_->validate(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!valid_ || !dst || pos < 0 || count <= 0 || pos >= originalSize_) return 0;
        int produced = 0;
        while (count > 0 && pos < originalSize_) {
            const std::uint64_t chunk = static_cast<std::uint64_t>(pos / chunkSize_);
            const int chunkOff = int(pos % chunkSize_);
            if (!ensureChunk(chunk)) break;
            const int avail = int(cache_.size()) - chunkOff;
            if (avail <= 0) break;
            int n = count < avail ? count : avail;
            const std::int64_t rem = originalSize_ - pos;
            if (n > rem) n = int(rem);
            std::memcpy(dst + produced, cache_.data() + chunkOff, std::size_t(n));
            pos += n; produced += n; count -= n;
        }
        return produced;
    }

private:
    bool readFull(std::int64_t pos, std::uint8_t* dst, std::size_t count) const {
        std::size_t done = 0;
        while (done < count) {
            const int requested = static_cast<int>(std::min<std::size_t>(count - done, 65536));
            const int n = disc_->read(pos + static_cast<std::int64_t>(done), dst + done, requested);
            if (n <= 0 || n > requested) return false;
            done += n;
        }
        return disc_->validate();
    }

    bool chunkOffset(std::uint64_t index, std::uint64_t* result) const {
        if (index == 0) { *result = 0; return true; }
        if (index == numChunks_) { *result = static_cast<std::uint64_t>(compressedSize_ - offsetDelta_); return true; }
        if (index > numChunks_) return false;
        std::uint8_t bytes[8] = {};
        if (!readFull(fileOffset_ + static_cast<std::int64_t>((index - 1) * entrySize_), bytes, entrySize_)) return false;
        std::uint64_t value = 0;
        for (unsigned int i = 0; i < entrySize_; ++i) value |= std::uint64_t(bytes[i]) << (i * 8);
        *result = value;
        return value <= static_cast<std::uint64_t>(compressedSize_ - offsetDelta_);
    }

    bool ensureChunk(std::uint64_t chunk) const {
        if (cachedChunk_ == chunk) return true;
        if (chunk >= numChunks_) return false;
        const int target = (chunk == numChunks_ - 1)
            ? int(originalSize_ - std::int64_t(chunk) * chunkSize_)
            : chunkSize_;
        std::uint64_t begin = 0, end = 0;
        if (!chunkOffset(chunk, &begin) || !chunkOffset(chunk + 1, &end) || end < begin ||
            end - begin > std::vector<std::uint8_t>().max_size()) return false;
        std::vector<std::uint8_t> raw(static_cast<std::size_t>(end - begin));
        if (!raw.empty() && !readFull(fileOffset_ + offsetDelta_ + static_cast<std::int64_t>(begin),
                                      raw.data(), raw.size())) return false;

        cachedChunk_ = UINT64_MAX;
        cache_.assign(std::size_t(target), 0);
        if (!decomp_ || !compressed_ || raw.size() == static_cast<std::size_t>(target)) {
            if (raw.size() != static_cast<std::size_t>(target)) return false;
            if (target > 0) std::memcpy(cache_.data(), raw.data(), std::size_t(target));
        } else {
            std::size_t out = 0;
            if (!decomp_->tryDecompress(raw.data(), raw.size(), cache_.data(),
                                        std::size_t(target), out) || out != std::size_t(target))
                return false;
        }
        cachedChunk_ = chunk;
        return true;
    }

    ByteStorePtr disc_;
    std::int64_t fileOffset_, compressedSize_, originalSize_;
    std::int32_t chunkSize_;
    bool compressed_;
    BlockDecompressorPtr decomp_;
    std::uint64_t numChunks_ = 0;
    unsigned int entrySize_ = 0;
    std::int64_t offsetDelta_ = 0;
    mutable std::uint64_t cachedChunk_ = UINT64_MAX;
    mutable std::vector<std::uint8_t> cache_;
    bool valid_ = false;
    mutable std::mutex mutex_;
};

bool readWofExact(const ByteStorePtr& source, std::uint64_t offset,
                  std::uint8_t* data, std::size_t length) {
    if (!source || !source->validate() || source->capacity() < 0 ||
        offset > static_cast<std::uint64_t>(source->capacity()) ||
        length > static_cast<std::uint64_t>(source->capacity()) - offset) return false;
    while (length) {
        const int requested = static_cast<int>(std::min<std::size_t>(length, 65536));
        const int n = source->read(static_cast<std::int64_t>(offset), data, requested);
        if (n <= 0 || n > requested) return false;
        offset += static_cast<std::uint64_t>(n);
        data += n;
        length -= static_cast<std::size_t>(n);
    }
    return source->validate();
}

class WofWimWindow final : public IByteStore {
public:
    WofWimWindow(ByteStorePtr source, std::int64_t offset, std::int64_t size)
        : source_(std::move(source)), offset_(offset), size_(size) {}
    std::int64_t capacity() const override { return size_; }
    bool validate() const override { return source_ && source_->validate(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || pos >= size_) return 0;
        const int n = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
        return readWofExact(source_, static_cast<std::uint64_t>(offset_ + pos), dst,
                            static_cast<std::size_t>(n)) ? n : -1;
    }
private:
    ByteStorePtr source_;
    std::int64_t offset_, size_;
};

class VerifiedWofWimStore final : public IByteStore {
public:
    VerifiedWofWimStore(ByteStorePtr source, const std::array<std::uint8_t, 20>& hash)
        : source_(std::move(source)), hash_(hash), length_(source_ ? source_->capacity() : -1) {}
    std::int64_t capacity() const override { return length_; }
    bool validate() const override {
        try { return source_ && source_->capacity() == length_ && source_->validate(); }
        catch (...) { return false; }
    }
    bool cheapRandomAccess() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return ready_ && source_ && source_->cheapRandomAccess();
    }
    bool verifyContent() const {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            return prepareLocked();
        } catch (...) { return false; }
    }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
      try {
        if (!validate() || pos < 0 || count < 0 || (count && !dst)) return -1;
        if (!count || pos >= capacity()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!prepareLocked()) return -1;
        const int wanted = static_cast<int>(std::min<std::int64_t>(
            std::min<std::int64_t>(count, length_ - pos), 1024 * 1024));
        std::vector<std::uint8_t> staged(static_cast<std::size_t>(wanted));
        int done = 0;
        while (done < wanted) {
            const std::uint64_t at = static_cast<std::uint64_t>(pos) + done;
            const std::uint64_t block = at / kBlockSize;
            const std::size_t within = static_cast<std::size_t>(at % kBlockSize);
            if (cachedBlock_ != block) {
                const std::uint64_t start = block * kBlockSize;
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(
                    kBlockSize, static_cast<std::uint64_t>(length_) - start));
                std::vector<std::uint8_t> bytes(n);
                if (!readWofExact(source_, start, bytes.data(), bytes.size())) return -1;
                const QByteArray digest = QCryptographicHash::hash(
                    QByteArray::fromRawData(reinterpret_cast<const char*>(bytes.data()), static_cast<int>(n)),
                    QCryptographicHash::Sha256);
                if (digest.size() != 32 || block >= digests_.size() ||
                    std::memcmp(digest.constData(), digests_[static_cast<std::size_t>(block)].data(), 32) != 0)
                    return -1;
                cache_.swap(bytes);
                cachedBlock_ = block;
            }
            const std::size_t n = std::min<std::size_t>(wanted - done, cache_.size() - within);
            std::memcpy(staged.data() + done, cache_.data() + within, n);
            done += static_cast<int>(n);
        }
        if (!validate()) return -1;
        std::memcpy(dst, staged.data(), static_cast<std::size_t>(done));
        return done;
      } catch (...) { return -1; }
    }
private:
    static const std::uint64_t kBlockSize = 65536;
    typedef std::array<std::uint8_t, 32> Digest;
    bool prepareLocked() const {
        if (ready_) return validate();
        if (length_ < 0 || !validate()) return false;
        const std::uint64_t length = static_cast<std::uint64_t>(length_);
        const std::uint64_t blocks = length / kBlockSize + (length % kBlockSize != 0);
        std::vector<Digest> digests;
        if (blocks > digests.max_size()) return false;
        digests.reserve(static_cast<std::size_t>(blocks));
        std::uint8_t buffer[kBlockSize];
        QCryptographicHash full(QCryptographicHash::Sha1);
        for (std::uint64_t offset = 0; offset < length;) {
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(kBlockSize, length - offset));
            if (!readWofExact(source_, offset, buffer, n)) return false;
            full.addData(reinterpret_cast<const char*>(buffer), static_cast<int>(n));
            const QByteArray digest = QCryptographicHash::hash(
                QByteArray::fromRawData(reinterpret_cast<const char*>(buffer), static_cast<int>(n)),
                QCryptographicHash::Sha256);
            if (digest.size() != 32) return false;
            Digest entry;
            std::memcpy(entry.data(), digest.constData(), entry.size());
            digests.push_back(entry);
            offset += n;
        }
        const QByteArray actual = full.result();
        if (actual.size() != 20 || std::memcmp(actual.constData(), hash_.data(), 20) != 0 || !validate()) return false;
        digests_.swap(digests);
        ready_ = true;
        return true;
    }
    ByteStorePtr source_;
    std::array<std::uint8_t, 20> hash_;
    std::int64_t length_;
    mutable std::mutex mutex_;
    mutable bool ready_ = false;
    mutable std::vector<Digest> digests_;
    mutable std::uint64_t cachedBlock_ = UINT64_MAX;
    mutable std::vector<std::uint8_t> cache_;
};

const std::uint64_t VerifiedWofWimStore::kBlockSize;

}  // namespace

ByteStorePtr WimReader::openWofResource(ByteStorePtr source, const WofWimReference& ref,
                                       bool verifyContentAtOpen) {
    try {
        std::vector<std::uint8_t> header(148);
        if (!readWofExact(source, 0, header.data(), header.size()) ||
            std::memcmp(header.data(), "MSWIM\0\0\0", 8) != 0 ||
            le32(header, 8) < header.size() ||
            le32(header, 8) > static_cast<std::uint64_t>(source->capacity()) ||
            std::memcmp(header.data() + 24, ref.guid.data(), ref.guid.size()) != 0 ||
            le16(header, 40) != 1 || le16(header, 42) != 1 ||
            !ref.imageIndex || ref.imageIndex > le32(header, 44)) return nullptr;
        const std::uint32_t flags = le32(header, 16);
        const std::uint32_t chunk = le32(header, 20);
        const auto parseHeader = [](const std::vector<std::uint8_t>& bytes, std::size_t at,
                                    ResHeader* result) -> bool {
            if (!result || at > bytes.size() || bytes.size() - at < 24) return false;
            const std::uint64_t first = std::uint64_t(le32(bytes, at)) |
                                        (std::uint64_t(le32(bytes, at + 4)) << 32);
            const std::uint64_t offset = std::uint64_t(le32(bytes, at + 8)) |
                                         (std::uint64_t(le32(bytes, at + 12)) << 32);
            const std::uint64_t size = std::uint64_t(le32(bytes, at + 16)) |
                                       (std::uint64_t(le32(bytes, at + 20)) << 32);
            if (offset > std::uint64_t(INT64_MAX) || size > std::uint64_t(INT64_MAX)) return false;
            result->flags = static_cast<std::uint8_t>(first >> 56);
            result->compressedSize = static_cast<std::int64_t>(first & 0x00ffffffffffffffULL);
            result->fileOffset = static_cast<std::int64_t>(offset);
            result->originalSize = static_cast<std::int64_t>(size);
            return true;
        };
        ResHeader table;
        if (!parseHeader(header, 48, &table)) return nullptr;
        if (table.fileOffset < 0 || table.compressedSize < 0 || table.originalSize < 0 ||
            (table.flags & ~(kResCompressed | kResMetaData)) || table.originalSize % 50 != 0 ||
            table.fileOffset > source->capacity() || table.compressedSize > source->capacity() - table.fileOffset)
            return nullptr;
        ByteStorePtr tableWindow = std::make_shared<WofWimWindow>(source, table.fileOffset, table.compressedSize);
        const auto verifiedTable = std::make_shared<VerifiedWofWimStore>(tableWindow, ref.tableHash);
        if (!verifiedTable->verifyContent()) return nullptr;
        ByteStorePtr tableSource = verifiedTable;
        BlockDecompressorPtr decoder;
        if (flags & kFlagLzx) decoder = std::make_shared<LzxWimDecompressor>(chunk);
        else if (flags & kFlagXpress) decoder = std::make_shared<XpressHuffmanDecompressor>();
        if ((flags & kFlagLzx) && (flags & kFlagXpress)) return nullptr;
        const auto make = [&](const ResHeader& h, const ByteStorePtr& backing) -> ByteStorePtr {
            if (h.fileOffset < 0 || h.compressedSize < 0 || h.originalSize < 0 ||
                !backing || h.fileOffset > backing->capacity() || h.compressedSize > backing->capacity() - h.fileOffset ||
                (h.flags & ~kResCompressed)) return nullptr;
            if (!(h.flags & kResCompressed)) {
                if (h.compressedSize != h.originalSize) return nullptr;
                return std::make_shared<WofWimWindow>(backing, h.fileOffset, h.originalSize);
            }
            if (!decoder || !chunk || chunk > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) return nullptr;
            auto result = std::make_shared<WimChunkStore>(backing, h.fileOffset, h.compressedSize,
                                                          h.originalSize, static_cast<std::int32_t>(chunk), true, decoder);
            return result->validate() ? result : ByteStorePtr();
        };
        ResHeader tableStream = table;
        tableStream.flags &= static_cast<std::uint8_t>(~kResMetaData);
        tableStream.fileOffset = 0;
        ByteStorePtr index = make(tableStream, tableSource);
        if (!index) return nullptr;
        ResHeader match;
        bool found = false;
        for (std::uint64_t at = 0; at < static_cast<std::uint64_t>(table.originalSize); at += 50) {
            std::vector<std::uint8_t> entry(50);
            if (!readWofExact(index, at, entry.data(), entry.size())) return nullptr;
            if (std::memcmp(entry.data() + 30, ref.contentHash.data(), 20) != 0) continue;
            if (found || le16(entry, 24) != 1) return nullptr;
            if (!parseHeader(entry, 0, &match)) return nullptr;
            found = true;
        }
        if (!found || match.originalSize < 0 || match.compressedSize < 0 || match.fileOffset < 0 ||
            static_cast<std::uint64_t>(match.originalSize) != ref.size ||
            static_cast<std::uint64_t>(match.compressedSize) != ref.storedSize ||
            static_cast<std::uint64_t>(match.fileOffset) != ref.offset) return nullptr;
        ByteStorePtr content = make(match, source);
        if (!content) return nullptr;
        const auto verified = std::make_shared<VerifiedWofWimStore>(content, ref.contentHash);
        if (verifyContentAtOpen && !verified->verifyContent()) return nullptr;
        return verified;
    } catch (...) { return nullptr; }
}

WimReader::WimReader(ByteStorePtr disc) : parts_{std::move(disc)} {
    try {
        parse();
    } catch (...) {
        valid_ = false;
        if (error_.empty()) error_ = "WIM parse error";
    }
}

WimReader::WimReader(std::vector<ByteStorePtr> parts) : parts_(std::move(parts)) {
    try {
        parse();
    } catch (...) {
        valid_ = false;
        if (error_.empty()) error_ = "WIM parse error";
    }
}

WimReader::ResHeader WimReader::readResHeader(const std::vector<std::uint8_t>& buf, std::size_t off) {
    ResHeader h;
    const std::int64_t field0 = le64(buf, off);
    h.flags = std::uint8_t((field0 >> 56) & 0xFF);
    h.compressedSize = field0 & 0x00FFFFFFFFFFFFFFLL;
    h.fileOffset = le64(buf, off + 8);
    h.originalSize = le64(buf, off + 16);
    return h;
}

ByteStorePtr WimReader::makeResourceStore(const ResHeader& hdr) const {
    const ByteStorePtr& source = hdr.source ? hdr.source : parts_.front();
    if ((hdr.flags & kResCompressed) == 0)
        return std::make_shared<SubStore>(source, hdr.fileOffset, hdr.compressedSize);

    BlockDecompressorPtr decomp;
    if (fileFlags_ & kFlagLzx)
        decomp = std::make_shared<LzxWimDecompressor>(std::size_t(chunkSize_));
    else if (fileFlags_ & kFlagXpress)
        decomp = std::make_shared<XpressHuffmanDecompressor>();
    return std::make_shared<WimChunkStore>(source, hdr.fileOffset, hdr.compressedSize,
                                           hdr.originalSize, chunkSize_, true, decomp);
}

std::vector<std::uint8_t> WimReader::materialize(const ResHeader& hdr) const {
    return makeResourceStore(hdr)->readAll();
}

void WimReader::parse() {
    if (parts_.empty() || !parts_.front()) {
        error_ = "No WIM parts were provided";
        return;
    }
    const std::size_t kResInfo = 24 + 26;  // ShortResourceHeader + 26
    std::uint16_t expectedParts = 0;
    std::uint32_t expectedChunkSize = 0;
    std::vector<std::uint8_t> guid;
    bool haveMeta = false;
    for (std::size_t partIndex = 0; partIndex < parts_.size(); ++partIndex) {
        if (!parts_[partIndex]) {
            error_ = "A WIM part has no readable byte source";
            return;
        }
        const std::vector<std::uint8_t> header = parts_[partIndex]->readRange(0, 512);
        if (header.size() < 148 ||
            std::memcmp(header.data(), "MSWIM\0\0\0", 8) != 0) {
            error_ = "Not a valid WIM file";
            return;
        }
        const std::uint16_t partNumber = le16(header, 40);
        const std::uint16_t totalParts = le16(header, 42);
        if (totalParts == 0 || partNumber != partIndex + 1) {
            error_ = "WIM parts are missing, duplicated, or out of order";
            return;
        }
        if (partIndex == 0) {
            expectedParts = totalParts;
            guid.assign(header.begin() + 24, header.begin() + 40);
            if (expectedParts != parts_.size()) {
                error_ = "The split WIM is incomplete";
                return;
            }
            fileFlags_ = le32(header, 16);
            expectedChunkSize = le32(header, 20);
            chunkSize_ = std::int32_t(expectedChunkSize);
            if (chunkSize_ <= 0) chunkSize_ = 32768;
        } else if (totalParts != expectedParts ||
                   !std::equal(guid.begin(), guid.end(), header.begin() + 24) ||
                   le32(header, 16) != fileFlags_ ||
                   le32(header, 20) != expectedChunkSize) {
            error_ = "WIM parts do not belong to the same split image";
            return;
        }

        ResHeader lookup = readResHeader(header, 48);
        lookup.source = parts_[partIndex];
        const std::vector<std::uint8_t> table = materialize(lookup);
        for (std::size_t off = 0; off + kResInfo <= table.size(); off += kResInfo) {
            ResHeader rh = readResHeader(table, off);
            rh.source = parts_[partIndex];
            std::vector<std::uint8_t> hash(table.begin() + off + 24 + 6,
                                           table.begin() + off + 24 + 6 + 20);
            if ((rh.flags & kResMetaData) != 0) {
                if (partIndex == 0 && !haveMeta) {
                    metadata_ = rh;
                    haveMeta = true;
                }
            } else {
                resources_.emplace(std::move(hash), std::move(rh));
            }
        }
    }
    if (!haveMeta) { error_ = "WIM has no image metadata"; return; }

    // Metadata stream: security block then the directory tree.
    meta_ = materialize(metadata_);
    if (meta_.size() < 8) { error_ = "WIM metadata truncated"; return; }
    const std::uint32_t totalLength = le32(meta_, 0);
    const std::int64_t rawRoot = roundUp(std::int64_t(totalLength), 8);
    // The record at rawRoot is the (unnamed) root directory entry; the actual
    // top-level children begin at its subdirOffset.
    rootDirPos_ = rawRoot;
    if (std::size_t(rawRoot) + 24 <= meta_.size()) {
        const std::int64_t rootSub = le64(meta_, std::size_t(rawRoot) + 16);
        if (rootSub != 0) rootDirPos_ = rootSub;
    }

    valid_ = true;
}

const std::vector<WimReader::DirEntry>& WimReader::directoryAt(std::int64_t offset) const {
    auto it = dirCache_.find(offset);
    if (it != dirCache_.end()) return it->second;

    std::vector<DirEntry> dir;
    std::int64_t pos = (offset == 0) ? rootDirPos_ : offset;
    while (pos >= 0 && std::size_t(pos) + 8 <= meta_.size()) {
        const std::int64_t length = le64(meta_, std::size_t(pos));
        if (length == 0) break;  // end of this directory
        if (length < 102 || std::uint64_t(length) > meta_.size() - std::size_t(pos) ||
            std::size_t(pos) + 102 > meta_.size()) break;
        DirEntry e;
        const std::uint32_t attributes = le32(meta_, std::size_t(pos) + 8);
        e.subdirOffset = le64(meta_, std::size_t(pos) + 16);
        e.hash.assign(meta_.begin() + std::size_t(pos) + 64, meta_.begin() + std::size_t(pos) + 84);
        const int fileNameLen = le16(meta_, std::size_t(pos) + 100);
        const int shortNameLen = le16(meta_, std::size_t(pos) + 98);
        const int streamCount = le16(meta_, std::size_t(pos) + 96);
        const std::uint64_t namesEnd = 102u + std::uint64_t(fileNameLen) + 2u +
                                       std::uint64_t(shortNameLen);
        if ((fileNameLen & 1) || (shortNameLen & 1) || namesEnd > std::uint64_t(length))
            break;
        if (fileNameLen > 0)
            e.name = utf16ToUtf8(meta_, std::size_t(pos) + 102, std::size_t(fileNameLen));
        e.isDirectory = (attributes & kAttrDirectory) != 0;
        std::size_t next = std::size_t(pos) + std::size_t(length);
        bool validStreams = true;
        for (int i = 0; i < streamCount; ++i) {
            if (next > meta_.size() || meta_.size() - next < 38) {
                validStreams = false;
                break;
            }
            const std::uint64_t streamLength = le64(meta_, next);
            const std::uint16_t streamNameLength = le16(meta_, next + 36);
            if (streamLength < 38 || streamLength > meta_.size() - next ||
                (streamNameLength & 1) || 38u + streamNameLength > streamLength) {
                validStreams = false;
                break;
            }
            if (i == 0 && streamNameLength == 0 && !e.isDirectory)
                e.hash.assign(meta_.begin() + next + 16, meta_.begin() + next + 36);
            next += std::size_t(streamLength);
        }
        if (!validStreams) break;
        // File length comes from the content resource (originalSize).
        bool allZero = true;
        for (std::uint8_t h : e.hash) if (h) { allZero = false; break; }
        if (!e.isDirectory && !allZero) {
            auto r = resources_.find(e.hash);
            if (r != resources_.end()) e.length = r->second.originalSize;
        }
        if (!e.name.empty()) dir.push_back(std::move(e));
        pos = std::int64_t(next);
    }
    auto res = dirCache_.emplace(offset, std::move(dir));
    return res.first->second;
}

const WimReader::DirEntry* WimReader::entryForPath(const std::string& path) const {
    // Split on '/' and '\\'.
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') { if (!cur.empty()) { parts.push_back(cur); cur.clear(); } }
        else cur.push_back(c);
    }
    if (!cur.empty()) parts.push_back(cur);

    const std::vector<DirEntry>* dir = &directoryAt(0);
    const DirEntry* found = nullptr;
    for (std::size_t partIndex = 0; partIndex < parts.size(); ++partIndex) {
        const std::string& seg = parts[partIndex];
        found = nullptr;
        for (const DirEntry& e : *dir) {
            // Case-insensitive compare.
            if (e.name.size() == seg.size()) {
                bool eq = true;
                for (std::size_t i = 0; i < seg.size(); ++i) {
                    char a = e.name[i], b = seg[i];
                    if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                    if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                    if (a != b) { eq = false; break; }
                }
                if (eq) { found = &e; break; }
            }
        }
        if (!found) return nullptr;
        if (partIndex + 1 < parts.size()) {
            if (!found->isDirectory || found->subdirOffset == 0) return nullptr;
            dir = &directoryAt(found->subdirOffset);
        }
    }
    return found;
}

std::vector<DiscEntry> WimReader::list(const std::string& dirPath) const {
    std::lock_guard<std::mutex> lock(dirMutex_);
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    const std::vector<DirEntry>* dir = nullptr;
    if (dirPath.empty() || dirPath == "/" || dirPath == "\\") {
        dir = &directoryAt(0);
    } else {
        const DirEntry* e = entryForPath(dirPath);
        if (!e || !e->isDirectory || e->subdirOffset == 0) return out;
        dir = &directoryAt(e->subdirOffset);
    }
    for (const DirEntry& e : *dir) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.length;
        out.push_back(d);
    }
    return out;
}

ByteStorePtr WimReader::openFile(const std::string& path) const {
    std::lock_guard<std::mutex> lock(dirMutex_);
    if (!valid_) return nullptr;
    const DirEntry* e = entryForPath(path);
    if (!e || e->isDirectory) return nullptr;
    bool allZero = true;
    for (std::uint8_t h : e->hash) if (h) { allZero = false; break; }
    if (allZero) return std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
    auto r = resources_.find(e->hash);
    if (r == resources_.end()) return nullptr;
    return makeResourceStore(r->second);
}

}  // namespace fs
}  // namespace peare
