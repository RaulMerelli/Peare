#include "TibxPageStore.h"
#include "TibxImapLsm.h"
#include "../../common/compression/DeflateDecoder.h"
#include "../compression/TibxCrypto.h"
#include "../compression/TibxRsa.h"
#include "../compression/Lz4Block.h"
#include "../compression/third_party/zstd/zstd.h"

#include <QCryptographicHash>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <map>
#include <set>
#include <utility>

namespace peare {
namespace fs {
namespace {

struct CrcTable {
    std::uint32_t values[256];

    CrcTable() {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (unsigned int bit = 0; bit < 8; ++bit) {
                value = (value >> 1) ^ ((value & 1U) ? 0x82f63b78U : 0U);
            }
            values[i] = value;
        }
    }
};

std::uint32_t pageChecksum(const TibxPageStore::Page& page) {
    static const CrcTable table;
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < page.size(); ++i) {
        const std::uint8_t byte = i >= 4 && i < 8 ? 0U : page[i];
        crc = table.values[(crc ^ byte) & 0xffU] ^ (crc >> 8);
    }
    return crc ^ 0xffffffffU;
}

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

std::uint32_t readBe32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint16_t readBe16(const std::uint8_t* p) {
    return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
}

// Pre-v8 slot omission follows dissect.archive; no authentic pre-v8 archive
// is currently available to verify the mapping end to end.
constexpr bool implicitHeaderSection(std::uint16_t version, std::size_t index) {
    return (version < 7 && index == 8) ||
           (version < 8 && index >= 12 && index <= 16);
}

constexpr std::size_t physicalHeaderIndex(std::uint16_t version, std::size_t index) {
    return index == 0 ? 0 : physicalHeaderIndex(version, index - 1) +
                                  (implicitHeaderSection(version, index - 1) ? 0 : 1);
}

static_assert(physicalHeaderIndex(6, 18) == 12, "TIBX pre-7 volume section mapping");
static_assert(physicalHeaderIndex(7, 18) == 13, "TIBX v7 volume section mapping");
static_assert(physicalHeaderIndex(8, 18) == 18, "TIBX v8 volume section mapping");
static_assert(!implicitHeaderSection(6, 7) && implicitHeaderSection(6, 8) &&
              !implicitHeaderSection(6, 9), "TIBX pre-7 notary section mapping");
static_assert(!implicitHeaderSection(7, 11) && implicitHeaderSection(7, 12) &&
              implicitHeaderSection(7, 16) && !implicitHeaderSection(7, 17),
              "TIBX v7 implicit section boundaries");
static_assert(!implicitHeaderSection(7, 8) && !implicitHeaderSection(8, 8) &&
              !implicitHeaderSection(8, 12) && !implicitHeaderSection(8, 16),
              "TIBX v7 and v8 present sections");

std::uint64_t readBe64(const std::uint8_t* p) {
    return (std::uint64_t(readBe32(p)) << 32) | readBe32(p + 4);
}

std::uint64_t commitChecksum(const TibxPageStore::Page& page) {
    std::uint64_t crc = 0;
    for (std::size_t i = 0; i < 512; ++i) {
        const std::uint8_t byte = (i >= 4 && i < 8) || (i >= 72 && i < 80)
                                      ? 0U : page[i];
        crc ^= byte;
        for (unsigned int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0x95ac9329ac4bc9b5ULL : 0ULL);
        }
    }
    return crc;
}

bool readVariableLength(const std::vector<std::uint8_t>& data, std::size_t* offset,
                        std::uint64_t* value) {
    std::uint64_t result = 0;
    for (unsigned int shift = 0; shift < 64; shift += 7) {
        if (*offset >= data.size()) return false;
        const std::uint8_t byte = data[(*offset)++];
        const std::uint64_t part = byte & 0x7fU;
        if (part > (std::numeric_limits<std::uint64_t>::max() >> shift)) return false;
        result |= part << shift;
        if (!(byte & 0x80U)) {
            *value = result;
            return true;
        }
    }
    return false;
}

bool decodeBlocks(const std::uint8_t* input, std::size_t length,
                  std::size_t maxOutput, std::vector<std::uint8_t>* output, std::string* error) {
    std::size_t offset = 0;
    while (offset < length) {
        if (length - offset < 8) return fail(error, "Truncated TIBX LZ4 block header");
        const std::uint32_t packed = readBe32(input + offset);
        const std::uint32_t unpacked = readBe32(input + offset + 4);
        offset += 8;
        if (packed > length - offset) return fail(error, "Truncated TIBX LZ4 block");
        if (output->size() > maxOutput || unpacked > maxOutput - output->size()) {
            return fail(error, "Invalid TIBX LZ4 output size");
        }
        if (!appendLz4Block(input + offset, packed, output, unpacked)) {
            return fail(error, "Invalid TIBX LZ4 block");
        }
        offset += packed;
    }
    return true;
}

}  // namespace

struct TibxPageStore::SegmentCache {
    std::mutex mutex;
    std::uint64_t headerOffset = 0;
    std::uint64_t segmentId = 0;
    bool valid = false;
    std::shared_ptr<const std::vector<std::uint8_t>> data;
};

TibxPageStore::TibxPageStore(ByteStorePtr source)
    : source_(std::move(source)), segmentCache_(std::make_shared<SegmentCache>()) {}

std::uint64_t TibxPageStore::pageCount() const {
    if (!source_ || source_->capacity() <= 0) return 0;
    return static_cast<std::uint64_t>(source_->capacity()) / kPageSize;
}

