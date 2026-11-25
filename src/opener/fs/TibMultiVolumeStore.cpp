#include "TibMultiVolumeStore.h"
#include "TibHeader.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace peare {
namespace fs {
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool readExact(const ByteStorePtr& source, std::int64_t offset,
               std::uint8_t* output, int size) {
    if (!source || offset < 0 || size < 0 || offset > source->capacity() ||
        size > source->capacity() - offset) return false;
    int done = 0;
    while (done < size) {
        const int amount = source->read(offset + done, output + done, size - done);
        if (amount <= 0 || amount > size - done) return false;
        done += amount;
    }
    return true;
}

ByteStorePtr fail(std::string* error, const char* message) {
    if (error) *error = message;
    return ByteStorePtr();
}

struct Part {
    ByteStorePtr source;
    std::int64_t dataOffset;
    std::int64_t end;
    std::uint32_t volumeId;
};

bool readLegacyHeader(const ByteStorePtr& source, std::uint8_t header[12],
                      std::uint32_t* archiveId, std::uint32_t* number) {
    if (!source || source->capacity() < 12 || !readExact(source, 0, header, 12) ||
        le32(header) != 0x44686eb4U) return false;
    if (archiveId) *archiveId = le32(header + 4);
    if (number) *number = le32(header + 8);
    return true;
}

class TibMultiVolumeStore final : public IByteStore {
public:
    TibMultiVolumeStore(std::vector<Part> parts, std::int64_t length)
        : parts_(std::move(parts)), length_(length) {}

    std::int64_t capacity() const override { return length_; }

    bool validate() const override {
        for (const Part& part : parts_)
            if (!part.source->validate()) return false;
        return true;
    }

    bool cheapRandomAccess() const override {
        for (const Part& part : parts_)
            if (!part.source->cheapRandomAccess()) return false;
        return true;
    }

    int read(std::int64_t pos, std::uint8_t* output, int count) const override {
        if (!output || pos < 0 || count <= 0 || pos >= length_) return 0;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, length_ - pos));
        auto part = std::upper_bound(parts_.begin(), parts_.end(), pos,
            [](std::int64_t value, const Part& entry) { return value < entry.end; });
        int done = 0;
        while (done < amount) {
            if (part == parts_.end()) return -1;
            const std::int64_t logicalStart = part == parts_.begin() ? 0 : (part - 1)->end;
            const std::int64_t available = part->end - pos;
            if (available <= 0) { ++part; continue; }
            const int chunk = static_cast<int>(std::min<std::int64_t>(amount - done, available));
            if (!readExact(part->source, part->dataOffset + (pos - logicalStart),
                           output + done, chunk)) return -1;
            done += chunk;
            pos += chunk;
            ++part;
        }
        return amount;
    }

private:
    std::vector<Part> parts_;
    std::int64_t length_;
};

} // namespace

