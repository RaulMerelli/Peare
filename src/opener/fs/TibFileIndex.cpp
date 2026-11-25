#include "TibFileIndex.h"
#include "TibHeader.h"
#include "TibLegacyFileTree.h"

#include "../../common/compression/DeflateDecoder.h"
#include "../compression/Crc32.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::size_t kScanBlock = 64U * 1024U;

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool readExact(const ByteStorePtr& source, std::int64_t offset,
               std::uint8_t* data, int size) {
    if (!source || offset < 0 || size < 0 || offset > source->capacity() ||
        size > source->capacity() - offset) return false;
    int done = 0;
    while (done < size) {
        const int n = source->read(offset + done, data + done, size - done);
        if (n <= 0 || n > size - done) return false;
        done += n;
    }
    return true;
}

class StoreInput final : public compression::InflateInput {
public:
    StoreInput(ByteStorePtr source, std::int64_t begin, std::int64_t end)
        : source_(std::move(source)), begin_(begin), end_(end), position_(0),
          bufferStart_(-1), bufferSize_(0), failed_(false) {}

    bool readByte(std::uint8_t* value) override {
        if (!value || position_ >= end_ - begin_) return false;
        const std::int64_t absolute = begin_ + position_;
        if (bufferStart_ < 0 || absolute < bufferStart_ ||
            absolute - bufferStart_ >= bufferSize_) {
            bufferStart_ = absolute;
            bufferSize_ = static_cast<int>(std::min<std::int64_t>(
                static_cast<std::int64_t>(buffer_.size()), end_ - absolute));
            if (!readExact(source_, bufferStart_, buffer_.data(), bufferSize_)) {
                bufferSize_ = 0;
                failed_ = true;
                return false;
            }
        }
        *value = buffer_[static_cast<std::size_t>(absolute - bufferStart_)];
        ++position_;
        return true;
    }

    std::size_t consumed() const override { return static_cast<std::size_t>(position_); }
    bool failed() const { return failed_; }
    bool rewind(std::size_t count) override {
        if (count > static_cast<std::size_t>(position_)) return false;
        position_ -= static_cast<std::int64_t>(count);
        return true;
    }

private:
    ByteStorePtr source_;
    std::int64_t begin_;
    std::int64_t end_;
    std::int64_t position_;
    std::int64_t bufferStart_;
    int bufferSize_;
    bool failed_;
    std::array<std::uint8_t, 4096> buffer_;
};

bool readRecord(const ByteStorePtr& source, std::int64_t start,
                std::int64_t end, std::uint8_t tag,
                std::vector<std::uint8_t>* output, std::int64_t* next,
                bool* fatal, std::string* error) {
    std::uint8_t actual = 0;
    if (start >= end)
        return fail(error, "Invalid TIB index record tag");
    if (!readExact(source, start, &actual, 1)) {
        *fatal = true;
        return fail(error, "Cannot read TIB index record tag");
    }
    if (actual != tag) return fail(error, "Invalid TIB index record tag");
    if (std::uint64_t(end - start - 1) > std::numeric_limits<std::size_t>::max()) {
        *fatal = true;
        return fail(error, "TIB compressed record is not representable");
    }
    StoreInput input(source, start + 1, end);
    std::string inflateError;
    if (!compression::inflateRawStream(input, output->max_size(), output, &inflateError)) {
        if (input.failed() || inflateError == "Deflate allocation failed" ||
            inflateError == "Deflate output exceeds limit") *fatal = true;
        if (error) *error = "Invalid TIB index Deflate stream: " + inflateError;
        return false;
    }
    if (input.failed()) {
        *fatal = true;
        return fail(error, "Cannot read TIB index Deflate stream");
    }
    const std::int64_t crcOffset = start + 1 + static_cast<std::int64_t>(input.consumed());
    std::uint8_t crc[4];
    if (crcOffset > end || end - crcOffset < 4)
        return fail(error, "Truncated TIB index checksum");
    if (!readExact(source, crcOffset, crc, 4)) {
        *fatal = true;
        return fail(error, "Cannot read TIB index checksum");
    }
    if (compression::crc32(output->data(), output->size()) != le32(crc))
        return fail(error, "TIB index checksum mismatch");
    *next = crcOffset + 4;
    return true;
}

