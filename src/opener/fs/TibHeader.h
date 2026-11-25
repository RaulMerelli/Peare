#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {
namespace tibheader {

struct Header {
    std::uint16_t size;
    std::uint16_t version;
    std::uint64_t archiveId;
    std::uint32_t volumeId;
    std::uint32_t number;
    std::uint32_t dataOffset;
};

inline std::uint16_t le16(const std::uint8_t* p) {
    return std::uint16_t(p[0]) | (std::uint16_t(p[1]) << 8);
}

inline std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

inline bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

inline bool parse(const std::vector<std::uint8_t>& bytes, bool normalizeOffset,
                  Header* header, std::string* error) {
    if (!header || bytes.size() < 24 || le32(bytes.data()) != 0xa2b924ceU ||
        le16(bytes.data() + 4) != bytes.size())
        return fail(error, "Invalid TIB header structure");
    const std::uint16_t size = le16(bytes.data() + 4);
    if ((size > 24 && size < 28) || (size > 28 && size < 32))
        return fail(error, "Truncated TIB header field");
    const std::uint32_t storedOffset = size >= 32 ? le32(bytes.data() + 28) : 0;
    if (size > 24) {
        const std::uint32_t expected = le32(bytes.data() + 24);
        std::uint32_t a = 1, b = 0;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            std::uint8_t value = bytes[i];
            if (i >= 24 && i < 28) value = 0;
            else if (normalizeOffset && size >= 32 && !storedOffset && i >= 28 && i < 32)
                value = static_cast<std::uint8_t>(std::uint32_t(size) >> (8 * (i - 28)));
            a = (a + value) % 65521U;
            b = (b + a) % 65521U;
        }
        if (((b << 16) | a) != expected) return fail(error, "TIB header checksum mismatch");
    }
    header->size = size;
    header->version = le16(bytes.data() + 6);
    header->archiveId = std::uint64_t(le32(bytes.data() + 8)) |
                        (std::uint64_t(le32(bytes.data() + 12)) << 32);
    header->volumeId = le32(bytes.data() + 16);
    header->number = le32(bytes.data() + 20);
    header->dataOffset = storedOffset ? storedOffset : size;
    return true;
}

inline bool read(const ByteStorePtr& source, Header* header, std::string* error) {
    if (!source || source->capacity() < 6) return fail(error, "Missing TIB header");
    std::uint8_t prefix[6];
    int done = 0;
    while (done < 6) {
        const int n = source->read(done, prefix + done, 6 - done);
        if (n <= 0 || n > 6 - done) return fail(error, "Cannot read TIB header");
        done += n;
    }
    const std::uint16_t size = le16(prefix + 4);
    if (le32(prefix) != 0xa2b924ceU || size < 24 || size > source->capacity())
        return fail(error, "Invalid TIB header size or signature");
    std::vector<std::uint8_t> bytes(size);
    done = 0;
    while (done < size) {
        const int n = source->read(done, bytes.data() + done, size - done);
        if (n <= 0 || n > size - done) return fail(error, "Cannot read TIB header");
        done += n;
    }
    return parse(bytes, true, header, error);
}

} // namespace tibheader
} // namespace fs
} // namespace peare
