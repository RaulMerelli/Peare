#include "TibLegacyFileTree.h"

#include <algorithm>
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

class Cursor {
public:
    explicit Cursor(const std::vector<std::uint8_t>& bytes) : bytes_(bytes), pos_(0) {}
    bool take(std::size_t n, const std::uint8_t** p) {
        if (n > bytes_.size() - pos_) return false;
        *p = bytes_.data() + pos_;
        pos_ += n;
        return true;
    }
    bool number32(std::uint32_t* value) {
        const std::uint8_t* p;
        if (!take(4, &p)) return false;
        *value = std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
                 (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
        return true;
    }
    bool number64(std::uint64_t* value) {
        std::uint32_t low, high;
        if (!number32(&low) || !number32(&high)) return false;
        *value = std::uint64_t(low) | (std::uint64_t(high) << 32);
        return true;
    }
    bool text(std::u16string* output) {
        std::uint32_t units;
        if (!number32(&units) || units > (bytes_.size() - pos_) / 2 || units > output->max_size()) return false;
        const std::uint8_t* p;
        if (!take(static_cast<std::size_t>(units) * 2, &p)) return false;
        output->reserve(units);
        for (std::uint32_t i = 0; i < units; ++i)
            output->push_back(static_cast<char16_t>(std::uint16_t(p[std::size_t(i) * 2]) |
                              (std::uint16_t(p[std::size_t(i) * 2 + 1]) << 8)));
        return true;
    }
    std::size_t position() const { return pos_; }
    std::size_t remaining() const { return bytes_.size() - pos_; }
private:
    const std::vector<std::uint8_t>& bytes_;
    std::size_t pos_;
};

bool atLeast(TibFileMetadataVersion v, std::uint32_t major, std::uint32_t minor) {
    return v.major > major || (v.major == major && v.minor >= minor);
}

struct Frame {
    std::size_t node;
    std::uint32_t remaining;
};

}  // namespace

bool parseTibLegacyFileTree(const std::vector<std::uint8_t>& decoded,
                            TibFileMetadataVersion version,
                            std::vector<TibLegacyFileNode>* nodes,
                            std::size_t* consumed, std::string* error) {
    if (error) error->clear();
    if (!nodes || !consumed) return fail(error, "Missing TIB legacy tree destination");
    nodes->clear();
    *consumed = 0;
    if (version.major >= 4) return fail(error, "Unsupported TIB legacy tree version");
    try {
        Cursor c(decoded);
        std::uint32_t rootCount;
        if (!c.number32(&rootCount) || rootCount > c.remaining() / 52)
            return fail(error, "Invalid TIB legacy root count");
        const std::size_t root = std::numeric_limits<std::size_t>::max();
        std::vector<TibLegacyFileNode> result;
        std::vector<Frame> stack;
        stack.push_back(Frame{root, rootCount});
        while (!stack.empty()) {
            if (stack.back().remaining) {
                const std::size_t parent = stack.back().node;
                --stack.back().remaining;
                std::uint32_t children;
                if (!c.number32(&children) || children > c.remaining() / 52)
                    return fail(error, "Invalid TIB legacy child count");
                if (result.size() == result.max_size() || stack.size() == stack.max_size())
                    return fail(error, "TIB legacy tree is not representable");
                TibLegacyFileNode node = {};
                node.parent = parent;
                result.push_back(std::move(node));
                stack.push_back(Frame{result.size() - 1, children});
                continue;
            }
            const std::size_t index = stack.back().node;
            stack.pop_back();
            if (index == root) continue;
            TibLegacyFileNode& node = result[index];
            if (!c.text(&node.name) || !c.text(&node.extendedName) ||
                !c.number64(&node.fieldA) || !c.number32(&node.fieldB) ||
                !c.number64(&node.recordPosition) || !c.number64(&node.fieldD) ||
                !c.number32(&node.fieldE) || !c.number32(&node.fieldF) || !c.number32(&node.fieldG))
                return fail(error, "Truncated TIB legacy node metadata");
            node.typeCode = static_cast<std::uint8_t>(node.fieldF & 0x0fU);
            if (atLeast(version, 1, 3)) {
                node.hasField13 = true;
                if (!c.number64(&node.field13)) return fail(error, "Truncated TIB legacy version 1.3 field");
            }
            if (atLeast(version, 1, 4)) {
                node.hasField14 = true;
                std::uint32_t length;
                const std::uint8_t* bytes;
                if (!c.number32(&node.field14) || !c.number32(&length) || !c.take(length, &bytes))
                    return fail(error, "Truncated TIB legacy version 1.4 fields");
                node.string14.assign(bytes, bytes + length);
            }
            if (atLeast(version, 3, 1)) {
                const std::uint8_t* value;
                node.hasField31 = true;
                if (!c.take(1, &value)) return fail(error, "Truncated TIB legacy version 3.1 field");
                node.field31 = *value;
            }
            std::replace(node.name.begin(), node.name.end(), u'\\', u'/');
            if (node.name.size() > 1 && node.name.back() == u'/') node.name.pop_back();
        }
        for (std::size_t i = 0; i < result.size(); ++i) {
            TibLegacyFileNode& node = result[i];
            if (node.parent != root) node.path = result[node.parent].path;
            if (node.name.size() > node.path.max_size() - node.path.size())
                return fail(error, "TIB legacy path is not representable");
            node.path += node.name;
        }
        *consumed = c.position();
        *nodes = std::move(result);
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "TIB legacy tree allocation failed");
    } catch (const std::length_error&) {
        return fail(error, "TIB legacy tree size is not representable");
    }
}

bool parseTibLegacyHashChunk(const std::vector<std::uint8_t>& decoded,
                             TibLegacyChunkHash* hash, std::size_t* consumed,
                             std::string* error) {
    if (error) error->clear();
    if (!hash || !consumed) return fail(error, "Missing TIB legacy hash destination");
    *consumed = 0;
    hash->algorithm = 0;
    hash->bytes.clear();
    if (decoded.size() < 3) return fail(error, "Truncated TIB legacy hash header");
    const std::size_t length = std::size_t(decoded[1]) | (std::size_t(decoded[2]) << 8);
    if (!length || length > decoded.size() - 3)
        return fail(error, "Invalid TIB legacy hash length");
    try {
        hash->bytes.assign(decoded.begin() + 3, decoded.begin() + 3 + length);
        hash->algorithm = decoded[0];
        *consumed = 3 + length;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "TIB legacy hash allocation failed");
    } catch (const std::length_error&) {
        return fail(error, "TIB legacy hash size is not representable");
    }
}

bool parseTibLegacyChunk5(const std::vector<std::uint8_t>& decoded,
                          std::vector<std::uint8_t>* value, std::size_t* consumed,
                          std::string* error) {
    if (error) error->clear();
    if (!value || !consumed) return fail(error, "Missing TIB legacy chunk destination");
    *consumed = 0;
    value->clear();
    if (decoded.size() < 16) return fail(error, "Truncated TIB legacy chunk 5");
    try {
        value->assign(decoded.begin(), decoded.begin() + 16);
        *consumed = 16;
        return true;
    } catch (const std::bad_alloc&) {
        return fail(error, "TIB legacy chunk allocation failed");
    } catch (const std::length_error&) {
        return fail(error, "TIB legacy chunk size is not representable");
    }
}

}  // namespace fs
}  // namespace peare