bool parseEntries(const std::vector<std::uint8_t>& bytes,
                  std::vector<TibFileIndexEntry>* entries, std::string* error) {
    if (bytes.size() < 4) return fail(error, "Truncated TIB index count");
    const std::uint32_t count = le32(bytes.data());
    if (count > (bytes.size() - 4) / 10)
        return fail(error, "Invalid TIB index count");
    std::vector<TibFileIndexEntry> result;
    if (count > result.max_size()) throw std::length_error("TIB index count");
    result.reserve(count);
    std::size_t pos = 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (bytes.size() - pos < 4) return fail(error, "Truncated TIB path length");
        const std::uint32_t units = le32(bytes.data() + pos);
        pos += 4;
        if (!units || units > (bytes.size() - pos) / 2)
            return fail(error, "Invalid TIB path length");
        TibFileIndexEntry entry;
        entry.path.reserve(units - 1);
        for (std::uint32_t n = 0; n < units; ++n) {
            const std::uint16_t c = std::uint16_t(bytes[pos]) |
                                    (std::uint16_t(bytes[pos + 1]) << 8);
            pos += 2;
            if (n + 1 == units) {
                if (c != 0) return fail(error, "Unterminated TIB path");
            } else {
                if (!c) return fail(error, "Embedded NUL in TIB path");
                entry.path.push_back(static_cast<char16_t>(c));
            }
        }
        if (bytes.size() - pos < 4) return fail(error, "Truncated TIB metadata length");
        const std::uint32_t metadataUnits = le32(bytes.data() + pos);
        pos += 4;
        if (metadataUnits > (bytes.size() - pos) / 2)
            return fail(error, "Invalid TIB metadata length");
        const std::size_t length = static_cast<std::size_t>(metadataUnits) * 2;
        entry.metadata.assign(bytes.begin() + pos, bytes.begin() + pos + length);
        pos += length;
        result.push_back(std::move(entry));
    }
    if (pos != bytes.size()) return fail(error, "Trailing data in TIB index");
    *entries = std::move(result);
    return true;
}

} // namespace

bool readTibFileGRecord(const ByteStorePtr& source, std::int64_t start,
                        std::int64_t limit, std::vector<std::uint8_t>* decoded,
                        std::int64_t* recordEnd, bool* fatal, std::string* error) {
    if (error) error->clear();
    if (!decoded || !recordEnd || !fatal) return fail(error, "Missing TIB g record output");
    *fatal = false;
    if (!source || start < 0 || limit < start || limit > source->capacity())
        return fail(error, "Invalid TIB g record bounds");
    std::vector<std::uint8_t> result;
    std::int64_t next = 0;
    if (!readRecord(source, start, limit, 0x67, &result, &next, fatal, error)) return false;
    *decoded = std::move(result);
    *recordEnd = next;
    return true;
}

bool parseTibFileGIndex(const std::vector<std::uint8_t>& decoded,
                        std::vector<TibFileIndexEntry>* entries, std::string* error) {
    if (error) error->clear();
    if (!entries) return fail(error, "Missing TIB index output");
    return parseEntries(decoded, entries, error);
}

