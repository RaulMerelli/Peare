#include "TibMacReader.h"

#include "TibHeader.h"
#include "../../common/compression/DeflateDecoder.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

namespace peare {
namespace fs {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

std::uint32_t le24(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16);
}

bool readExact(const ByteStorePtr& source, std::uint64_t offset, std::uint8_t* data, std::size_t size) {
    if (!source || !source->validate() || source->capacity() < 0 ||
        offset > static_cast<std::uint64_t>(source->capacity()) ||
        size > static_cast<std::uint64_t>(source->capacity()) - offset) return false;
    while (size) {
        const int wanted = static_cast<int>(std::min<std::size_t>(size, 65536));
        const int n = source->read(static_cast<std::int64_t>(offset), data, wanted);
        if (n <= 0 || n > wanted) return false;
        offset += static_cast<std::uint64_t>(n);
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return source->validate();
}

class RecordInput final : public compression::InflateInput {
public:
    RecordInput(const ByteStorePtr& source, std::uint64_t offset, std::size_t size)
        : source_(source), offset_(offset), size_(size), position_(0), buffered_(0), cursor_(0) {}
    bool readByte(std::uint8_t* value) override {
        if (position_ == size_) return false;
        if (cursor_ == buffered_) {
            buffered_ = std::min<std::size_t>(sizeof(buffer_), size_ - position_);
            if (!readExact(source_, offset_ + position_, buffer_, buffered_)) return false;
            cursor_ = 0;
        }
        *value = buffer_[cursor_++];
        ++position_;
        return true;
    }
    std::size_t consumed() const override { return position_; }
    bool rewind(std::size_t count) override {
        if (count > position_) return false;
        position_ -= count;
        buffered_ = cursor_ = 0;
        return true;
    }
private:
    ByteStorePtr source_;
    std::uint64_t offset_;
    std::size_t size_;
    std::size_t position_;
    std::uint8_t buffer_[4096];
    std::size_t buffered_;
    std::size_t cursor_;
};

class TibMacHandleStore final : public IByteStore {
public:
    TibMacHandleStore(ByteStorePtr data, std::uint64_t offset, TibMacHandle handle)
        : data_(std::move(data)), offset_(offset), handle_(handle) {}
    std::int64_t capacity() const override { return handle_.size; }
    bool validate() const override { return data_ && data_->validate(); }
    bool cheapRandomAccess() const override { return false; }
    int read(std::int64_t position, std::uint8_t* dst, int count) const override {
        if (position < 0 || count <= 0 || position >= capacity()) return 0;
        if (!dst || !validate() || handle_.start < 5) return -1;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, capacity() - position));
        std::uint8_t header[4], trailer[4];
        if (!readExact(data_, offset_, header, 4)) return -1;
        const std::uint32_t recordSize = le24(header + 1);
        if (recordSize < 10 || !readExact(data_, offset_ + recordSize, trailer, 4) ||
            trailer[0] != 0x78 || le24(trailer + 1) != recordSize)
            return -1;
        RecordInput compressed(data_, offset_ + 4, recordSize - 4);
        const std::uint64_t boxStart = std::uint64_t(handle_.start) - 5;
        const std::uint64_t blobStart = boxStart + 9;
        const std::uint64_t wantedStart = blobStart + static_cast<std::uint64_t>(position);
        std::vector<std::uint8_t> pending;
        std::uint8_t box[9] = {};
        std::uint64_t out = 0;
        std::uint64_t boxEnd = 0;
        bool validBox = false;
        const auto consume = [&](std::uint8_t byte) -> bool {
            if (out >= boxStart && out - boxStart < sizeof(box)) {
                box[static_cast<std::size_t>(out - boxStart)] = byte;
                if (out - boxStart == sizeof(box) - 1) {
                    const std::uint32_t size = le24(box + 1);
                    if (size < 5 || tibheader::le16(box + 4) != 0x04c0 || handle_.size > size - 5)
                        return false;
                    boxEnd = boxStart + 4 + size;
                    validBox = true;
                }
            }
            if (out >= wantedStart && out - wantedStart < static_cast<std::uint64_t>(amount))
                pending.push_back(byte);
            if (out == std::numeric_limits<std::uint64_t>::max()) return false;
            ++out;
            return true;
        };
        if (!compression::inflateZlibStreaming(compressed, recordSize - 4, consume, nullptr) ||
            !validBox || out < boxEnd || pending.size() != static_cast<std::size_t>(amount) || !validate()) return -1;
        std::copy(pending.begin(), pending.end(), dst);
        return amount;
    }
private:
    ByteStorePtr data_;
    std::uint64_t offset_;
    TibMacHandle handle_;
};

