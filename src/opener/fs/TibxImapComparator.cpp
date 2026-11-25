#include "TibxImapComparator.h"

#include <algorithm>
#include <cstddef>

namespace peare {
namespace fs {
namespace {

struct UppercaseMapping {
    std::uint32_t source;
    std::uint32_t upper;
};

const UppercaseMapping kUppercase[] = {
#include "TibxUnicodeUpper8.inc"
};

std::uint32_t uppercase(std::uint32_t value) {
    if (value >= 'a' && value <= 'z') return value - 'a' + 'A';
    const auto* end = kUppercase + sizeof(kUppercase) / sizeof(kUppercase[0]);
    const auto* found = std::lower_bound(
        kUppercase, end, value,
        [](const UppercaseMapping& entry, std::uint32_t codepoint) {
            return entry.source < codepoint;
        });
    return found != end && found->source == value ? found->upper : value;
}

bool readCodepoint(const std::vector<std::uint8_t>& key, std::size_t* offset,
                   std::uint32_t* value) {
    if (*offset >= key.size()) return false;
    const std::uint8_t first = key[(*offset)++];
    if (first < 0x80U) { *value = first; return true; }
    unsigned int extra;
    std::uint32_t decoded;
    std::uint32_t minimum;
    if (first >= 0xc2U && first <= 0xdfU) {
        extra = 1; decoded = first & 0x1fU; minimum = 0x80U;
    } else if (first >= 0xe0U && first <= 0xefU) {
        extra = 2; decoded = first & 0x0fU; minimum = 0x800U;
    } else if (first >= 0xf0U && first <= 0xf4U) {
        extra = 3; decoded = first & 0x07U; minimum = 0x10000U;
    } else {
        return false;
    }
    if (key.size() - *offset < extra) return false;
    for (unsigned int i = 0; i < extra; ++i) {
        const std::uint8_t next = key[(*offset)++];
        if ((next & 0xc0U) != 0x80U) return false;
        decoded = (decoded << 6) | (next & 0x3fU);
    }
    if (decoded < minimum || decoded > 0x10ffffU) return false;
    *value = decoded;
    return true;
}

struct NameCharacter {
    std::uint32_t codepoint;
    bool escaped;
    bool separatorAfter;
};

bool decodeName(const std::vector<std::uint8_t>& key, std::vector<NameCharacter>* name) {
    std::size_t offset = 7;
    while (offset < key.size()) {
        std::uint32_t value;
        if (!readCodepoint(key, &offset, &value)) return false;
        bool escaped = false;
        if (value == '\\') {
            escaped = true;
            if (!readCodepoint(key, &offset, &value)) return false;
        }
        name->push_back(NameCharacter{value, escaped, false});
    }
    bool separator = false;
    for (std::size_t i = name->size(); i > 0; --i) {
        NameCharacter& character = (*name)[i - 1];
        character.separatorAfter = separator;
        if (character.codepoint == '/' && !character.escaped) separator = true;
    }
    return true;
}

int compareHierarchical(const std::vector<NameCharacter>& left,
                        const std::vector<NameCharacter>& right) {
    std::size_t i = 0;
    while (i < left.size() && i < right.size()) {
        const NameCharacter& a = left[i];
        const NameCharacter& b = right[i];
        const bool aSlash = a.codepoint == '/' && !a.escaped;
        const bool bSlash = b.codepoint == '/' && !b.escaped;
        if (a.codepoint == b.codepoint && aSlash == bSlash) { ++i; continue; }
        if (aSlash) return b.separatorAfter ? -1 : 1;
        if (bSlash) return a.separatorAfter ? 1 : -1;
        if (a.separatorAfter != b.separatorAfter) return a.separatorAfter ? 1 : -1;
        const std::uint32_t upperA = uppercase(a.codepoint);
        const std::uint32_t upperB = uppercase(b.codepoint);
        if (upperA != upperB) return upperA < upperB ? -1 : 1;
        if (a.codepoint != b.codepoint) return a.codepoint < b.codepoint ? -1 : 1;
        ++i;
    }
    if (left.size() == right.size()) return 0;
    return left.size() < right.size() ? -1 : 1;
}

std::uint32_t sliceId(const std::vector<std::uint8_t>& key) {
    return (std::uint32_t(key[0]) << 24) | (std::uint32_t(key[1]) << 16) |
           (std::uint32_t(key[2]) << 8) | std::uint32_t(key[3]);
}

}  // namespace

bool compareTibxImapKeys(const std::vector<std::uint8_t>& left,
                         const std::vector<std::uint8_t>& right,
                         TibxImapSort mode, int* result) {
    if (!result || left.size() < 7 || right.size() < 7) return false;
    int order;
    if (mode == TibxImapSort::Plain) {
        const auto a = left.begin() + 7;
        const auto b = right.begin() + 7;
        order = std::lexicographical_compare(a, left.end(), b, right.end()) ? -1 :
                std::lexicographical_compare(b, right.end(), a, left.end()) ? 1 : 0;
    } else if (mode == TibxImapSort::Hierarchical) {
        std::vector<NameCharacter> a, b;
        if (!decodeName(left, &a) || !decodeName(right, &b)) return false;
        order = compareHierarchical(a, b);
    } else {
        return false;
    }
    if (!order) {
        const std::uint32_t a = sliceId(left);
        const std::uint32_t b = sliceId(right);
        order = a == b ? 0 : (a > b ? -1 : 1);
    }
    *result = order;
    return true;
}

}  // namespace fs
}  // namespace peare