bool parseTibFileMetadata(const std::vector<std::uint8_t>& decoded,
                          TibFileMetadata* metadata, std::string* error) {
    if (error) error->clear();
    if (!metadata) return fail(error, "Missing TIB metadata output");
    if (decoded.size() < 8) return fail(error, "Truncated TIB metadata version");
    TibFileMetadata result;
    result.version = TibFileMetadataVersion{le32(decoded.data()), le32(decoded.data() + 4)};
    result.hasPositionA = false;
    result.positionA = 0;
    result.hasPositionB = false;
    result.positionB = 0;
    result.hasEncryptionVector = false;
    result.versionFields.clear();
    std::size_t pos = 8;
    const auto take = [&](std::size_t size) -> bool {
        if (size > decoded.size() - pos) return false;
        pos += size;
        return true;
    };
    if (!take(32 + 16 + 4 + 8 + 8) || !take(4))
        return fail(error, "Truncated TIB metadata fields");
    const std::uint32_t units = le32(decoded.data() + pos - 4);
    if (units > (decoded.size() - pos) / 2 || !take(std::size_t(units) * 2) || !take(8))
        return fail(error, "Invalid TIB metadata string");
    const auto atLeast = [&](std::uint32_t major, std::uint32_t minor) -> bool {
        return result.version.major > major ||
               (result.version.major == major && result.version.minor >= minor);
    };
    if (atLeast(1, 1) && !take(4)) return fail(error, "Truncated TIB metadata fields");
    if (atLeast(1, 2)) {
        if (!take(8)) return fail(error, "Truncated TIB metadata position A");
        result.hasPositionA = true;
        result.positionA = le64(decoded.data() + pos - 8);
    }
    if (atLeast(3, 0)) {
        if (!take(8) || !take(8)) return fail(error, "Truncated TIB metadata position B");
        result.hasPositionB = true;
        result.positionB = le64(decoded.data() + pos - 8);
        if (!take(1)) return fail(error, "Truncated TIB encryption flag");
        result.hasEncryptionVector = decoded[pos - 1] != 0;
        if (result.hasEncryptionVector) {
            if (!take(4)) return fail(error, "Truncated TIB encryption vector length");
            const std::uint32_t length = le32(decoded.data() + pos - 4);
            if (length < 8 || length > decoded.size() - pos)
                return fail(error, "Invalid TIB encryption vector length");
            const std::uint8_t* vector = decoded.data() + pos;
            if (vector[0] != 1 || compression::crc32(vector, length) != 0xffffffffU)
                return fail(error, "Invalid TIB encryption vector or checksum");
            const std::size_t checksumStart = std::size_t(length) - 4;
            std::size_t record = 1;
            bool terminated = false;
            while (record < checksumStart) {
                if (vector[record] == 0xff) { terminated = true; break; }
                if (checksumStart - record < 2) return fail(error, "Truncated TIB encryption subrecord");
                const std::size_t recordLength = 6 + 4 * std::size_t(vector[record + 1]);
                if (recordLength > checksumStart - record)
                    return fail(error, "Truncated TIB encryption subrecord");
                record += recordLength;
            }
            if (!terminated) return fail(error, "Missing TIB encryption vector terminator");
            result.encryptionVector.assign(vector, vector + length);
            pos += length;
        }
    }
    if (atLeast(5, 0)) {
        for (std::size_t i = 0; i < 2; ++i) {
            if (!take(8)) return fail(error, "Truncated TIB version 5.0 fields");
            result.versionFields.push_back(le64(decoded.data() + pos - 8));
        }
    }
    if (atLeast(5, 1)) {
        if (!take(8)) return fail(error, "Truncated TIB version 5.1 field");
        result.versionFields.push_back(le64(decoded.data() + pos - 8));
    }
    result.decoded = decoded;
    *metadata = std::move(result);
    return true;
}