class TibMacFileStore final : public IByteStore {
public:
    TibMacFileStore(ByteStorePtr data, std::vector<ByteStorePtr> parts, std::vector<std::int64_t> starts,
                    std::int64_t size)
        : data_(std::move(data)), parts_(std::move(parts)), starts_(std::move(starts)), size_(size) {}
    std::int64_t capacity() const override { return size_; }
    bool validate() const override {
        if (!data_ || !data_->validate()) return false;
        for (const auto& part : parts_) if (!part || !part->validate()) return false;
        return true;
    }
    bool cheapRandomAccess() const override { return false; }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= size_) return 0;
        if (!dst || !validate()) return -1;
        try {
            const int amount = static_cast<int>(std::min<std::int64_t>(
                std::min<std::int64_t>(count, size_ - pos), 1024 * 1024));
            std::vector<std::uint8_t> pending(static_cast<std::size_t>(amount));
            int done = 0;
            for (std::size_t i = 0; i < parts_.size() && done < amount; ++i) {
                const std::int64_t cursor = pos + done;
                const std::int64_t end = starts_[i] + parts_[i]->capacity();
                if (cursor < starts_[i] || cursor >= end) continue;
                const int wanted = static_cast<int>(std::min<std::int64_t>(amount - done, end - cursor));
                if (!readExact(parts_[i], static_cast<std::uint64_t>(cursor - starts_[i]),
                               pending.data() + done, static_cast<std::size_t>(wanted))) return -1;
                done += wanted;
            }
            if (done != amount || !validate()) return -1;
            std::copy(pending.begin(), pending.end(), dst);
            return amount;
        } catch (const std::exception&) { return -1; }
    }
private:
    ByteStorePtr data_;
    std::vector<ByteStorePtr> parts_;
    std::vector<std::int64_t> starts_;
    std::int64_t size_;
};

} // namespace

bool linkTibMacSlices(std::vector<TibMacSlice>* slices, std::string* error) {
    if (!slices || slices->size() > std::size_t(std::numeric_limits<std::int64_t>::max()))
        return fail(error, "Invalid Mac TIB slice count");
    for (std::size_t i = 0; i < slices->size(); ++i)
        (*slices)[i].parent = i ? static_cast<std::int64_t>(i - 1) : -1;
    if (error) error->clear();
    return true;
}

bool buildTibMacTree(std::vector<TibMacTreeEntry>* entries, std::string* error) {
    if (!entries || entries->size() > std::size_t(std::numeric_limits<std::int64_t>::max()))
        return fail(error, "Invalid Mac TIB entry count");
    struct Ancestor { std::size_t index; std::size_t end; };
    std::vector<Ancestor> ancestors;
    std::vector<std::int64_t> parents;
    parents.reserve(entries->size());
    for (std::size_t i = 0; i < entries->size(); ++i) {
        while (!ancestors.empty() && i == ancestors.back().end) ancestors.pop_back();
        parents.push_back(ancestors.empty() ? -1 : static_cast<std::int64_t>(ancestors.back().index));
        const TibMacTreeEntry& item = (*entries)[i];
        if (item.item.type != 0x80 && item.item.type != 0x83) continue;
        const std::size_t limit = ancestors.empty() ? entries->size() : ancestors.back().end;
        if (item.numEntries > limit - i - 1) return fail(error, "Invalid Mac TIB directory subtree");
        const std::size_t end = i + 1 + static_cast<std::size_t>(item.numEntries);
        if (end > i + 1) ancestors.push_back(Ancestor{i, end});
    }
    for (std::size_t i = 0; i < entries->size(); ++i) (*entries)[i].parent = parents[i];
    if (error) error->clear();
    return true;
}

