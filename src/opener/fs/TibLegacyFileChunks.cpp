#include "TibLegacyFileChunks.h"

#include "../../common/compression/DeflateDecoder.h"
#include "../compression/Crc32.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>
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

class Input final : public compression::InflateInput {
public:
    Input(ByteStorePtr source, std::int64_t start, std::int64_t end)
        : source_(std::move(source)), start_(start), end_(end), pos_(start),
          bufferStart_(-1), bufferSize_(0), failed_(false) {}
    bool readByte(std::uint8_t* value) override {
        if (!value || pos_ >= end_) return false;
        if (bufferStart_ < 0 || pos_ < bufferStart_ || pos_ - bufferStart_ >= bufferSize_) {
            bufferStart_ = pos_;
            const int wanted = static_cast<int>(std::min<std::int64_t>(buffer_.size(), end_ - pos_));
            bufferSize_ = source_->read(pos_, buffer_.data(), wanted);
            if (bufferSize_ <= 0 || bufferSize_ > wanted) { failed_ = true; return false; }
        }
        *value = buffer_[static_cast<std::size_t>(pos_++ - bufferStart_)];
        return true;
    }
    std::size_t consumed() const override { return static_cast<std::size_t>(pos_ - start_); }
    bool rewind(std::size_t count) override {
        if (count > static_cast<std::uint64_t>(pos_ - start_)) return false;
        pos_ -= static_cast<std::int64_t>(count);
        return true;
    }
    bool failed() const { return failed_; }
private:
    ByteStorePtr source_;
    std::int64_t start_, end_, pos_, bufferStart_;
    int bufferSize_;
    bool failed_;
    std::array<std::uint8_t, 4096> buffer_;
};

struct Record {
    std::int64_t end;
    std::size_t size;
    std::uint32_t checksum;
};

bool decode(const ByteStorePtr& source, std::int64_t offset, std::int64_t end, std::uint8_t expectedType,
            const std::function<bool(std::uint8_t)>& sink, Record* record,
            std::string* error) {
    if (!source || !source->validate() || offset < 0 || end > source->capacity() || offset >= end ||
        static_cast<std::uint64_t>(end - offset) > std::numeric_limits<std::size_t>::max())
        return fail(error, "Invalid TIB legacy record bounds");
    Input input(source, offset + 1, end);
    std::uint8_t type = 0;
    if (source->read(offset, &type, 1) != 1) return fail(error, "TIB legacy tag read failed");
    if (type != expectedType) return fail(error, "TIB legacy record type changed");
    const bool zlib = type == 'l' || type == 'm' || type == 'n';
    std::size_t windowSize = 32768;
    if (zlib) {
        std::uint8_t cmf = 0, flg = 0;
        if (!input.readByte(&cmf) || !input.readByte(&flg) || (cmf & 15U) != 8U ||
            (cmf >> 4) > 7U || ((unsigned(cmf) << 8) | flg) % 31U || (flg & 32U))
            return fail(error, "Invalid TIB legacy zlib header");
        windowSize = std::size_t(1) << ((cmf >> 4) + 8);
    }
    std::uint32_t crc = 0xffffffffU;
    std::uint32_t low = 1, high = 0;
    const auto& table = compression::crc32Table()[0];
    std::size_t size = 0;
    const bool ok = compression::inflateRawStreaming(input, [&](std::uint8_t value) {
        crc = (crc >> 8) ^ table[(crc ^ value) & 0xffU];
        low = (low + value) % 65521U;
        high = (high + low) % 65521U;
        return sink(value);
    }, &size, windowSize, error);
    if (!ok || input.failed()) return fail(error, "Invalid TIB legacy compressed record");
    std::uint32_t stored = 0;
    for (unsigned i = 0; i < 4; ++i) {
        std::uint8_t byte = 0;
        if (!input.readByte(&byte)) return fail(error, "Truncated TIB legacy checksum");
        if (zlib) stored = (stored << 8) | byte;
        else stored |= std::uint32_t(byte) << (8 * i);
    }
    if ((zlib ? ((high << 16) | low) : ~crc) != stored)
        return fail(error, "TIB legacy checksum mismatch");
    record->end = offset + 1 + static_cast<std::int64_t>(input.consumed());
    record->size = size;
    record->checksum = stored;
    return true;
}