static bool findLocalTibFileIndexStart(const ByteStorePtr& source, std::uint64_t base,
                                       std::int64_t trailerStart, std::int64_t* first) {
    if (!source || !first || trailerStart < 0 || base >= static_cast<std::uint64_t>(trailerStart))
        return false;
    std::vector<std::uint8_t> block(kScanBlock);
    std::vector<std::uint8_t> metadataBytes;
    std::vector<std::uint8_t> firstMetadataBytes;
    std::vector<std::uint8_t> indexBytes;
    std::vector<TibFileIndexEntry> entries;
    std::vector<std::uint64_t> continuationStarts;
    std::int64_t selected = -1;
    std::int64_t offset = static_cast<std::int64_t>(base);
    while (offset < trailerStart) {
        const int length = static_cast<int>(std::min<std::int64_t>(
            static_cast<std::int64_t>(block.size()), trailerStart - offset));
        if (!readExact(source, offset, block.data(), length)) return false;
        for (int i = 0; i < length; ++i) {
            if (block[static_cast<std::size_t>(i)] != 0x65) continue;
            const std::int64_t candidate = offset + i;
            if (std::binary_search(continuationStarts.begin(), continuationStarts.end(),
                                   static_cast<std::uint64_t>(candidate))) continue;
            std::int64_t next = candidate;
            bool fatal = false;
            bool valid = true;
            metadataBytes.clear();
            firstMetadataBytes.clear();
            bool firstRecord = true;
            for (;;) {
                std::uint8_t tag = 0;
                if (next >= trailerStart || !readExact(source, next, &tag, 1) || tag != 0x65) break;
                std::int64_t recordEnd = 0;
                std::string ignored;
                if (!readRecord(source, next, trailerStart, 0x65, &metadataBytes,
                                &recordEnd, &fatal, &ignored)) {
                    valid = false;
                    break;
                }
                if (firstRecord) {
                    firstMetadataBytes = metadataBytes;
                    firstRecord = false;
                }
                next = recordEnd;
                if (next < trailerStart) {
                    std::uint8_t following = 0;
                    if (!readExact(source, next, &following, 1)) { valid = false; break; }
                    if (following == 0x65) {
                        const std::uint64_t continuation = static_cast<std::uint64_t>(next);
                        const auto position = std::lower_bound(continuationStarts.begin(),
                                                               continuationStarts.end(), continuation);
                        if (position == continuationStarts.end() || *position != continuation)
                            continuationStarts.insert(position, continuation);
                    }
                }
            }
            if (!valid || fatal || firstMetadataBytes.size() < 8 || next >= trailerStart) continue;
            const TibFileMetadataVersion version = {le32(firstMetadataBytes.data()),
                                                     le32(firstMetadataBytes.data() + 4)};
            TibFileMetadata parsed;
            if (!supportsTibFileMetadataVersion(version) ||
                !parseTibFileMetadata(firstMetadataBytes, &parsed, nullptr)) continue;
            std::uint8_t tag = 0;
            if (!readExact(source, next, &tag, 1) || tag != 0x67) continue;
            std::int64_t recordEnd = 0;
            bool indexFatal = false;
            std::string ignored;
            if (!readTibFileGRecord(source, next, trailerStart, &indexBytes, &recordEnd,
                                    &indexFatal, &ignored) || indexFatal || recordEnd != trailerStart ||
                !parseTibFileGIndex(indexBytes, &entries, nullptr)) continue;
            if (selected >= 0) return false;
            selected = candidate;
        }
        offset += length;
    }
    if (selected < 0) return false;
    *first = selected;
    return true;
}