bool parseTibMacHandleGroups(const std::uint8_t* bytes, std::size_t size,
                             std::vector<TibMacHandle>* dataHandles, std::string* error) {
    if (!dataHandles || (size && !bytes)) return fail(error, "Invalid Mac TIB handle groups");
    std::vector<TibMacHandle> result;
    std::size_t pos = 0;
    while (pos < size) {
        if (size - pos < 8) return fail(error, "Truncated Mac TIB handle group");
        const std::uint32_t type = tibheader::le32(bytes + pos);
        const std::uint32_t length = tibheader::le32(bytes + pos + 4);
        pos += 8;
        if (length > size - pos || size - pos - length < 4)
            return fail(error, "Invalid Mac TIB handle group length");
        std::vector<TibMacHandle> group;
        if (!parseTibMacHandleDescriptors(bytes + pos, length, &group, error)) return false;
        if (type == 4) result.insert(result.end(), group.begin(), group.end());
        pos += static_cast<std::size_t>(length) + 4;
    }
    *dataHandles = std::move(result);
    if (error) error->clear();
    return true;
}

ByteStorePtr openTibMacFile(const ByteStorePtr& data, std::uint32_t blockSize,
                            const std::vector<TibMacHandle>& handles,
                            std::uint64_t fileSize, std::string* error) {
    if (!data || !data->validate() || fileSize > std::uint64_t(std::numeric_limits<std::int64_t>::max())) {
        fail(error, "Mac TIB file size exceeds store capacity");
        return ByteStorePtr();
    }
    std::vector<ByteStorePtr> parts;
    std::vector<std::int64_t> starts;
    std::uint64_t total = 0;
    for (const TibMacHandle& handle : handles) {
        if (total > fileSize || handle.size > fileSize - total) {
            fail(error, "Mac TIB handle sizes exceed file size");
            return ByteStorePtr();
        }
        ByteStorePtr part = openTibMacHandle(data, blockSize, handle, error);
        if (!part) return ByteStorePtr();
        starts.push_back(static_cast<std::int64_t>(total));
        parts.push_back(std::move(part));
        total += handle.size;
    }
    if (total != fileSize) {
        fail(error, "Mac TIB handle sizes do not cover file");
        return ByteStorePtr();
    }
    if (error) error->clear();
    return std::make_shared<TibMacFileStore>(data, std::move(parts), std::move(starts),
                                             static_cast<std::int64_t>(fileSize));
}

bool parseTibMacEntryName(const std::uint8_t* bytes, std::size_t size,
                          TibMacEntryName* entry, std::string* error) {
    if (!bytes || !entry) return fail(error, "Invalid Mac TIB entry");
    std::size_t pos = 0;
    const auto take = [&](std::size_t count) -> bool {
        if (count > size - pos) return false;
        pos += count;
        return true;
    };
    const auto read64 = [&](std::uint64_t* value) -> bool {
        if (!take(8)) return false;
        *value = std::uint64_t(tibheader::le32(bytes + pos - 8)) |
                 (std::uint64_t(tibheader::le32(bytes + pos - 4)) << 32);
        return true;
    };
    if (!take(2)) return fail(error, "Truncated Mac TIB filename size");
    const std::uint16_t units = tibheader::le16(bytes);
    TibMacEntryName parsed;
    if (!take(2) || !take(1)) return fail(error, "Truncated Mac TIB entry type");
    parsed.type = bytes[pos - 1];
    if (!take(3) || !read64(&parsed.size) || !take(8) ||
        !read64(&parsed.mtime) || !read64(&parsed.atime) || !take(4))
        return fail(error, "Truncated Mac TIB entry fields");
    if (std::size_t(units) * 2 != size - pos) return fail(error, "Invalid Mac TIB filename bounds");
    parsed.name.reserve(units);
    for (std::size_t i = 0; i < units; ++i)
        parsed.name.push_back(static_cast<char16_t>(tibheader::le16(bytes + pos + i * 2)));
    *entry = std::move(parsed);
    if (error) error->clear();
    return true;
}

bool parseTibMacHandleDescriptors(const std::uint8_t* bytes, std::size_t size,
                                  std::vector<TibMacHandle>* handles, std::string* error) {
    if (!handles || (size && !bytes) || size % 20) return fail(error, "Invalid Mac TIB handle descriptors");
    std::vector<TibMacHandle> parsed;
    for (std::size_t pos = 0; pos < size; pos += 20) {
        TibMacHandle handle;
        handle.recordStart = std::uint64_t(tibheader::le32(bytes + pos)) |
                             (std::uint64_t(tibheader::le32(bytes + pos + 4)) << 32);
        handle.start = tibheader::le32(bytes + pos + 8);
        handle.size = tibheader::le32(bytes + pos + 12);
        handle.recordSize = tibheader::le32(bytes + pos + 16);
        parsed.push_back(handle);
    }
    *handles = std::move(parsed);
    if (error) error->clear();
    return true;
}