class Stream final : public IByteStore {
public:
    Stream(ByteStorePtr source, std::int64_t offset, std::uint8_t type, Record record)
        : source_(std::move(source)), offset_(offset), type_(type), record_(record) {}
    std::int64_t capacity() const override { return static_cast<std::int64_t>(record_.size); }
    bool validate() const override { return source_ && source_->validate(); }
    bool cheapRandomAccess() const override { return false; }
    bool visit(const std::function<bool(std::uint8_t)>& sink, std::string* error) const {
        Record current = {};
        if (!decode(source_, offset_, record_.end, type_, sink, &current, error)) return false;
        if (current.end != record_.end || current.size != record_.size || current.checksum != record_.checksum)
            return fail(error, "TIB legacy record changed");
        return true;
    }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate()) return -1;
        if (!dst || pos < 0 || count <= 0 || pos >= capacity()) return 0;
        try {
            const int amount = static_cast<int>(std::min<std::int64_t>(count, capacity() - pos));
            std::vector<std::uint8_t> result;
            result.reserve(static_cast<std::size_t>(amount));
            std::size_t at = 0;
            Record current = {};
            if (!decode(source_, offset_, record_.end, type_, [&](std::uint8_t value) {
                if (at >= static_cast<std::size_t>(pos) && result.size() < static_cast<std::size_t>(amount))
                    result.push_back(value);
                ++at;
                return true;
            }, &current, nullptr) || current.end != record_.end || current.size != record_.size ||
                current.checksum != record_.checksum || result.size() != static_cast<std::size_t>(amount)) return -1;
            std::memcpy(dst, result.data(), result.size());
            return amount;
        } catch (const std::bad_alloc&) { return -1; }
          catch (const std::length_error&) { return -1; }
    }
private:
    ByteStorePtr source_;
    std::int64_t offset_;
    std::uint8_t type_;
    Record record_;
};

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

struct Extent {
    std::int64_t logical, source, size;
};

class SparseStream final : public IByteStore {
public:
    SparseStream(ByteStorePtr source, std::int64_t size, std::vector<Extent> extents)
        : source_(std::move(source)), size_(size), extents_(std::move(extents)) {}
    std::int64_t capacity() const override { return size_; }
    bool validate() const override { return source_ && source_->validate(); }
    bool cheapRandomAccess() const override { return false; }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate()) return -1;
        if (!dst || pos < 0 || count <= 0 || pos >= size_) return 0;
        try {
            const int amount = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
            std::vector<std::uint8_t> result(static_cast<std::size_t>(amount), 0);
            const std::int64_t end = pos + amount;
            const std::shared_ptr<Stream> raw = std::dynamic_pointer_cast<Stream>(source_);
            if (!raw) return -1;
            std::size_t index = 0;
            std::int64_t at = 0;
            if (!raw->visit([&](std::uint8_t byte) {
                while (index < extents_.size() && at >= extents_[index].source + extents_[index].size)
                    ++index;
                if (index < extents_.size()) {
                    const Extent& e = extents_[index];
                    if (at >= e.source) {
                        const std::int64_t logical = e.logical + (at - e.source);
                        if (logical >= pos && logical < end)
                            result[static_cast<std::size_t>(logical - pos)] = byte;
                    }
                }
                ++at;
                return true;
            }, nullptr)) return -1;
            std::memcpy(dst, result.data(), result.size());
            return amount;
        } catch (const std::bad_alloc&) { return -1; }
          catch (const std::length_error&) { return -1; }
    }
private:
    ByteStorePtr source_;
    std::int64_t size_;
    std::vector<Extent> extents_;
};