static bool readTibFileRecords(const ByteStorePtr& source,
                               std::vector<TibFileIndexEntry>* entries,
                               std::uint64_t* dataOffset, TibFileMetadataVersion* metadataVersion,
                               TibFileMetadata* fileMetadata, bool metadataOnly,
                               TibFileIndexFallback* fallback,
                               std::string* error) {
    if (error) error->clear();
    if ((!metadataOnly && (!entries || !metadataVersion)) || !dataOffset ||
        (metadataOnly && !fileMetadata)) return fail(error, "Missing TIB index output");
    if (entries) entries->clear();
    if (fallback) {
        fallback->metadataAvailable = false;
        fallback->legacyTreeAvailable = false;
        fallback->legacyTree.clear();
        fallback->metadata = TibFileMetadata();
        fallback->metadataError.clear();
        fallback->dataOffset = 0;
        fallback->metadataStart = 0;
        fallback->metadataEnd = 0;
        fallback->trailerStart = 0;
    }
    if (fileMetadata) {
        fileMetadata->version = TibFileMetadataVersion{0, 0};
        fileMetadata->hasPositionA = false;
        fileMetadata->positionA = 0;
        fileMetadata->hasPositionB = false;
        fileMetadata->positionB = 0;
        fileMetadata->hasEncryptionVector = false;
        fileMetadata->encryptionVector.clear();
        fileMetadata->versionFields.clear();
        fileMetadata->decoded.clear();
    }
    *dataOffset = 0;
    if (metadataVersion) *metadataVersion = TibFileMetadataVersion{0, 0};
    if (!source || source->capacity() < 24) return fail(error, "Missing TIB file header");
    try {
    const std::int64_t size = source->capacity();
    tibheader::Header header;
    if (!tibheader::read(source, &header, error)) return false;
    if (header.size > 36)
        return fail(error, "Unsupported TIB file header");
    const std::uint64_t base = header.dataOffset;
    if (base < header.size || base >= std::uint64_t(size))
        return fail(error, "Invalid TIB data offset");
    std::vector<std::uint8_t> block(kScanBlock + 3);
    std::string metadataError;
    std::int64_t end = size - 3;
    while (end > 15) {
        const std::int64_t begin = std::max<std::int64_t>(
            15, end - static_cast<std::int64_t>(kScanBlock));
        const int length = static_cast<int>(end - begin + 3);
        if (!readExact(source, begin, block.data(), length))
            return fail(error, "Cannot read TIB trailer search range");
        for (std::int64_t j = end; j-- > begin;) {
            if (le32(block.data() + static_cast<std::size_t>(j - begin)) != 0x94e18a2cU)
                continue;
            std::uint8_t trailer[15];
            if (!readExact(source, j - 15, trailer, 15))
                return fail(error, "Cannot read TIB trailer");
            std::int64_t trailerStart = j - 15;
            if (trailer[0] != 0x68) {
                const std::uint8_t splitPrefix[] = {0x68, 0x01, 0x00, 0x00, 0xff,
                                                    0xff, 0x00, 0x00, 0x00, 0x00};
                std::uint8_t prefix[sizeof(splitPrefix)];
                if (j < 18) continue;
                if (!readExact(source, j - 18, prefix, sizeof(prefix)))
                    return fail(error, "Cannot read TIB trailer");
                if (!std::equal(prefix, prefix + sizeof(prefix), splitPrefix)) continue;
                trailerStart = j - 18;
            }
            const std::uint64_t relative = le64(trailer + 7);
            std::int64_t first = -1;
            if (relative < std::uint64_t(size) - base && relative + base < static_cast<std::uint64_t>(trailerStart)) {
                first = static_cast<std::int64_t>(relative + base);
                std::uint8_t candidateTag = 0;
                if (!readExact(source, first, &candidateTag, 1) || candidateTag != 0x65) first = -1;
            }
            if (first < 0 && !findLocalTibFileIndexStart(source, base, trailerStart, &first))
                continue;
            std::vector<std::uint8_t> decoded;
            std::vector<TibFileIndexEntry> candidate;
            std::string candidateError;
            bool fatal = false;
            std::int64_t next = 0;
            std::uint8_t metadataTag = 0;
            if (!readExact(source, first, &metadataTag, 1))
                return fail(error, "Cannot read TIB metadata record tag");
            if (metadataTag != 0x65) continue;
            if (!readRecord(source, first, trailerStart, 0x65, &decoded, &next,
                            &fatal, &candidateError)) {
                if (error) *error = candidateError;
                return false;
            }
            while (next < trailerStart) {
                std::uint8_t tag = 0;
                if (!readExact(source, next, &tag, 1))
                    return fail(error, "Cannot read TIB metadata record tag");
                if (tag != 0x65) break;
                std::vector<std::uint8_t> previousMetadata;
                if (!readRecord(source, next, trailerStart, 0x65, &previousMetadata, &next,
                                &fatal, &candidateError)) {
                    if (error) *error = candidateError;
                    return false;
                }
            }
            if (decoded.size() < 8) return fail(error, "Truncated TIB metadata record");
            const TibFileMetadataVersion version = {le32(decoded.data()), le32(decoded.data() + 4)};
            TibFileMetadata parsed;
            std::string parseError;
            const bool recognizedMetadata = parseTibFileMetadata(decoded, &parsed, &parseError);
            if (metadataOnly) {
                if (!recognizedMetadata) {
                    if (metadataError.empty()) metadataError = parseError;
                    continue;
                }
                *fileMetadata = std::move(parsed);
                *dataOffset = base;
                return true;
            }
            if (fallback && !fallback->metadataAvailable) {
                if (recognizedMetadata) {
                    fallback->metadata = std::move(parsed);
                    fallback->metadataAvailable = true;
                    fallback->dataOffset = base;
                    fallback->metadataStart = static_cast<std::uint64_t>(first);
                    fallback->metadataEnd = static_cast<std::uint64_t>(next);
                    fallback->trailerStart = static_cast<std::uint64_t>(trailerStart);
                    if (fallback->metadata.hasEncryptionVector)
                        return fail(error, "TIB index requires decryption");
                } else if (fallback->metadataError.empty()) {
                    fallback->metadataError = parseError;
                }
            }
            decoded.clear();
            if (!readTibFileGRecord(source, next, trailerStart, &decoded, &next,
                                    &fatal, &candidateError)) {
                if (fatal || recognizedMetadata) { if (error) *error = candidateError; return false; }
                continue;
            }
            if (next != trailerStart) {
                if (recognizedMetadata) return fail(error, "Invalid TIB file index boundary");
                continue;
            }
            if (version.major < 4) {
                std::vector<TibLegacyFileNode> legacy;
                std::size_t consumed = 0;
                if (!parseTibLegacyFileTree(decoded, version, &legacy, &consumed, error)) return false;
                if (fallback) {
                    fallback->legacyTreeAvailable = true;
                    fallback->legacyVersion = version;
                    fallback->legacyTree = std::move(decoded);
                    fallback->dataOffset = base;
                    fallback->metadataStart = static_cast<std::uint64_t>(first);
                }
                return fail(error, "TIB legacy file content is not supported");
            }
            if (!supportsTibFileMetadataVersion(version))
                return fail(error, "Unsupported TIB metadata version");
            if (!parseTibFileGIndex(decoded, &candidate, &candidateError)) {
                if (recognizedMetadata) { if (error) *error = candidateError; return false; }
                continue;
            }
            // Earlier recovery points need separate format analysis and support.
            *entries = std::move(candidate);
            *dataOffset = base;
            *metadataVersion = version;
            return true;
        }
        end = begin;
    }
    if (metadataOnly && !metadataError.empty()) {
        if (error) *error = metadataError;
        return false;
    }
    return fail(error, "Valid TIB index trailer not found");
    } catch (const std::bad_alloc&) {
        return fail(error, "TIB index allocation failed");
    } catch (const std::length_error&) {
        return fail(error, "TIB index size is not representable");
    }
}

bool readTibFileMetadata(const ByteStorePtr& source, TibFileMetadata* metadata,
                         std::uint64_t* dataOffset, std::string* error) {
    return readTibFileRecords(source, nullptr, dataOffset, nullptr, metadata, true, nullptr, error);
}

bool readTibFileIndex(const ByteStorePtr& source,
                      std::vector<TibFileIndexEntry>* entries,
                      std::uint64_t* dataOffset, TibFileMetadataVersion* metadataVersion,
                      std::string* error, TibFileIndexFallback* fallback) {
    return readTibFileRecords(source, entries, dataOffset, metadataVersion, nullptr, false, fallback, error);
}

} // namespace fs
} // namespace peare