ByteStorePtr openTibMacHandle(const ByteStorePtr& data, std::uint32_t blockSize,
                              const TibMacHandle& handle, std::string* error) {
    const std::uint64_t base = std::uint64_t(blockSize) * 3;
    if (!data || !data->validate() || data->capacity() < 0 || handle.start < 5 ||
        handle.recordStart > std::uint64_t(data->capacity()) ||
        base > std::uint64_t(data->capacity()) - handle.recordStart) {
        fail(error, "Invalid Mac TIB handle position");
        return ByteStorePtr();
    }
    if (error) error->clear();
    return std::make_shared<TibMacHandleStore>(data, base + handle.recordStart, handle);
}

namespace {

bool parseMacEntry(const std::vector<std::uint8_t>& body, std::size_t* cursor,
                   TibMacTreeEntry* entry, std::string* error) {
    std::size_t pos = *cursor;
    const std::size_t limit = body.size();
    if (pos > limit || limit - pos < 4) return fail(error, "Truncated Mac TIB entry prefix");
    const std::uint32_t prefixSize = tibheader::le32(body.data() + pos);
    pos += 4;
    if (prefixSize > limit - pos) return fail(error, "Invalid Mac TIB entry prefix length");
    pos += prefixSize;
    if (limit - pos < 4) return fail(error, "Truncated Mac TIB entry length");
    const std::uint32_t restSize = tibheader::le32(body.data() + pos);
    if (restSize < 4 || restSize > limit - pos) return fail(error, "Invalid Mac TIB entry length");
    const std::size_t end = pos + restSize;
    pos += 4;
    if (end - pos < 5) return fail(error, "Truncated Mac TIB entry preamble");
    if (body[pos + 4] == 0x40) {
        if (end - pos < 12) return fail(error, "Truncated Mac TIB entry extended preamble");
        pos += 10;
        const std::uint16_t n = tibheader::le16(body.data() + pos);
        pos += 2;
        const std::size_t skip = std::size_t(6) + std::size_t(n) * 2 + 4;
        if (skip > end - pos) return fail(error, "Invalid Mac TIB entry extended preamble");
        pos += skip;
    } else {
        if (end - pos < 8) return fail(error, "Truncated Mac TIB entry preamble");
        pos += 8;
    }
    if (end - pos < 2) return fail(error, "Truncated Mac TIB name length");
    const std::uint16_t firstSize = tibheader::le16(body.data() + pos);
    if (firstSize < 2 || firstSize > end - pos) return fail(error, "Invalid Mac TIB name length");
    const std::size_t nameEnd = pos + firstSize;
    TibMacTreeEntry parsed;
    parsed.parent = -1;
    parsed.numEntries = 0;
    if (!parseTibMacEntryName(body.data() + pos + 2, firstSize - 2, &parsed.item, error)) return false;
    if (parsed.item.type == 0x80 || parsed.item.type == 0x83) {
        if (end - nameEnd < 49) return fail(error, "Truncated Mac TIB directory count");
        parsed.numEntries = tibheader::le32(body.data() + nameEnd + 45);
    }
    std::size_t skip = 0;
    if (parsed.item.type == 0) skip = 35;
    else if (parsed.item.type == 0x80) skip = 85;
    else if (parsed.item.type == 0x83) skip = 41;
    else return fail(error, "Unsupported Mac TIB entry type");
    if (skip > end - nameEnd || end - nameEnd - skip < 4)
        return fail(error, "Truncated Mac TIB entry handles");
    pos = nameEnd + skip;
    const std::uint32_t handleSize = tibheader::le32(body.data() + pos);
    pos += 4;
    if (handleSize > end - pos) return fail(error, "Invalid Mac TIB entry handle size");
    if (!parseTibMacHandleGroups(body.data() + pos, handleSize, &parsed.handles, error)) return false;
    *entry = std::move(parsed);
    *cursor = end;
    return true;
}

bool parseMacMetadata(const std::vector<std::uint8_t>& box, TibMacSlice* slice,
                      std::string* error) {
    if (box.size() < 6 || tibheader::le16(box.data() + 4) != 0x0040)
        return fail(error, "Invalid Mac TIB metadata box");
    const std::size_t bodyStart = 6;
    if (box.size() - bodyStart < 3) return fail(error, "Truncated Mac TIB metadata");
    const std::size_t innerStart = bodyStart + 3;
    if (box.size() - innerStart < 53 + 16 + 8 || box.size() - innerStart < 100)
        return fail(error, "Truncated Mac TIB metadata header");
    const std::size_t idPos = innerStart + 53;
    TibMacSlice parsed;
    parsed.parent = -1;
    std::copy(box.begin() + idPos, box.begin() + idPos + 16, parsed.id.begin());
    parsed.timestamp = std::uint64_t(tibheader::le32(box.data() + idPos + 16)) |
        (std::uint64_t(tibheader::le32(box.data() + idPos + 20)) << 32);
    static const std::uint8_t xml[] = {'<', 0, '?', 0, 'x', 0, 'm', 0, 'l', 0};
    std::size_t xmlPos = innerStart + 100;
    while (xmlPos <= box.size() && box.size() - xmlPos >= sizeof(xml) &&
           !std::equal(xml, xml + sizeof(xml), box.data() + xmlPos)) ++xmlPos;
    if (xmlPos < 2 || xmlPos > box.size() || box.size() - xmlPos < sizeof(xml))
        return fail(error, "Mac TIB metadata XML not found");
    const std::uint16_t xmlSize = tibheader::le16(box.data() + xmlPos - 2);
    if ((xmlSize & 1) || xmlSize < sizeof(xml) || xmlSize > box.size() - xmlPos)
        return fail(error, "Invalid Mac TIB metadata XML length");
    std::size_t pos = xmlPos + xmlSize;
    if (pos == box.size()) return fail(error, "Truncated Mac TIB metadata after XML");
    const std::size_t skip = box[pos] ? 52 : 8;
    if (skip > box.size() - pos) return fail(error, "Truncated Mac TIB metadata preamble");
    pos += skip;
    while (pos < box.size()) {
        TibMacTreeEntry item;
        if (!parseMacEntry(box, &pos, &item, error)) return false;
        parsed.entries.push_back(std::move(item));
    }
    if (!buildTibMacTree(&parsed.entries, error)) return false;
    *slice = std::move(parsed);
    return true;
}

} // namespace