ByteStorePtr openTibMultiVolume(const std::vector<ByteStorePtr>& sources,
                                std::string* error) {
    if (error) error->clear();
    if (sources.empty() || sources.size() > std::numeric_limits<std::uint32_t>::max())
        return fail(error, "Invalid TIB volume count");
    try {
        std::vector<Part> parts;
        parts.reserve(sources.size());
        std::uint8_t firstLegacyHeader[12];
        std::uint32_t legacyArchiveId = 0;
        std::uint32_t legacyNumber = 0;
        if (readLegacyHeader(sources.front(), firstLegacyHeader,
                             &legacyArchiveId, &legacyNumber)) {
            if (sources.size() < 2)
                return fail(error, "Legacy TIB multi-volume input requires multiple volumes");
            if (legacyNumber != 1) return fail(error, "Legacy TIB volumes must start at volume 1");
            std::uint64_t total = 12;
            parts.push_back(Part{std::make_shared<MemoryStore>(firstLegacyHeader, 12), 0, 12, 0});
            for (std::size_t i = 0; i < sources.size(); ++i) {
                std::uint8_t header[12];
                std::uint32_t archiveId = 0;
                std::uint32_t number = 0;
                if (!sources[i] || !sources[i]->validate() ||
                    !readLegacyHeader(sources[i], header, &archiveId, &number))
                    return fail(error, "Missing or invalid legacy TIB volume");
                if (archiveId != legacyArchiveId || number != i + 1)
                    return fail(error, "Legacy TIB volume identity or order mismatch");
            }
            const std::uint64_t tableSize = std::uint64_t(sources.size()) * 8 + 4;
            const std::int64_t lastSize = sources.back()->capacity();
            if (tableSize > std::uint64_t(lastSize) || lastSize < 12)
                return fail(error, "Invalid legacy TIB volume table bounds");
            const std::int64_t tableStart = lastSize - static_cast<std::int64_t>(tableSize);
            std::uint8_t marker[4];
            if (!readExact(sources.back(), lastSize - 4, marker, 4) ||
                le32(marker) != 0x179631b4U)
                return fail(error, "Invalid legacy TIB volume footer");
            for (std::size_t i = 0; i < sources.size(); ++i) {
                std::uint8_t entry[8];
                if (!readExact(sources.back(), tableStart + static_cast<std::int64_t>(i * 8), entry, 8))
                    return fail(error, "Cannot read legacy TIB volume table");
                const std::uint64_t length = le64(entry);
                const std::int64_t physicalEnd = i + 1 == sources.size()
                    ? tableStart : sources[i]->capacity();
                if (physicalEnd < 12 || length > std::uint64_t(physicalEnd - 12) ||
                    (i + 1 < sources.size() && length != std::uint64_t(physicalEnd - 12)) ||
                    length > std::uint64_t(std::numeric_limits<std::int64_t>::max() - total))
                    return fail(error, "Invalid legacy TIB volume length");
                total += length;
                parts.push_back(Part{sources[i], 12, static_cast<std::int64_t>(total), 0});
            }
            if (error) error->clear();
            return std::make_shared<TibMultiVolumeStore>(std::move(parts),
                                                         static_cast<std::int64_t>(total));
        }
        std::uint64_t archiveId = 0;
        tibheader::Header lastHeader = {};
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const ByteStorePtr& source = sources[i];
            if (!source || !source->validate() || source->capacity() < 24)
                return fail(error, "Missing or unreadable TIB volume");
            tibheader::Header header;
            if (!tibheader::read(source, &header, error)) return ByteStorePtr();
            if (header.size > 36)
                return fail(error, "Invalid TIB volume header");
            const std::uint64_t id = header.archiveId;
            if (i == 0) archiveId = id;
            if (id != archiveId || header.number != i + 1)
                return fail(error, "TIB volume identity or order mismatch");
            const std::uint32_t dataOffset = header.dataOffset;
            if (dataOffset < header.size || dataOffset > source->capacity())
                return fail(error, "Invalid TIB volume data offset");
            Part part = {source, static_cast<std::int64_t>(dataOffset), 0, header.volumeId};
            parts.push_back(std::move(part));
            if (i + 1 == sources.size()) lastHeader = header;
        }

        const ByteStorePtr& last = sources.back();
        const std::int64_t size = last->capacity();
        std::uint8_t suffix[6];
        if (!readExact(last, size - 6, suffix, 6)) return fail(error, "Cannot read TIB footer");
        std::reverse(suffix, suffix + 6);
        const std::uint16_t footerSize = le16(suffix + 4);
        if (le32(suffix) != 0xa2b924ceU || footerSize < 24 || footerSize > 36 || footerSize > size)
            return fail(error, "Invalid TIB footer");
        std::vector<std::uint8_t> footer(footerSize);
        const std::int64_t footerStart = size - footerSize;
        if (!readExact(last, footerStart, footer.data(), footerSize))
            return fail(error, "Cannot read TIB footer");
        std::reverse(footer.begin(), footer.end());
        tibheader::Header footerHeader;
        if (!tibheader::parse(footer, false, &footerHeader, error)) return ByteStorePtr();
        if (footerHeader.archiveId != lastHeader.archiveId ||
            footerHeader.volumeId != lastHeader.volumeId || footerHeader.number != lastHeader.number)
            return fail(error, "TIB footer identity does not match final header");
        const std::uint64_t tableLength = std::uint64_t(parts.size()) * 12;
        if (tableLength > std::uint64_t(footerStart) ||
            footerStart - static_cast<std::int64_t>(tableLength) < parts.back().dataOffset)
            return fail(error, "Invalid TIB volume table bounds");
        const std::int64_t tableStart = footerStart - static_cast<std::int64_t>(tableLength);
        std::int64_t total = 0;
        for (std::size_t i = 0; i < parts.size(); ++i) {
            std::uint8_t entry[12];
            if (!readExact(last, tableStart + static_cast<std::int64_t>(i * 12ULL), entry, 12))
                return fail(error, "Cannot read TIB volume table");
            if (le32(entry) != parts[i].volumeId) return fail(error, "TIB volume ID mismatch");
            const std::uint64_t length = le64(entry + 4);
            const std::int64_t physicalEnd = i + 1 == parts.size() ? tableStart : parts[i].source->capacity();
            if (parts[i].dataOffset > physicalEnd ||
                length > std::uint64_t(physicalEnd - parts[i].dataOffset) ||
                length > std::uint64_t(std::numeric_limits<std::int64_t>::max() - total))
                return fail(error, "Invalid TIB volume length");
            total += static_cast<std::int64_t>(length);
            parts[i].end = total;
        }
        return std::make_shared<TibMultiVolumeStore>(std::move(parts), total);
    } catch (const std::bad_alloc&) {
        return fail(error, "TIB volume allocation failed");
    } catch (const std::length_error&) {
        return fail(error, "TIB volume count is not representable");
    }
}

} // namespace fs
} // namespace peare