bool TibxPageStore::readPage(std::uint64_t index, Page* page, std::string* error, bool* ioError) const {
    if (ioError) *ioError = false;
    if (!source_ || !page) return fail(error, "Missing TIBX page source");
    if (index >= pageCount()) return fail(error, "TIBX page is outside the archive");
    const std::int64_t offset = static_cast<std::int64_t>(index * kPageSize);
    int done = 0;
    while (done < static_cast<int>(kPageSize)) {
        const int count = source_->read(offset + done, page->data() + done,
                                        static_cast<int>(kPageSize) - done);
        if (count <= 0 || count > static_cast<int>(kPageSize) - done) {
            if (ioError) *ioError = true;
            return fail(error, "Could not read the complete TIBX page");
        }
        done += count;
    }
    if ((*page)[0] != 'A' || (*page)[1] == 0 || (*page)[2] != 0) {
        return fail(error, "Invalid TIBX page header");
    }
    const std::uint32_t expected = readBe32(page->data() + 4);
    if (pageChecksum(*page) != expected) return fail(error, "TIBX page checksum mismatch");
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readCommitInfo(std::uint64_t index, CommitInfo* info, bool* ioError) const {
    Page page;
    if (!info || !readPage(index, &page, nullptr, ioError) || page[1] != 2 ||
        std::memcmp(page.data() + 8, "ARCI", 4) != 0 ||
        commitChecksum(page) != readBe64(page.data() + 72)) return false;
    info->flags = readBe32(page.data() + 16);
    info->archiveSize = readBe32(page.data() + 20);
    info->archiveOffset = readBe64(page.data() + 24);
    info->sequence = readBe64(page.data() + 32);
    info->previousOffset = readBe64(page.data() + 64);
    info->volumeOffset = readBe64(page.data() + 80);
    std::copy(page.begin() + 136, page.begin() + 152, info->archiveId.begin());
    for (std::size_t i = 0; i < info->volumeIds.size(); ++i) {
        info->volumeIds[i] = readBe32(page.data() + 88 + i * 12);
        info->volumeOffsets[i] = readBe64(page.data() + 92 + i * 12);
    }
    return true;
}

bool TibxPageStore::readHeaderAtCommit(std::uint64_t index, CommittedHeader* header, bool* ioError) const {
    return header && readCommit(index, header, false, ioError);
}

bool TibxPageStore::readCommit(std::uint64_t index, CommittedHeader* header, bool requireCurrent,
                               bool* ioError) const {
    Page commit;
    if (!readPage(index, &commit, nullptr, ioError) || commit[1] != 2 ||
        std::memcmp(commit.data() + 8, "ARCI", 4) != 0 ||
        commitChecksum(commit) != readBe64(commit.data() + 72) ||
        (requireCurrent && !(readBe32(commit.data() + 16) & 0x02U))) return false;

    const std::uint32_t storedSize = readBe32(commit.data() + 20);
    const std::uint64_t offset = readBe64(commit.data() + 24);
    if (!offset || offset % kPageSize || !storedSize || storedSize % kPageSize) return false;
    const std::uint64_t start = offset / kPageSize;
    const std::uint64_t pages = storedSize / kPageSize;
    if (start >= pageCount() || pages > pageCount() - start) return false;

    Page part;
    if (!readPage(start, &part, nullptr, ioError) || part[1] != 1 ||
        std::memcmp(part.data() + 8, "ARCH", 4) != 0) return false;
    const std::uint32_t length = readBe32(part.data() + 12);
    const std::uint64_t capacity = pages * (kPageSize - 8);
    if (length < 1024 || length > capacity) return false;

    std::vector<std::uint8_t> data;
    data.reserve(length);
    for (std::uint64_t i = 0; i < pages && data.size() < length; ++i) {
        if (i && (!readPage(start + i, &part, nullptr, ioError) || part[1] != 1)) return false;
        const std::size_t count = std::min<std::size_t>(kPageSize - 8, length - data.size());
        data.insert(data.end(), part.begin() + 8, part.begin() + 8 + count);
    }
    if (readBe16(data.data() + 8) != readBe16(commit.data() + 12) ||
        std::memcmp(data.data() + 32, commit.data() + 136, 16) != 0 ||
        readBe64(data.data() + 376) > readBe64(commit.data() + 32) ||
        readBe64(data.data() + 392) > readBe64(commit.data() + 48) ||
        readBe64(data.data() + 400) != readBe64(commit.data() + 56) ||
        (!(readBe32(commit.data() + 16) & 2U) &&
         readBe64(data.data() + 424) != index * kPageSize)) return false;
    header->offset = offset;
    header->sequence = readBe64(commit.data() + 32);
    header->data.swap(data);
    return true;
}

bool TibxPageStore::readCurrentHeader(CommittedHeader* header, bool* ioError) const {
    if (ioError) *ioError = false;
    return header && source_ && pageCount() > 2 && readCommit(2, header, true, ioError);
}

bool TibxPageStore::readCommittedHeader(CommittedHeader* header, std::string* error, bool* ioFailure) const {
    if (ioFailure) *ioFailure = false;
    if (!header || !source_) return fail(error, "Missing TIBX header destination");
    bool ioError = false;
    if (readCurrentHeader(header, &ioError)) {
        if (error) error->clear();
        return true;
    }
    if (ioError) { if (ioFailure) *ioFailure = true; return fail(error, "Could not read the complete TIBX page"); }
    Page firstCommit;
    if (pageCount() > 2 && readPage(2, &firstCommit, nullptr, &ioError) && firstCommit[1] == 2 &&
        std::memcmp(firstCommit.data() + 8, "ARCI", 4) == 0 &&
        (readBe32(firstCommit.data() + 16) & 2U) != 0 &&
        commitChecksum(firstCommit) == readBe64(firstCommit.data() + 72)) {
        const std::uint64_t position = readBe64(firstCommit.data() + 24);
        const std::uint64_t size = readBe32(firstCommit.data() + 20);
        const std::uint64_t available = pageCount() * kPageSize;
        if (position && (position > available || size > available - position)) {
            return fail(error, "Missing TIBX continuation");
        }
    }
    if (ioError) { if (ioFailure) *ioFailure = true; return fail(error, "Could not read the complete TIBX page"); }
    for (std::uint64_t i = pageCount(); i > 0; --i) {
        if (readCommit(i - 1, header, false, &ioError)) {
            if (error) error->clear();
            return true;
        }
        if (ioError) { if (ioFailure) *ioFailure = true; return fail(error, "Could not read the complete TIBX page"); }
    }
    return fail(error, "No valid TIBX commit found");
}

bool TibxPageStore::readHeaderSections(const CommittedHeader& header,
                                       std::vector<HeaderSection>* sections,
                                       std::string* error) {
    if (!sections) return fail(error, "Missing TIBX section destination");
    if (header.data.size() < 1024 ||
        std::memcmp(header.data.data(), "ARCH", 4) != 0 ||
        readBe32(header.data.data() + 4) != header.data.size()) {
        return fail(error, "Invalid TIBX archive header");
    }

    std::vector<HeaderSection> result;
    std::size_t offset = 1024;
    const std::uint16_t version = readBe16(header.data.data() + 8);
    while (offset < header.data.size()) {
        while (implicitHeaderSection(version, result.size())) {
            const HeaderSection emptySection = {offset, 0};
            result.push_back(emptySection);
        }
        if (header.data.size() - offset < 4) return fail(error, "Truncated TIBX section length");
        const std::uint32_t length = readBe32(header.data.data() + offset);
        offset += 4;
        const std::uint64_t padded = (std::uint64_t(length) + 3U) & ~std::uint64_t(3U);
        if (padded > header.data.size() - offset) return fail(error, "Truncated TIBX section");
        HeaderSection section = {offset, static_cast<std::size_t>(length)};
        result.push_back(section);
        offset += static_cast<std::size_t>(padded);
    }
    sections->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readSuperblock(const CommittedHeader& header, const HeaderSection& section,
                                   Superblock* superblock, std::string* error) {
    if (!superblock) return fail(error, "Missing TIBX superblock destination");
    if (section.offset > header.data.size() || section.length > header.data.size() - section.offset ||
        section.length < 376) return fail(error, "Truncated TIBX superblock");
    const std::uint8_t* data = header.data.data() + section.offset;
    if (std::memcmp(data, "L-SB", 4) != 0 || data[4] > 2) {
        return fail(error, "Unsupported TIBX superblock");
    }
    const unsigned int levelCount = unsigned(data[5]);
    if (levelCount > 10 || data[6] < data[5]) return fail(error, "Invalid TIBX tree levels");
    const std::uint32_t inlineLength = readBe32(data + 348);
    if (inlineLength != section.length - 376) return fail(error, "Invalid TIBX inline tree size");

    Superblock result;
    result.keyLength = readBe32(data + 16);
    result.valueLength = readBe32(data + 20);
    result.inlineEncoding = data[344];
    result.inlineCount = readBe16(data + 346);
    result.inlineOffset = section.offset + 376;
    result.inlineLength = inlineLength;
    result.levels.reserve(levelCount);
    for (unsigned int i = 0; i < levelCount; ++i) {
        const std::uint8_t* level = data + 24 + 32 * i;
        TreeLevel tree = {readBe64(level), readBe64(level + 8), readBe32(level + 16)};
        result.levels.push_back(tree);
    }
    *superblock = std::move(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::decodeInlineTree(const CommittedHeader& header, const Superblock& superblock,
                                     std::vector<std::uint8_t>* data, std::string* error) const {
    if (!data) return fail(error, "Missing TIBX inline tree destination");
    if (superblock.inlineOffset > header.data.size() ||
        superblock.inlineLength > header.data.size() - superblock.inlineOffset) {
        return fail(error, "Truncated TIBX inline tree");
    }
    const std::uint8_t encoding = superblock.inlineEncoding & 0x7fU;
    const std::size_t end = superblock.inlineOffset + superblock.inlineLength;
    std::vector<std::uint8_t> encoded(header.data.begin() + superblock.inlineOffset, header.data.begin() + end);
    if (superblock.inlineEncoding & 0x80U) {
        std::vector<std::uint8_t> clear;
        if (!tibxDecrypt(encryptionKey_, encryptionAlgorithm_, encoded, &clear)) return fail(error, "Invalid encrypted TIBX inline tree");
        encoded.swap(clear);
    }
    std::vector<std::uint8_t> result;
    if (encoding == 0) {
        result.swap(encoded);
    } else if (encoding == 1) {
        if (!decodeBlocks(encoded.data(), encoded.size(), std::numeric_limits<std::size_t>::max(),
                          &result, error)) return false;
    } else {
        return fail(error, "Unsupported TIBX inline tree codec");
    }
    data->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::unlock(const CommittedHeader& header, const Superblock& keyMap,
                           const std::string& password, std::string* error) {
    std::vector<TreeCell> cells;
    if (!readTree(header, keyMap, &cells, error)) return false;
    for (const TreeCell& cell : cells) {
        if (!cell.deleted && !cell.key.empty() && cell.key[0] == 1 &&
            tibxUnwrapKey(password, cell.value, &encryptionKey_, &encryptionAlgorithm_)) {
            segmentCache_ = std::make_shared<SegmentCache>();
            return true;
        }
    }
    return fail(error, "Invalid TIBX password or unsupported encryption key");
}

bool TibxPageStore::unlockPrivateKey(const CommittedHeader& header, const Superblock& keyMap,
                                     const std::string& privateKeyPem, std::string* error,
                                     bool* resourceFailure) {
    if (resourceFailure) *resourceFailure = false;
    TibxRsaPrivateKey privateKey;
    std::vector<std::uint8_t> spki;
    const TibxRsaResult loaded = privateKey.loadPem(privateKeyPem);
    if (loaded == TibxRsaResult::ResourceError) {
        if (resourceFailure) *resourceFailure = true;
        return fail(error, "TIBX private key resource failure");
    }
    if (loaded != TibxRsaResult::Success)
        return fail(error, "Invalid or unsupported TIBX private key");
    const TibxRsaResult exported = privateKey.publicSpkiDer(&spki);
    if (exported == TibxRsaResult::ResourceError) {
        if (resourceFailure) *resourceFailure = true;
        return fail(error, "TIBX private key resource failure");
    }
    if (exported != TibxRsaResult::Success ||
        spki.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return fail(error, "Invalid or unsupported TIBX private key");
    const QByteArray keyId = QCryptographicHash::hash(
        QByteArray(reinterpret_cast<const char*>(spki.data()), static_cast<int>(spki.size())),
        QCryptographicHash::Sha256);
    std::vector<TreeCell> cells;
    if (!readTree(header, keyMap, &cells, error)) {
        if (resourceFailure) *resourceFailure = true;
        return false;
    }
    std::set<std::pair<unsigned, std::vector<std::uint8_t> > > attempted;
    for (const TreeCell& cell : cells) {
        if (cell.deleted || cell.key.size() != 37 || cell.key[0] != 2 ||
            cell.value.size() <= 4 || cell.value[0] != 2 ||
            std::memcmp(cell.key.data() + 1, keyId.constData(), 32) != 0) continue;
        const unsigned alg = cell.value[1];
        const std::size_t size = alg == 1 || alg == 5 ? 16 :
            alg == 2 || alg == 6 ? 24 : alg == 3 || alg == 4 || alg == 7 ? 32 : 0;
        if (!size) continue;
        const std::vector<std::uint8_t> wrapped(cell.value.begin() + 4, cell.value.end());
        if (!attempted.insert(std::make_pair(alg, wrapped)).second) continue;
        std::vector<std::uint8_t> clear;
        const TibxRsaResult result = privateKey.decryptPkcs1v15(wrapped, &clear);
        if (result == TibxRsaResult::ResourceError) {
            std::fill(clear.begin(), clear.end(), 0);
            if (resourceFailure) *resourceFailure = true;
            return fail(error, "TIBX private key resource failure");
        }
        if (result != TibxRsaResult::Success || clear.size() != size) {
            std::fill(clear.begin(), clear.end(), 0);
            continue;
        }
        encryptionKey_.swap(clear);
        std::fill(clear.begin(), clear.end(), 0);
        encryptionAlgorithm_ = static_cast<std::uint8_t>(alg);
        segmentCache_ = std::make_shared<SegmentCache>();
        if (error) error->clear();
        return true;
    }
    return fail(error, "TIBX private key does not unlock this archive");
}

bool TibxPageStore::readTreePage(std::uint64_t offset, TreePage* page, std::string* error) const {
    if (!page) return fail(error, "Missing TIBX tree page destination");
    if (offset % kPageSize) return fail(error, "Unaligned TIBX tree page");
    Page source;
    if (!readPage(offset / kPageSize, &source, error)) return false;
    const bool leaf = source[1] == 3;
    if ((!leaf && source[1] != 4) ||
        std::memcmp(source.data() + 8, leaf ? "LEAF" : "LDIR", 4) != 0 || source[12] >= 2) {
        return fail(error, "Invalid TIBX tree page header");
    }
    const std::uint8_t encoding = source[13];
    const std::uint32_t expanded = readBe32(source.data() + 16);
    const std::uint32_t packed = readBe32(source.data() + 20);
    const std::size_t start = 60;
    const std::uint64_t firstPage = offset / kPageSize;
    const std::size_t firstBytes = std::min<std::size_t>(packed, kPageSize - start);
    const std::uint64_t remaining = std::uint64_t(packed) - firstBytes;
    const std::uint64_t continuationSize = kPageSize - 8;
    const std::uint64_t continuations = remaining / continuationSize +
                                        (remaining % continuationSize != 0 ? 1 : 0);
    const std::uint64_t pages = pageCount();
    if (firstPage >= pages || continuations > pages - firstPage - 1)
        return fail(error, "Truncated TIBX tree page payload");
    TreePage result;
    result.leaf = leaf;
    result.cellCount = readBe16(source.data() + 14);
    const std::uint8_t pageType = source[1];
    std::vector<std::uint8_t> encoded;
    if (packed > encoded.max_size()) return fail(error, "TIBX tree page payload is too large");
    encoded.reserve(packed);
    encoded.insert(encoded.end(), source.begin() + start, source.begin() + start + firstBytes);
    for (std::uint64_t i = 1; i <= continuations; ++i) {
        if (!readPage(firstPage + i, &source, error)) return false;
        if (source[1] != pageType) return fail(error, "Invalid TIBX tree continuation page");
        const std::size_t count = std::min<std::size_t>(kPageSize - 8, packed - encoded.size());
        encoded.insert(encoded.end(), source.begin() + 8, source.begin() + 8 + count);
    }
    if (encoding & 0x80U) {
        std::vector<std::uint8_t> clear;
        if (!tibxDecrypt(encryptionKey_, encryptionAlgorithm_, encoded, &clear)) return fail(error, "Invalid encrypted TIBX tree page");
        encoded.swap(clear);
    }
    if ((encoding & 0x7fU) == 0) {
        if (encoded.size() != expanded) return fail(error, "Invalid raw TIBX tree page size");
        result.data.swap(encoded);
    } else if ((encoding & 0x7fU) == 1) {
        if (!decodeBlocks(encoded.data(), encoded.size(), expanded, &result.data, error)) return false;
        if (result.data.size() != expanded) return fail(error, "Invalid TIBX tree page size");
    } else {
        return fail(error, "Unsupported TIBX tree page codec");
    }
    *page = std::move(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readInlineCells(const std::vector<std::uint8_t>& data,
                                    const Superblock& superblock, std::vector<TreeCell>* cells,
                                    std::string* error) {
    if (!cells) return fail(error, "Missing TIBX cells destination");
    std::vector<TreeCell> result;
    std::size_t offset = 0;
    while (result.size() < superblock.inlineCount) {
        if (data.size() - offset < 4) return fail(error, "Truncated TIBX cell group");
        const std::uint8_t count = data[offset];
        const std::uint32_t alive = (std::uint32_t(data[offset + 1]) << 16) |
                                    (std::uint32_t(data[offset + 2]) << 8) |
                                    std::uint32_t(data[offset + 3]);
        offset += 4;
        if (!count || count > 24 || count > superblock.inlineCount - result.size()) {
            return fail(error, "Invalid TIBX cell group size");
        }
        for (unsigned int i = 0; i < count; ++i) {
            std::uint64_t keyLength = superblock.keyLength;
            std::uint64_t valueLength = superblock.valueLength;
            if (!superblock.keyLength &&
                (!readVariableLength(data, &offset, &keyLength) ||
                 !readVariableLength(data, &offset, &valueLength))) {
                return fail(error, "Invalid TIBX variable cell length");
            }
            const bool deleted = (alive & (1U << i)) == 0;
            if (keyLength > data.size() - offset) return fail(error, "Truncated TIBX cell key");
            TreeCell cell;
            cell.deleted = deleted;
            cell.key.assign(data.begin() + offset, data.begin() + offset + std::size_t(keyLength));
            offset += std::size_t(keyLength);
            if (!deleted) {
                if (valueLength > data.size() - offset) return fail(error, "Truncated TIBX cell value");
                cell.value.assign(data.begin() + offset,
                                  data.begin() + offset + std::size_t(valueLength));
                offset += std::size_t(valueLength);
            }
            result.push_back(std::move(cell));
        }
    }
    if (offset != data.size()) return fail(error, "Unexpected TIBX cell data");
    cells->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readTreeCells(const TreePage& page, const Superblock& superblock,
                                  std::vector<TreeCell>* cells, std::string* error,
                                  const TibxImapSort* imapSort) {
    if (!cells) return fail(error, "Missing TIBX cells destination");
    if (page.leaf) {
        Superblock level = superblock;
        level.inlineCount = page.cellCount;
        return readInlineCells(page.data, level, cells, error);
    }

    std::vector<TreeCell> result;
    std::size_t offset = 0;
    for (unsigned int i = 0; i < page.cellCount; ++i) {
        std::uint64_t keyLength = superblock.keyLength;
        std::uint64_t valueLength = 8;
        if (!superblock.keyLength &&
            (!readVariableLength(page.data, &offset, &keyLength) ||
             !readVariableLength(page.data, &offset, &valueLength))) {
            return fail(error, "Invalid TIBX directory cell length");
        }
        if (valueLength != 8 || keyLength > page.data.size() - offset) {
            return fail(error, "Invalid TIBX directory cell key");
        }
        TreeCell cell;
        cell.deleted = false;
        cell.key.assign(page.data.begin() + offset,
                        page.data.begin() + offset + std::size_t(keyLength));
        offset += std::size_t(keyLength);
        if (page.data.size() - offset < 8) return fail(error, "Truncated TIBX child pointer");
        cell.value.assign(page.data.begin() + offset, page.data.begin() + offset + 8);
        offset += 8;
        if (imapSort) {
            int order = 0;
            if (!compareTibxImapKeys(cell.key, cell.key, *imapSort, &order) ||
                (!result.empty() &&
                 !compareTibxImapKeys(cell.key, result.back().key, *imapSort, &order))) {
                return fail(error, "Invalid TIBX IMAP directory key");
            }
            if (order < 0) return fail(error, "Unordered TIBX directory keys");
        } else if (!result.empty() && cell.key < result.back().key) {
            return fail(error, "Unordered TIBX directory keys");
        }
        result.push_back(std::move(cell));
    }
    if (offset != page.data.size()) return fail(error, "Unexpected TIBX directory data");
    cells->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::findTreeCell(const CommittedHeader& header, const Superblock& superblock,
                                 const std::vector<std::uint8_t>& key, TreeCell* cell,
                                 bool* found, std::string* error) const {
    if (!cell || !found) return fail(error, "Missing TIBX lookup destination");
    if (superblock.keyLength && key.size() != superblock.keyLength) {
        return fail(error, "Invalid TIBX lookup key size");
    }
    *found = false;
    std::vector<std::uint8_t> inlineData;
    std::vector<TreeCell> cells;
    if (!decodeInlineTree(header, superblock, &inlineData, error) ||
        !readInlineCells(inlineData, superblock, &cells, error)) return false;
    bool inlineFound = false;
    for (const TreeCell& candidate : cells) {
        if (candidate.key == key) {
            inlineFound = true;
            *found = !candidate.deleted;
            if (*found) *cell = candidate;
        }
    }
    if (inlineFound) {
        if (error) error->clear();
        return true;
    }

    for (const TreeLevel& level : superblock.levels) {
        if (level.rootOffset == std::numeric_limits<std::uint64_t>::max()) continue;
        std::uint64_t offset = level.rootOffset;
        std::set<std::uint64_t> visited;
        for (;;) {
            if (!visited.insert(offset).second) return fail(error, "TIBX tree contains a cycle");
            TreePage page;
            if (!readTreePage(offset, &page, error) ||
                !readTreeCells(page, superblock, &cells, error)) return false;
            if (page.leaf) {
                for (const TreeCell& candidate : cells) {
                    if (candidate.key == key) {
                        *found = !candidate.deleted;
                        if (*found) *cell = candidate;
                        if (error) error->clear();
                        return true;
                    }
                }
                break;
            }
            const auto child = std::lower_bound(
                cells.begin(), cells.end(), key,
                [](const TreeCell& candidate, const std::vector<std::uint8_t>& target) {
                    return candidate.key < target;
                });
            if (child == cells.end()) break;
            offset = readBe64(child->value.data());
        }
    }
    if (error) error->clear();
    return true;
}

bool TibxPageStore::findNextTreeCell(const CommittedHeader& header, const Superblock& superblock,
                                     const std::vector<std::uint8_t>& lower,
                                     TreeCell* cell, bool* found, std::string* error) const {
    if (!cell || !found) return fail(error, "Missing TIBX lookup destination");
    std::vector<std::uint8_t> target(lower), inlineData;
    std::vector<TreeCell> inlineCells;
    if (!decodeInlineTree(header, superblock, &inlineData, error) ||
        !readInlineCells(inlineData, superblock, &inlineCells, error)) return false;
    for (;;) {
        bool have = false;
        TreeCell best;
        for (const TreeCell& entry : inlineCells) {
            if (!(entry.key < target) && (!have || !(best.key < entry.key))) {
                best = entry;
                have = true;
            }
        }
        for (const TreeLevel& level : superblock.levels) {
            if (level.rootOffset == std::numeric_limits<std::uint64_t>::max()) continue;
            std::vector<std::uint64_t> pending(1, level.rootOffset);
            std::set<std::uint64_t> visited;
            while (!pending.empty()) {
                const std::uint64_t offset = pending.back();
                pending.pop_back();
                if (!visited.insert(offset).second) return fail(error, "TIBX tree contains a cycle");
                TreePage page;
                std::vector<TreeCell> entries;
                if (!readTreePage(offset, &page, error) ||
                    !readTreeCells(page, superblock, &entries, error)) return false;
                if (page.leaf) {
                    for (const TreeCell& entry : entries) {
                        if (!(entry.key < target) && (!have || entry.key < best.key)) {
                            best = entry;
                            have = true;
                        }
                    }
                } else {
                    for (std::size_t i = entries.size(); i-- > 0;) {
                        if (entries[i].key < target) continue;
                        if (have && i && !(entries[i - 1].key < best.key)) continue;
                        pending.push_back(readBe64(entries[i].value.data()));
                    }
                }
            }
        }
        if (!have) { *found = false; if (error) error->clear(); return true; }
        if (!best.deleted) { *cell = std::move(best); *found = true; if (error) error->clear(); return true; }
        target = best.key;
        target.push_back(0);
    }
}

bool TibxPageStore::readSegment(const CommittedHeader& header, const Superblock& segmentMap,
                                std::uint64_t segmentId,
                                std::shared_ptr<const std::vector<std::uint8_t>>* data,
                                std::string* error) const {
    if (!data) return fail(error, "Missing TIBX segment destination");
    {
        std::lock_guard<std::mutex> lock(segmentCache_->mutex);
        if (segmentCache_->valid && segmentCache_->segmentId == segmentId &&
            segmentCache_->headerOffset == header.offset) {
            *data = segmentCache_->data;
            if (error) error->clear();
            return true;
        }
    }
    std::vector<std::uint8_t> key(8);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::uint8_t>(segmentId >> (56 - i * 8));
    }
    TreeCell cell;
    bool found = false;
    if (!findTreeCell(header, segmentMap, key, &cell, &found, error)) return false;
    if (!found) return fail(error, "TIBX segment not found");
    if (cell.value.size() < 32) return fail(error, "Invalid TIBX segment map entry");
    const std::uint64_t locator = readBe64(cell.value.data());
    const std::uint64_t firstPage = locator & 0x00ffffffffffffffULL;
    const std::uint64_t count = locator >> 56;
    if (!count || firstPage >= pageCount() || count > pageCount() - firstPage) {
        return fail(error, "TIBX segment is outside the archive");
    }

    Page page;
    if (!readPage(firstPage, &page, error)) return false;
    const bool encrypted = std::memcmp(page.data() + 8, "SE\0\1", 4) == 0;
    if (page[1] != 0xff || (!encrypted && std::memcmp(page.data() + 8, "SG\0\1", 4) != 0)) {
        return fail(error, "Invalid TIBX segment header");
    }
    const std::uint32_t expanded = readBe32(page.data() + 12);
    const std::uint32_t compressed = readBe32(page.data() + 16);
    const std::uint8_t codec = page[24];
    if (encrypted != (readBe32(page.data() + 20) != 0)) return fail(error, "Invalid TIBX encryption marker");
    const std::uint64_t available = kPageSize - 44 + (count - 1) * (kPageSize - 8);
    if (compressed > available) return fail(error, "Truncated TIBX segment");

    std::vector<std::uint8_t> packed;
    packed.reserve(compressed);
    for (std::uint64_t i = 0; i < count && packed.size() < compressed; ++i) {
        if (i && (!readPage(firstPage + i, &page, error) || page[1] != 0xff)) {
            return fail(error, "Invalid TIBX segment page");
        }
        const std::size_t start = i ? 8 : 44;
        const std::size_t bytes = std::min<std::size_t>(kPageSize - start,
                                                        compressed - packed.size());
        packed.insert(packed.end(), page.begin() + start, page.begin() + start + bytes);
    }

    if (encrypted) {
        std::vector<std::uint8_t> clear;
        if (!tibxDecrypt(encryptionKey_, encryptionAlgorithm_, packed, &clear)) return fail(error, "Invalid encrypted TIBX segment");
        packed.swap(clear);
    }

    std::vector<std::uint8_t> result;
    if (codec == 0) {
        if (packed.size() != expanded) return fail(error, "Invalid stored TIBX segment size");
        result.swap(packed);
    } else if (codec == 1) {
        if (!decompressLz4Block(packed.data(), packed.size(), &result, expanded)) {
            return fail(error, "Invalid TIBX LZ4 segment");
        }
    } else if (codec == 2) {
        if (!compression::inflateZlibExact(packed, expanded, &result)) {
            return fail(error, "Invalid TIBX zlib segment");
        }
    } else if (codec == 3) {
        result.resize(expanded);
        const std::size_t written = ZSTD_decompress(result.data(), result.size(),
                                                    packed.data(), packed.size());
        if (ZSTD_isError(written) || written != result.size()) {
            return fail(error, "Invalid TIBX Zstandard segment");
        }
    } else {
        return fail(error, "Unsupported TIBX segment codec");
    }
    const bool retain = result.size() <= 32U * 1024U * 1024U;
    std::shared_ptr<const std::vector<std::uint8_t>> decoded =
        std::make_shared<const std::vector<std::uint8_t>>(std::move(result));
    if (retain) {
        std::lock_guard<std::mutex> lock(segmentCache_->mutex);
        segmentCache_->headerOffset = header.offset;
        segmentCache_->segmentId = segmentId;
        segmentCache_->data = decoded;
        segmentCache_->valid = true;
    }
    *data = std::move(decoded);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readTree(const CommittedHeader& header, const Superblock& superblock,
                             std::vector<TreeCell>* cells, std::string* error) const {
    return readTreePrefix(header, superblock, std::vector<std::uint8_t>(), cells, error);
}

bool TibxPageStore::readTreePrefix(const CommittedHeader& header, const Superblock& superblock,
                                   const std::vector<std::uint8_t>& prefix,
                                   std::vector<TreeCell>* cells, std::string* error) const {
    if (!cells) return fail(error, "Missing TIBX tree destination");
    std::vector<std::uint8_t> upper(prefix);
    while (!upper.empty() && upper.back() == 0xffU) upper.pop_back();
    if (!upper.empty()) ++upper.back();
    const auto inRange = [&prefix, &upper](const std::vector<std::uint8_t>& key) {
        return !(key < prefix) && (upper.empty() || key < upper);
    };
    std::vector<std::uint8_t> inlineData;
    std::vector<TreeCell> entries;
    if (!decodeInlineTree(header, superblock, &inlineData, error) ||
        !readInlineCells(inlineData, superblock, &entries, error)) return false;
    std::map<std::vector<std::uint8_t>, TreeCell> merged;
    for (const TreeCell& entry : entries) {
        if (!inRange(entry.key)) continue;
        const auto inserted = merged.insert(std::make_pair(entry.key, entry));
        if (!inserted.second) inserted.first->second = entry;
    }

    for (const TreeLevel& level : superblock.levels) {
        if (level.rootOffset == std::numeric_limits<std::uint64_t>::max()) continue;
        std::vector<std::uint64_t> pending(1, level.rootOffset);
        std::set<std::uint64_t> visited;
        while (!pending.empty()) {
            const std::uint64_t offset = pending.back();
            pending.pop_back();
            if (!visited.insert(offset).second) return fail(error, "TIBX tree contains a cycle");
            TreePage page;
            if (!readTreePage(offset, &page, error) ||
                !readTreeCells(page, superblock, &entries, error)) return false;
            for (std::size_t i = 0; i < entries.size(); ++i) {
                const TreeCell& entry = entries[i];
                if (page.leaf) {
                    if (inRange(entry.key)) merged.insert(std::make_pair(entry.key, entry));
                } else if (!(entry.key < prefix) &&
                           (i == 0 || upper.empty() || entries[i - 1].key < upper)) {
                    pending.push_back(readBe64(entry.value.data()));
                }
            }
        }
    }

    std::vector<TreeCell> result;
    result.reserve(merged.size());
    for (const auto& entry : merged) result.push_back(entry.second);
    cells->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readExtents(const CommittedHeader& header, const Superblock& dataMap, std::uint64_t volumeId,
                                std::vector<Extent>* extents, std::string* error) const {
    if (!extents) return fail(error, "Missing TIBX extent destination");
    if (dataMap.keyLength != 31 || dataMap.valueLength != 10) {
        return fail(error, "Unsupported TIBX extent map schema");
    }
    std::vector<TreeCell> cells;
    std::vector<std::uint8_t> prefix(8);
    for (std::size_t i = 0; i < prefix.size(); ++i)
        prefix[i] = static_cast<std::uint8_t>(volumeId >> (56 - i * 8));
    if (!readTreePrefix(header, dataMap, prefix, &cells, error)) return false;
    std::vector<Extent> result;
    result.reserve(cells.size());
    for (const TreeCell& cell : cells) {
        if (cell.deleted) continue;
        if (cell.key.size() != 31 || cell.value.size() != 10) {
            return fail(error, "Invalid TIBX extent map entry");
        }
        const std::uint8_t* key = cell.key.data();
        Extent extent = {readBe64(key), readBe64(key + 8),
                         (std::uint32_t(key[16]) << 16) | (std::uint32_t(key[17]) << 8) |
                             std::uint32_t(key[18]),
                         readBe32(key + 19), readBe64(key + 23),
                         readBe64(cell.value.data()), readBe16(cell.value.data() + 8)};
        result.push_back(extent);
    }
    extents->swap(result);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readRecoveryPoints(const CommittedHeader& header, const Superblock& sliceMap,
                                       std::vector<RecoveryPoint>* points, std::string* error) const {
    if (!points) return fail(error, "Missing TIBX recovery point destination");
    if (sliceMap.keyLength != 4 || sliceMap.valueLength != 132) {
        return fail(error, "Unsupported TIBX recovery point schema");
    }
    std::vector<TreeCell> cells;
    if (!readTree(header, sliceMap, &cells, error)) return false;
    std::vector<RecoveryPoint> result;
    for (const TreeCell& cell : cells) {
        if (cell.deleted) continue;
        if (cell.key.size() != 4 || cell.value.size() != 132) {
            return fail(error, "Invalid TIBX recovery point");
        }
        if (cell.value[68] & 0x80U) continue;
        RecoveryPoint point;
        point.sliceId = readBe32(cell.key.data());
        point.fullId = readBe32(cell.value.data() + 48);
        point.hidden = (cell.value[68] & 0x73U) == 3U;
        std::copy(cell.value.begin(), cell.value.begin() + 16, point.id.begin());
        point.created = readBe64(cell.value.data() + 16);
        point.finished = readBe64(cell.value.data() + 24);
        result.push_back(point);
    }
    points->swap(result);
    if (error) error->clear();
    return true;
}

namespace {

bool visitResolvedImap(const TibxPageStore& pages, const TibxPageStore::CommittedHeader& header,
                       const TibxPageStore::Superblock& itemMap, std::uint32_t fullId,
                       std::uint32_t sliceId,
                       const std::function<bool(const TibxPageStore::TreeCell&)>& visit,
                       std::string* error) {
    if (header.data.size() < 488 || fullId > sliceId) return fail(error, "Invalid TIBX IMAP range");
    const TibxImapSort mode = (readBe16(header.data.data() + 8) > 1 &&
                               (readBe64(header.data.data() + 480) & 1U))
        ? TibxImapSort::Plain : TibxImapSort::Hierarchical;
    TibxImapLsm cursor(pages, header, itemMap, mode);
    std::vector<std::uint8_t> lower, group;
    bool haveLower = false, haveGroup = false, selected = false;
    for (;;) {
        TibxPageStore::TreeCell cell;
        bool found = false;
        if (!cursor.next(haveLower ? &lower : nullptr, false, &cell, &found, error)) return false;
        if (!found) return true;
        lower = cell.key;
        haveLower = true;
        std::vector<std::uint8_t> nameKey(cell.key);
        std::fill(nameKey.begin(), nameKey.begin() + 4, std::uint8_t(0));
        int order = 0;
        if (haveGroup && !compareTibxImapKeys(group, nameKey, mode, &order))
            return fail(error, "Invalid TIBX IMAP name");
        if (!haveGroup || order != 0) {
            group.swap(nameKey);
            haveGroup = true;
            selected = false;
        }
        const std::uint32_t itemSlice = readBe32(cell.key.data());
        if (selected || itemSlice < fullId || itemSlice > sliceId || !(cell.key[5] & 1U)) continue;
        selected = true;
        if (!(cell.key[4] & 1U) && !visit(cell)) return false;
    }
}

std::size_t unescapedSlash(const std::string& value) {
    std::size_t backslashes = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '\\') { ++backslashes; continue; }
        if (value[i] == '/' && !(backslashes & 1U)) return i;
        backslashes = 0;
    }
    return std::string::npos;
}

bool primaryStream(const TibxPageStore::TreeCell& cell, std::uint64_t* id,
                   std::uint64_t* size, std::string* error) {
    if (cell.value.empty()) return fail(error, "Invalid TIBX item metadata");
    const std::size_t count = std::size_t(cell.value[0]) + 1;
    if (count > (cell.value.size() - 1) / 18) return fail(error, "Truncated TIBX stream descriptors");
    bool found = false;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint8_t* descriptor = cell.value.data() + 1 + i * 18;
        if (descriptor[0] != 0) continue;
        if (found) return fail(error, "Unsupported TIBX primary stream");
        *id = readBe64(descriptor + 2);
        *size = readBe64(descriptor + 10);
        found = true;
    }
    return found || fail(error, "Missing TIBX primary stream");
}

}  // namespace

bool TibxPageStore::readDirectoryItems(const CommittedHeader& header, const Superblock& itemMap,
                                       std::uint32_t fullId, std::uint32_t sliceId,
                                       const std::string& path, std::vector<Volume>* volumes,
                                       std::vector<FileItem>* files, bool* exists,
                                       std::string* error) const {
    if (!volumes || !files || !exists) return fail(error, "Missing TIBX directory destination");
    std::map<std::string, Volume> found;
    const auto visitVolume = [&](const TreeCell& cell) -> bool {
        if (readBe16(cell.key.data() + 4) != 3) return true;
        if (cell.key[6] != 0) {
            return fail(error, "Unsupported TIBX volume metadata");
        }
        Volume volume;
        if (!primaryStream(cell, &volume.id, &volume.size, error)) return false;
        volume.name.assign(cell.key.begin() + 7, cell.key.end());
        volume.sliceId = readBe32(cell.key.data());
        auto previous = found.find(volume.name);
        if (previous == found.end()) {
            found.insert(std::make_pair(volume.name, volume));
        } else {
            if (previous->second.id != volume.id || previous->second.sliceId != volume.sliceId ||
                previous->second.size != volume.size) return fail(error, "Conflicting TIBX volume entries");
        }
        return true;
    };
    std::map<std::string, FileItem> foundItems;
    *exists = path.empty();
    const auto visitFile = [&](const TreeCell& itemCell) -> bool {
        if (readBe16(itemCell.key.data() + 4) != 1) return true;
        const std::uint8_t kind = itemCell.key[6];
        if (!(kind & (0x04U | 0x08U | 0x10U))) return true;
        const std::string name(itemCell.key.begin() + 7, itemCell.key.end());
        if (name == path && !(kind & 0x08U)) *exists = true;
        const std::string prefix = path.empty() ? std::string() : path + "/";
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) return true;
        *exists = true;
        const std::string suffix = name.substr(prefix.size());
        const std::size_t slash = unescapedSlash(suffix);
        const std::string child = suffix.substr(0, slash);
        if (child.empty()) return fail(error, "Invalid TIBX directory entry");
        FileItem item;
        item.name = prefix + child;
        item.directory = slash != std::string::npos || !(kind & 0x08U);
        item.id = 0;
        item.size = 0;
        item.sliceId = readBe32(itemCell.key.data());
        if (!item.directory) {
            if (!primaryStream(itemCell, &item.id, &item.size, error)) return false;
        }
        const auto previous = foundItems.find(item.name);
        if (previous == foundItems.end()) foundItems.insert(std::make_pair(item.name, item));
        else if (previous->second.directory != item.directory ||
                 (!item.directory && (previous->second.id != item.id || previous->second.size != item.size)))
            return fail(error, "Conflicting TIBX directory entries");
        return true;
    };
    const auto visit = [&](const TreeCell& cell) -> bool { return visitVolume(cell) && visitFile(cell); };
    if (!visitResolvedImap(*this, header, itemMap, fullId, sliceId, visit, error)) return false;
    std::vector<Volume> volumeResult;
    volumeResult.reserve(found.size());
    for (const auto& entry : found) volumeResult.push_back(entry.second);
    std::vector<FileItem> fileResult;
    fileResult.reserve(foundItems.size());
    for (const auto& item : foundItems) fileResult.push_back(item.second);
    volumes->swap(volumeResult);
    files->swap(fileResult);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readResolvedItems(const CommittedHeader& header, const Superblock& itemMap,
                                     std::uint32_t fullId, std::uint32_t sliceId,
                                     std::vector<ResolvedItem>* items, std::string* error) const {
    if (!items) return fail(error, "Missing TIBX resolved items destination");
    std::vector<ResolvedItem> itemResult;
    const auto visit = [&](const TreeCell& cell) -> bool {
        ResolvedItem item;
        item.name.assign(cell.key.begin() + 7, cell.key.end());
        item.sliceId = readBe32(cell.key.data());
        item.flags = readBe16(cell.key.data() + 4);
        item.type = cell.key[6];
        item.descriptorsValid = !cell.value.empty() &&
            std::size_t(cell.value[0]) + 1 <= (cell.value.size() - 1) / 18;
        if (item.descriptorsValid) {
            const std::size_t count = std::size_t(cell.value[0]) + 1;
            item.streams.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const std::uint8_t* descriptor = cell.value.data() + 1 + i * 18;
                StreamDescriptor stream = {descriptor[0], descriptor[1],
                                           readBe64(descriptor + 2), readBe64(descriptor + 10)};
                item.streams.push_back(stream);
            }
        }
        itemResult.push_back(std::move(item));
        return true;
    };
    if (!visitResolvedImap(*this, header, itemMap, fullId, sliceId, visit, error)) return false;
    items->swap(itemResult);
    if (error) error->clear();
    return true;
}

bool TibxPageStore::readVolumeRange(const CommittedHeader& header, const Superblock& segmentMap,
                                    const std::vector<Extent>& extents, std::uint64_t volumeId,
                                    std::uint32_t itemSlice, std::uint64_t offset,
                                    std::uint8_t* output, std::size_t count, std::string* error) const {
    if (count && !output) return fail(error, "Missing TIBX volume destination");
    if (count > std::numeric_limits<std::uint64_t>::max() - offset) {
        return fail(error, "Invalid TIBX volume range");
    }
    if (!count) {
        if (error) error->clear();
        return true;
    }
    std::fill(output, output + count, std::uint8_t(0));
    const std::uint64_t end = offset + count;
    std::uint64_t cachedId = 0;
    std::shared_ptr<const std::vector<std::uint8_t>> segment;
    struct Boundary { std::uint64_t position; const Extent* extent; bool start; };
    std::vector<Boundary> boundaries;
    for (const Extent& extent : extents) {
        if (extent.volumeId != volumeId || extent.sliceId > itemSlice) continue;
        if (extent.length > std::numeric_limits<std::uint64_t>::max() - extent.sourceOffset)
            return fail(error, "Invalid TIBX extent range");
        const std::uint64_t begin = std::max(offset, extent.sourceOffset);
        const std::uint64_t finish = std::min(end, extent.sourceOffset + extent.length);
        if (begin < finish) {
            boundaries.push_back(Boundary{begin, &extent, true});
            boundaries.push_back(Boundary{finish, &extent, false});
        }
    }
    std::sort(boundaries.begin(), boundaries.end(), [](const Boundary& a, const Boundary& b) {
        return a.position < b.position;
    });
    const auto priority = [](const Extent* a, const Extent* b) {
        if (a->sliceId != b->sliceId) return a->sliceId < b->sliceId;
        if (a->extentId != b->extentId) return a->extentId < b->extentId;
        // Extents point into the same vector; the first encountered wins ties.
        return std::less<const Extent*>()(b, a);
    };
    std::set<const Extent*, decltype(priority)> active(priority);
    std::size_t i = 0;
    while (i < boundaries.size()) {
        const std::uint64_t begin = boundaries[i].position;
        do {
            if (boundaries[i].start) active.insert(boundaries[i].extent);
            else active.erase(boundaries[i].extent);
            ++i;
        } while (i < boundaries.size() && boundaries[i].position == begin);
        if (i == boundaries.size() || active.empty()) continue;
        const std::uint64_t finish = boundaries[i].position;
        auto winner = active.rbegin();
        const Extent& extent = **winner;
        if (!extent.segmentId) continue;
        if (extent.segmentId != cachedId) {
            if (!readSegment(header, segmentMap, extent.segmentId, &segment, error)) return false;
            cachedId = extent.segmentId;
        }
        const std::uint64_t base = extent.segmentIndex == 0xffffU
                                       ? 0 : std::uint64_t(extent.segmentIndex) * 16;
        if (base > segment->size() || extent.length > segment->size() - base)
            return fail(error, "TIBX extent exceeds its segment");
        const std::size_t source = static_cast<std::size_t>(base + (begin - extent.sourceOffset));
        const std::size_t destination = static_cast<std::size_t>(begin - offset);
        const std::size_t length = static_cast<std::size_t>(finish - begin);
        std::copy(segment->begin() + source, segment->begin() + source + length,
                  output + destination);
    }
    if (error) error->clear();
    return true;
}

}  // namespace fs
}  // namespace peare