bool openBackupStream(const ByteStorePtr& source, ByteStorePtr* out, std::string* error) {
    const std::shared_ptr<Stream> raw = std::dynamic_pointer_cast<Stream>(source);
    if (!raw) return fail(error, "Invalid TIB legacy backup stream");
    const std::uint64_t size = static_cast<std::uint64_t>(raw->capacity());
    std::array<std::uint8_t, 20> header = {};
    std::array<std::uint8_t, 8> offsetBytes = {};
    std::size_t headerUsed = 0, offsetUsed = 0;
    std::uint64_t pos = 0, skip = 0, sparseBody = 0, logical = 0;
    bool readingOffset = false, recognized = false, malformed = false;
    std::vector<Extent> extents;
    const std::uint64_t maxPosition = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    auto append = [&](std::uint64_t start, std::uint64_t data, std::uint64_t length) {
        if (start < logical || start > maxPosition || length > maxPosition - start ||
            data > maxPosition || length > maxPosition - data) return false;
        if (length) extents.push_back(Extent{static_cast<std::int64_t>(start),
                                             static_cast<std::int64_t>(data),
                                             static_cast<std::int64_t>(length)});
        logical = start + length;
        recognized = true;
        return true;
    };
    const bool ok = raw->visit([&](std::uint8_t byte) {
        const std::uint64_t next = pos + 1;
        if (skip) {
            --skip;
        } else if (readingOffset) {
            offsetBytes[offsetUsed++] = byte;
            if (offsetUsed == offsetBytes.size()) {
                if (!append(le64(offsetBytes.data()), next, sparseBody)) { malformed = true; return false; }
                skip = sparseBody;
                readingOffset = false;
                offsetUsed = 0;
            }
        } else {
            header[headerUsed++] = byte;
            if (headerUsed == header.size()) {
                headerUsed = 0;
                const std::uint32_t id = le32(header.data());
                const std::uint64_t body = le64(header.data() + 8);
                const std::uint32_t name = le32(header.data() + 16);
                if (next > size || name > size - next || body > size - next - name) {
                    malformed = true; return false;
                }
                if (name || (id != 1 && id != 9)) {
                    skip = std::uint64_t(name) + body;
                } else if (id == 1) {
                    if (!append(logical, next, body)) { malformed = true; return false; }
                    skip = body;
                } else {
                    if (body < 8) { malformed = true; return false; }
                    readingOffset = true;
                    sparseBody = body - 8;
                }
            }
        }
        pos = next;
        return true;
    }, error);
    if (!ok) return malformed ? fail(error, "Invalid TIB legacy NT Backup stream") : false;
    if (pos != size || skip || readingOffset || headerUsed)
        return fail(error, "Truncated TIB legacy NT Backup stream");
    if (!recognized) return fail(error, "TIB legacy NT Backup primary data not found");
    *out = std::make_shared<SparseStream>(source, static_cast<std::int64_t>(logical), std::move(extents));
    return true;
}

} // namespace

TibLegacyFileChunks::TibLegacyFileChunks(ByteStorePtr source, std::int64_t record, std::int64_t end)
    : source_(std::move(source)), position_(record), end_(end), started_(false), ended_(false), pending_(false), failed_(false) {}