bool inspectTibMac(const ByteStorePtr& data, const ByteStorePtr& companion,
                   TibMacInspection* result, std::string* error) {
    if (!result) return fail(error, "Missing Mac TIB inspection destination");
    std::uint8_t prefix[32];
    if (!readExact(data, 0, prefix, sizeof(prefix))) return fail(error, "Cannot read Mac TIB header");
    const std::uint16_t headerSize = tibheader::le16(prefix + 4);
    if (headerSize < sizeof(prefix)) return fail(error, "Invalid Mac TIB header size");
    std::vector<std::uint8_t> headerBytes(headerSize);
    if (!readExact(data, 0, headerBytes.data(), headerBytes.size())) return fail(error, "Truncated Mac TIB header");
    tibheader::Header header;
    if (!tibheader::parse(headerBytes, false, &header, error)) return false;
    if (header.version != 1 || header.size < 32) return fail(error, "Invalid Mac TIB header version or size");
    std::uint8_t field[4];
    if (!readExact(data, 28, field, sizeof(field))) return fail(error, "Cannot read Mac TIB block size");
    const std::uint32_t blockSize = tibheader::le32(field);
    if (std::uint64_t(blockSize) > std::uint64_t(INT64_MAX) / 3)
        return fail(error, "Invalid Mac TIB block size");

    std::uint8_t frame[4];
    if (!readExact(companion, 16, frame, sizeof(frame))) return fail(error, "Cannot read Mac TIB companion record");
    const std::uint32_t size = le24(frame + 1);
    if (frame[0] != 0x88 || size < 4) return fail(error, "Invalid Mac TIB companion record header");
    if (!readExact(companion, 16 + std::uint64_t(size), frame, sizeof(frame)))
        return fail(error, "Truncated Mac TIB companion record");
    if (frame[0] != 0x78 || le24(frame + 1) != size)
        return fail(error, "Invalid Mac TIB companion trailer");
    const std::uint32_t trailerSize = le24(frame + 1);
    if (size < 10) return fail(error, "Truncated Mac TIB zlib stream");
    RecordInput compressed(companion, 20, size - 4);
    std::vector<std::uint8_t> decoded;
    std::size_t wanted = 8;
    const auto retainFirstBox = [&decoded, &wanted](std::uint8_t byte) -> bool {
        if (decoded.size() < wanted) {
            decoded.push_back(byte);
            if (decoded.size() == 8) {
                const std::uint32_t boxSize = le24(decoded.data() + 5);
                if (boxSize == 1) return false;
                wanted += boxSize;
            }
        }
        return true;
    };
    if (!compression::inflateZlibStreaming(compressed, size - 4, retainFirstBox, error)) return false;
    if (decoded.size() != wanted) return fail(error, "Truncated Mac TIB companion chunk or box");
    const std::uint32_t boxSize = le24(decoded.data() + 5);

    TibMacInspection inspected;
    inspected.blockSize = blockSize;
    inspected.recordOffset = std::uint64_t(blockSize) * 3;
    inspected.companionRecordSize = size;
    inspected.trailerSize = trailerSize;
    inspected.firstChunkSize = tibheader::le32(decoded.data());
    inspected.firstBoxSize = boxSize;
    inspected.firstBoxMagic = decoded[4];
    inspected.firstBoxType = boxSize ? tibheader::le16(decoded.data() + 8) : 0;
    *result = inspected;
    if (error) error->clear();
    return true;
}