bool TibLegacyFileChunks::next(TibLegacyChunk* chunk, std::string* error) {
    if (error) error->clear();
    if (!chunk) return fail(error, "Missing TIB legacy chunk destination");
    if (failed_) return fail(error, "TIB legacy sequence is in an error state");
    failed_ = true;
    chunk->stream.reset();
    chunk->hash.bytes.clear();
    chunk->metadata.clear();
    chunk->attributes = 0;
    chunk->directoryAttribute = false;
    chunk->internalType = 0;
    chunk->structureSize = 0;
    try {
        while (!ended_) {
            if (!source_ || !source_->validate() || position_ < 0 || position_ >= end_ || end_ > source_->capacity())
                return fail(error, "Truncated TIB legacy sequence");
            std::uint8_t type = 0;
            if (source_->read(position_, &type, 1) != 1) return fail(error, "TIB legacy tag read failed");
            if (!started_ && type != 'f') return fail(error, "Missing TIB legacy file record");
            if (started_ && type >= 100 && type != 'k') {
                ended_ = true;
                if (pending_) {
                    chunk->kind = TibLegacyChunkKind::Hash;
                    chunk->hash = std::move(hash_);
                    pending_ = false;
                    failed_ = false;
                    return true;
                }
                break;
            }
            if (type == 4 && pending_) return fail(error, "TIB legacy stream 4 has a pending hash");
            if (type == 'k' && pending_)
                return fail(error, "TIB legacy hash replacement requires unsupported provider processing");
            std::vector<std::uint8_t> prefix;
            const std::size_t wanted = type == 'k' ? 65538U :
                (type == 5 ? 16U : (type == 6 ? 22U + 2U * std::numeric_limits<std::uint16_t>::max() :
                 (type == 1 ? 4U : 0U)));
            Record record = {};
            if (!decode(source_, position_, end_, type, [&](std::uint8_t value) {
                if (prefix.size() < wanted) prefix.push_back(value);
                return true;
            }, &record, error)) return false;
            const std::int64_t offset = position_;
            position_ = record.end;
            started_ = true;
            if (type == 'k') {
                std::size_t consumed = 0;
                if (!parseTibLegacyHashChunk(prefix, &hash_, &consumed, error)) return false;
                pending_ = true;
            } else if (type == 1) {
                if (record.size < 4) return fail(error, "Truncated TIB legacy attributes");
                chunk->kind = TibLegacyChunkKind::Attributes1;
                chunk->attributes = le32(prefix.data());
                chunk->directoryAttribute = (chunk->attributes & 0x10U) != 0;
                failed_ = false;
                return true;
            } else if (type == 6) {
                if (record.size < 20) return fail(error, "Truncated TIB legacy type metadata");
                const std::uint32_t length = le32(prefix.data());
                if (length < 20 || length > record.size) return fail(error, "Invalid TIB legacy type metadata size");
                chunk->kind = TibLegacyChunkKind::Metadata6;
                chunk->structureSize = length;
                chunk->internalType = le32(prefix.data() + 4);
                chunk->metadata = std::move(prefix);
                failed_ = false;
                return true;
            } else if (type == 5) {
                if (record.size < 16) return fail(error, "Truncated TIB legacy chunk 5");
                chunk->kind = TibLegacyChunkKind::Metadata5;
                chunk->metadata = std::move(prefix);
                failed_ = false;
                return true;
            } else if (type == 2 || type == 4) {
                if (record.size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                    return fail(error, "TIB legacy stream size is not representable");
                chunk->kind = type == 2 ? TibLegacyChunkKind::Stream2 : TibLegacyChunkKind::Stream4;
                chunk->stream = std::make_shared<Stream>(source_, offset, type, record);
                failed_ = false;
                return true;
            }
        }
        chunk->kind = TibLegacyChunkKind::End;
        failed_ = false;
        return true;
    } catch (const std::bad_alloc&) { return fail(error, "TIB legacy allocation failed"); }
      catch (const std::length_error&) { return fail(error, "TIB legacy size is not representable"); }
}

bool openTibLegacyPrimaryStream(TibLegacyFileChunks* chunks, ByteStorePtr* stream,
                                std::string* error) {
    if (error) error->clear();
    if (!chunks || !stream) return fail(error, "Missing TIB legacy stream destination");
    stream->reset();
    try {
        for (;;) {
            TibLegacyChunk chunk = {};
            if (!chunks->next(&chunk, error)) return false;
            if (chunk.kind == TibLegacyChunkKind::End)
                return fail(error, "TIB legacy primary stream not found");
            if (chunk.kind == TibLegacyChunkKind::Stream4) {
                *stream = std::move(chunk.stream);
                return true;
            }
            if (chunk.kind == TibLegacyChunkKind::Stream2)
                return openBackupStream(chunk.stream, stream, error);
        }
    } catch (const std::bad_alloc&) { return fail(error, "TIB legacy allocation failed"); }
      catch (const std::length_error&) { return fail(error, "TIB legacy size is not representable"); }
}

bool readTibLegacyItem(TibLegacyFileChunks* chunks, TibLegacyItem* item, std::string* error) {
    if (error) error->clear();
    if (!chunks || !item) return fail(error, "Missing TIB legacy item destination");
    *item = TibLegacyItem{};
    try {
        bool file = false, directory = false, special = false;
        TibLegacyItem result = {};
        ByteStorePtr content;
        for (;;) {
            TibLegacyChunk chunk = {};
            if (!chunks->next(&chunk, error)) return false;
            if (chunk.kind == TibLegacyChunkKind::End) break;
            if (chunk.kind == TibLegacyChunkKind::Hash)
                return fail(error, "TIB legacy hash requires unsupported provider processing");
            if (chunk.kind == TibLegacyChunkKind::Attributes1 && chunk.directoryAttribute) directory = true;
            if (chunk.kind == TibLegacyChunkKind::Metadata6) {
                if (chunk.internalType == 128) directory = true;
                else if (chunk.internalType == 0 || chunk.internalType == 6) file = true;
                else {
                    if (special || file || directory) return fail(error, "Conflicting TIB legacy item metadata");
                    special = true;
                    switch (chunk.internalType) {
                    case 1: case 2:
                        if (chunk.structureSize < 28 || chunk.metadata.size() < 28)
                            return fail(error, "Truncated TIB legacy device metadata");
                        result.kind = chunk.internalType == 1 ? TibLegacyItemKind::CharacterDevice : TibLegacyItemKind::BlockDevice;
                        result.deviceMajor = le32(chunk.metadata.data() + 20);
                        result.deviceMinor = le32(chunk.metadata.data() + 24);
                        break;
                    case 3: result.kind = TibLegacyItemKind::Fifo; break;
                    case 4: result.kind = TibLegacyItemKind::Socket; break;
                    case 5: {
                        if (chunk.structureSize < 22 || chunk.metadata.size() < 22)
                            return fail(error, "Truncated TIB legacy symlink metadata");
                        const std::size_t units = std::size_t(chunk.metadata[20]) | (std::size_t(chunk.metadata[21]) << 8);
                        if (units > (chunk.structureSize - 22) / 2 || units > (chunk.metadata.size() - 22) / 2)
                            return fail(error, "Truncated TIB legacy symlink target");
                        result.linkTarget.reserve(units);
                        for (std::size_t i = 0; i < units; ++i)
                            result.linkTarget.push_back(static_cast<char16_t>(std::uint16_t(chunk.metadata[22 + i * 2]) |
                                (std::uint16_t(chunk.metadata[23 + i * 2]) << 8)));
                        result.kind = TibLegacyItemKind::Symlink;
                        break;
                    }
                    default: return fail(error, "Unsupported TIB legacy special item type");
                    }
                }
            }
            if (chunk.kind == TibLegacyChunkKind::Stream2 || chunk.kind == TibLegacyChunkKind::Stream4) {
                file = true;
                if (!content) {
                    if (chunk.kind == TibLegacyChunkKind::Stream2) {
                        if (!openBackupStream(chunk.stream, &content, error)) return false;
                    } else content = chunk.stream;
                }
            }
            if ((file && directory) || (special && (file || directory)))
                return fail(error, "Conflicting TIB legacy item metadata");
        }
        if (directory) result.kind = TibLegacyItemKind::Directory;
        else if (!special) {
            if (!content) content = std::make_shared<MemoryStore>(std::vector<std::uint8_t>());
            result.kind = TibLegacyItemKind::File;
            result.stream = std::move(content);
        }
        *item = std::move(result);
        return true;
    } catch (const std::bad_alloc&) { return fail(error, "TIB legacy allocation failed"); }
      catch (const std::length_error&) { return fail(error, "TIB legacy size is not representable"); }
}

} // namespace fs
} // namespace peare