bool readTibMacIndex(const ByteStorePtr& data, const ByteStorePtr& companion,
                     std::uint32_t* blockSize, std::vector<TibMacSlice>* slices,
                     std::string* error) {
    if (!blockSize || !slices || !companion || !companion->validate() || companion->capacity() < 16)
        return fail(error, "Invalid Mac TIB index source");
    TibMacInspection inspection;
    if (!inspectTibMac(data, companion, &inspection, error)) return false;
    std::vector<TibMacSlice> parsed;
    std::uint64_t offset = 16;
    const std::uint64_t capacity = static_cast<std::uint64_t>(companion->capacity());
    while (offset < capacity) {
        std::uint8_t header[4], trailer[4];
        if (!readExact(companion, offset, header, 4) || header[0] != 0x88)
            return fail(error, "Invalid Mac TIB index record header");
        const std::uint32_t frameSize = le24(header + 1);
        if (frameSize < 10 || frameSize > capacity - offset - 4 ||
            !readExact(companion, offset + frameSize, trailer, 4) ||
            trailer[0] != 0x78 || le24(trailer + 1) != frameSize)
            return fail(error, "Invalid Mac TIB index record framing");
        RecordInput input(companion, offset + 4, frameSize - 4);
        std::uint8_t lengthBytes[4] = {};
        std::size_t lengthPos = 0;
        std::uint32_t chunkSize = 0;
        std::vector<std::uint8_t> chunk;
        std::vector<std::vector<std::uint8_t>> metadata;
        bool invalid = false;
        const auto consume = [&](std::uint8_t byte) -> bool {
            if (lengthPos < 4) {
                lengthBytes[lengthPos++] = byte;
                if (lengthPos == 4) {
                    chunkSize = tibheader::le32(lengthBytes);
                    if (chunkSize < 4 || chunkSize > 0x00ffffffU + 4U) { invalid = true; return false; }
                    chunk.clear();
                }
                return true;
            }
            chunk.push_back(byte);
            if (chunk.size() == 4) {
                const std::uint32_t boxSize = le24(chunk.data() + 1);
                if (boxSize == 1 || chunkSize != boxSize + 4) { invalid = true; return false; }
            }
            if (chunk.size() == chunkSize) {
                if (chunkSize >= 6 && tibheader::le16(chunk.data() + 4) == 0x0040)
                    metadata.push_back(std::move(chunk));
                lengthPos = 0;
                chunk.clear();
            }
            return true;
        };
        if (!compression::inflateZlibStreaming(input, frameSize - 4, consume, error)) {
            if (invalid) return fail(error, "Invalid Mac TIB index chunk");
            return false;
        }
        if (lengthPos || !chunk.empty()) return fail(error, "Truncated Mac TIB index chunk");
        for (const auto& box : metadata) {
            TibMacSlice slice;
            if (!parseMacMetadata(box, &slice, error)) return false;
            parsed.push_back(std::move(slice));
        }
        offset += std::uint64_t(frameSize) + 4;
    }
    if (parsed.empty()) return fail(error, "Mac TIB metadata slice not found");
    if (!linkTibMacSlices(&parsed, error)) return false;
    if (!data->validate() || !companion->validate()) return fail(error, "Mac TIB source became invalid");
    *blockSize = inspection.blockSize;
    *slices = std::move(parsed);
    if (error) error->clear();
    return true;
}

} // namespace fs
} // namespace peare
