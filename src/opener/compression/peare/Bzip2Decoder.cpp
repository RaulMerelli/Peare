#include "Bzip2Decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <future>
#include <memory>
#include <utility>

namespace peare {
namespace compression {
namespace {

const std::uint64_t kBlockMagic = UINT64_C(0x314159265359);
const std::uint64_t kEndMagic = UINT64_C(0x177245385090);
const std::size_t kMaxSelectors = 18002;
const unsigned kMaxCodeLength = 20;
// Legacy de-randomization values are attributed in THIRDPARTY.md.
const std::array<std::uint16_t, 512> kLegacyRandomization = {{
    619, 720, 127, 481, 931, 816, 813, 233, 566, 247, 985, 724, 205, 454, 863, 491,
    741, 242, 949, 214, 733, 859, 335, 708, 621, 574, 73, 654, 730, 472, 419, 436,
    278, 496, 867, 210, 399, 680, 480, 51, 878, 465, 811, 169, 869, 675, 611, 697,
    867, 561, 862, 687, 507, 283, 482, 129, 807, 591, 733, 623, 150, 238, 59, 379,
    684, 877, 625, 169, 643, 105, 170, 607, 520, 932, 727, 476, 693, 425, 174, 647,
    73, 122, 335, 530, 442, 853, 695, 249, 445, 515, 909, 545, 703, 919, 874, 474,
    882, 500, 594, 612, 641, 801, 220, 162, 819, 984, 589, 513, 495, 799, 161, 604,
    958, 533, 221, 400, 386, 867, 600, 782, 382, 596, 414, 171, 516, 375, 682, 485,
    911, 276, 98, 553, 163, 354, 666, 933, 424, 341, 533, 870, 227, 730, 475, 186,
    263, 647, 537, 686, 600, 224, 469, 68, 770, 919, 190, 373, 294, 822, 808, 206,
    184, 943, 795, 384, 383, 461, 404, 758, 839, 887, 715, 67, 618, 276, 204, 918,
    873, 777, 604, 560, 951, 160, 578, 722, 79, 804, 96, 409, 713, 940, 652, 934,
    970, 447, 318, 353, 859, 672, 112, 785, 645, 863, 803, 350, 139, 93, 354, 99,
    820, 908, 609, 772, 154, 274, 580, 184, 79, 626, 630, 742, 653, 282, 762, 623,
    680, 81, 927, 626, 789, 125, 411, 521, 938, 300, 821, 78, 343, 175, 128, 250,
    170, 774, 972, 275, 999, 639, 495, 78, 352, 126, 857, 956, 358, 619, 580, 124,
    737, 594, 701, 612, 669, 112, 134, 694, 363, 992, 809, 743, 168, 974, 944, 375,
    748, 52, 600, 747, 642, 182, 862, 81, 344, 805, 988, 739, 511, 655, 814, 334,
    249, 515, 897, 955, 664, 981, 649, 113, 974, 459, 893, 228, 433, 837, 553, 268,
    926, 240, 102, 654, 459, 51, 686, 754, 806, 760, 493, 403, 415, 394, 687, 700,
    946, 670, 656, 610, 738, 392, 760, 799, 887, 653, 978, 321, 576, 617, 626, 502,
    894, 679, 243, 440, 680, 879, 194, 572, 640, 724, 926, 56, 204, 700, 707, 151,
    457, 449, 797, 195, 791, 558, 945, 679, 297, 59, 87, 824, 713, 663, 412, 693,
    342, 606, 134, 108, 571, 364, 631, 212, 174, 643, 304, 329, 343, 97, 430, 751,
    497, 314, 983, 374, 822, 928, 140, 206, 73, 263, 980, 736, 876, 478, 430, 305,
    170, 514, 364, 692, 829, 82, 855, 953, 676, 246, 369, 970, 294, 750, 807, 827,
    150, 790, 288, 923, 804, 378, 215, 828, 592, 281, 565, 555, 710, 82, 896, 831,
    547, 261, 524, 462, 293, 465, 502, 56, 661, 821, 976, 991, 658, 869, 905, 758,
    745, 193, 768, 550, 608, 933, 378, 286, 215, 979, 792, 961, 61, 688, 793, 644,
    986, 403, 106, 366, 905, 644, 372, 567, 466, 434, 645, 210, 389, 550, 919, 135,
    780, 773, 635, 389, 707, 100, 626, 958, 165, 504, 920, 176, 193, 713, 857, 265,
    203, 50, 668, 108, 645, 990, 626, 197, 510, 357, 358, 850, 858, 364, 936, 638
}};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), offset_(0), buffer_(0), available_(0) {}

    bool read(unsigned count, std::uint32_t* value) {
        if (!value || count > 32) return false;
        while (available_ < count) {
            if (offset_ >= size_) return false;
            buffer_ = (buffer_ << 8) | data_[offset_++];
            available_ += 8;
        }
        const unsigned shift = available_ - count;
        const std::uint64_t mask = count == 32 ? UINT64_C(0xffffffff)
                                                : ((UINT64_C(1) << count) - 1);
        *value = static_cast<std::uint32_t>((buffer_ >> shift) & mask);
        available_ -= count;
        if (available_ == 0) buffer_ = 0;
        else buffer_ &= (UINT64_C(1) << available_) - 1;
        return true;
    }

    bool peek(unsigned count, std::uint32_t* value) {
        if (!value || count > 32) return false;
        while (available_ < count) {
            if (offset_ >= size_) return false;
            buffer_ = (buffer_ << 8) | data_[offset_++];
            available_ += 8;
        }
        const unsigned shift = available_ - count;
        const std::uint64_t mask = count == 32 ? UINT64_C(0xffffffff)
                                                : ((UINT64_C(1) << count) - 1);
        *value = static_cast<std::uint32_t>((buffer_ >> shift) & mask);
        return true;
    }

    void drop(unsigned count) {
        available_ -= count;
        if (available_ == 0) buffer_ = 0;
        else buffer_ &= (UINT64_C(1) << available_) - 1;
    }

    bool read64(unsigned count, std::uint64_t* value) {
        if (!value || count > 64) return false;
        std::uint32_t high = 0;
        std::uint32_t low = 0;
        const unsigned highBits = count > 32 ? count - 32 : 0;
        const unsigned lowBits = count - highBits;
        if ((highBits && !read(highBits, &high)) || !read(lowBits, &low)) return false;
        *value = (std::uint64_t(high) << lowBits) | low;
        return true;
    }

    void alignByte() {
        available_ = 0;
        buffer_ = 0;
    }

    std::size_t byteOffset() const { return offset_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
    std::uint64_t buffer_;
    unsigned available_;
};

struct HuffmanTree {
    static const unsigned kFastBits = 10;
    std::array<std::uint32_t, kMaxCodeLength + 1> firstCode{};
    std::array<std::uint32_t, kMaxCodeLength + 1> firstIndex{};
    std::array<std::uint32_t, kMaxCodeLength + 1> counts{};
    std::array<std::uint16_t, 1U << kFastBits> fast{};
    std::vector<std::uint16_t> symbols;
    unsigned minimum = 0;
    unsigned maximum = 0;

    bool build(const std::vector<std::uint8_t>& lengths) {
        counts.fill(0);
        minimum = kMaxCodeLength + 1;
        maximum = 0;
        for (std::uint8_t length : lengths) {
            if (length == 0 || length > kMaxCodeLength) return false;
            ++counts[length];
            minimum = std::min(minimum, unsigned(length));
            maximum = std::max(maximum, unsigned(length));
        }
        symbols.clear();
        symbols.reserve(lengths.size());
        std::uint32_t code = 0;
        std::uint32_t index = 0;
        for (unsigned length = 1; length <= maximum; ++length) {
            firstCode[length] = code;
            firstIndex[length] = index;
            code += counts[length];
            if (code > (UINT32_C(1) << length)) return false;
            index += counts[length];
            code <<= 1;
        }
        for (unsigned length = 1; length <= maximum; ++length)
            for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol)
                if (lengths[symbol] == length)
                    symbols.push_back(static_cast<std::uint16_t>(symbol));
        if (symbols.size() != lengths.size() || minimum > maximum) return false;
        fast.fill(0);
        for (unsigned length = 1; length <= std::min(maximum, kFastBits); ++length) {
            const unsigned suffixBits = kFastBits - length;
            const unsigned repeat = 1U << suffixBits;
            for (std::uint32_t offset = 0; offset < counts[length]; ++offset) {
                const std::uint32_t code = firstCode[length] + offset;
                const std::uint32_t base = code << suffixBits;
                const std::uint16_t entry = static_cast<std::uint16_t>(
                    (symbols[firstIndex[length] + offset] << 5) | length);
                for (std::uint32_t suffix = 0; suffix < repeat; ++suffix)
                    fast[base + suffix] = entry;
            }
        }
        return true;
    }

    bool decode(BitReader& bits, std::uint32_t* symbol) const {
        if (!symbol || minimum == 0 || maximum > kMaxCodeLength) return false;
        std::uint32_t prefix = 0;
        if (bits.peek(kFastBits, &prefix)) {
            const std::uint16_t entry = fast[prefix];
            const unsigned length = entry & 0x1FU;
            if (length != 0) {
                bits.drop(length);
                *symbol = entry >> 5;
                return true;
            }
            std::uint32_t code = prefix;
            bits.drop(kFastBits);
            for (unsigned codeLength = kFastBits + 1;
                 codeLength <= maximum; ++codeLength) {
                std::uint32_t bit = 0;
                if (!bits.read(1, &bit)) return false;
                code = (code << 1) | bit;
                if (code >= firstCode[codeLength]) {
                    const std::uint32_t delta = code - firstCode[codeLength];
                    if (delta < counts[codeLength]) {
                        const std::size_t position = firstIndex[codeLength] + delta;
                        if (position >= symbols.size()) return false;
                        *symbol = symbols[position];
                        return true;
                    }
                }
            }
            return false;
        }
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= maximum; ++length) {
            std::uint32_t bit = 0;
            if (!bits.read(1, &bit)) return false;
            code = (code << 1) | bit;
            if (length < minimum || code < firstCode[length]) continue;
            const std::uint32_t delta = code - firstCode[length];
            if (delta < counts[length]) {
                const std::size_t position = firstIndex[length] + delta;
                if (position >= symbols.size()) return false;
                *symbol = symbols[position];
                return true;
            }
        }
        return false;
    }
};

const std::array<std::array<std::uint32_t, 256>, 8>& crcTable() {
    static const std::array<std::array<std::uint32_t, 256>, 8> table = [] {
        std::array<std::array<std::uint32_t, 256>, 8> values{};
        for (std::uint32_t i = 0; i < values[0].size(); ++i) {
            std::uint32_t crc = i << 24;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04C11DB7U : crc << 1;
            values[0][i] = crc;
        }
        for (std::size_t slice = 1; slice < values.size(); ++slice)
            for (std::size_t i = 0; i < values[slice].size(); ++i) {
                const std::uint32_t crc = values[slice - 1][i];
                values[slice][i] = (crc << 8) ^ values[0][crc >> 24];
            }
        return values;
    }();
    return table;
}

std::uint32_t blockCrc(const std::uint8_t* bytes, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    const auto& table = crcTable();
    while (size >= 8) {
        const std::uint32_t first = (std::uint32_t(bytes[0]) << 24) |
            (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) |
            std::uint32_t(bytes[3]);
        crc ^= first;
        crc = table[7][crc >> 24] ^ table[6][(crc >> 16) & 0xffU] ^
            table[5][(crc >> 8) & 0xffU] ^ table[4][crc & 0xffU] ^
            table[3][bytes[4]] ^ table[2][bytes[5]] ^
            table[1][bytes[6]] ^ table[0][bytes[7]];
        bytes += 8;
        size -= 8;
    }
    for (std::size_t i = 0; i < size; ++i)
        crc = (crc << 8) ^ table[0][((crc >> 24) ^ bytes[i]) & 0xffU];
    return crc ^ 0xffffffffU;
}

bool appendRepeated(std::vector<std::uint8_t>& output, std::uint8_t value,
                    std::size_t count, std::size_t limit) {
    if (count > limit - output.size() || count > output.max_size() - output.size())
        return false;
    output.insert(output.end(), count, value);
    return true;
}

struct BzipBlockSymbols {
    std::vector<std::uint8_t> lastColumn;
    std::uint32_t originalPointer;
    bool randomized;

    BzipBlockSymbols() : originalPointer(0), randomized(false) {}
};

struct BzipBlockResult {
    bool success;
    std::uint32_t crc;
};

bool decodeBlockSymbols(BitReader& bits, std::uint32_t originalPointer,
                        bool randomized, std::size_t blockLimit,
                        BzipBlockSymbols* decoded) {
    if (!decoded) return false;
    std::uint32_t groups = 0;
    std::uint32_t selectorCount = 0;
    std::uint32_t mapHigh = 0;
    if (!bits.read(16, &mapHigh)) return false;
    std::array<bool, 256> used{};
    unsigned usedCount = 0;
    for (unsigned group = 0; group < 16; ++group) {
        if (!(mapHigh & (UINT32_C(1) << (15 - group)))) continue;
        std::uint32_t mapLow = 0;
        if (!bits.read(16, &mapLow)) return false;
        for (unsigned bit = 0; bit < 16; ++bit) {
            if (mapLow & (UINT32_C(1) << (15 - bit))) {
                used[group * 16 + bit] = true;
                ++usedCount;
            }
        }
    }
    if (usedCount == 0 || !bits.read(3, &groups) || groups < 2 || groups > 6 ||
        !bits.read(15, &selectorCount) || selectorCount == 0 ||
        selectorCount > kMaxSelectors)
        return false;

    std::vector<std::uint8_t> selectors;
    selectors.reserve(selectorCount);
    std::array<std::uint8_t, 6> selectorMtf{};
    for (unsigned i = 0; i < groups; ++i) selectorMtf[i] = static_cast<std::uint8_t>(i);
    for (std::uint32_t i = 0; i < selectorCount; ++i) {
        unsigned index = 0;
        std::uint32_t bit = 0;
        do {
            if (!bits.read(1, &bit)) return false;
            if (bit && ++index >= groups) return false;
        } while (bit);
        const std::uint8_t selected = selectorMtf[index];
        for (unsigned j = index; j > 0; --j) selectorMtf[j] = selectorMtf[j - 1];
        selectorMtf[0] = selected;
        selectors.push_back(selected);
    }

    const unsigned alphabetSize = usedCount + 2;
    std::array<HuffmanTree, 6> trees;
    for (unsigned group = 0; group < groups; ++group) {
        std::uint32_t currentLength = 0;
        if (!bits.read(5, &currentLength) || currentLength == 0 ||
            currentLength > kMaxCodeLength)
            return false;
        std::vector<std::uint8_t> lengths(alphabetSize);
        for (unsigned symbol = 0; symbol < alphabetSize; ++symbol) {
            std::uint32_t change = 0;
            while (true) {
                if (!bits.read(1, &change)) return false;
                if (!change) break;
                std::uint32_t direction = 0;
                if (!bits.read(1, &direction)) return false;
                if (direction) {
                    if (currentLength <= 1) return false;
                    --currentLength;
                } else {
                    if (currentLength >= kMaxCodeLength) return false;
                    ++currentLength;
                }
            }
            lengths[symbol] = static_cast<std::uint8_t>(currentLength);
        }
        if (!trees[group].build(lengths)) return false;
    }

    std::array<std::uint8_t, 256> symbolStack{};
    unsigned stackSize = 0;
    for (unsigned symbol = 0; symbol < used.size(); ++symbol)
        if (used[symbol]) symbolStack[stackSize++] = static_cast<std::uint8_t>(symbol);

    std::vector<std::uint8_t> lastColumn;
    lastColumn.reserve(blockLimit);
    std::size_t selectorIndex = 0;
    unsigned groupRemaining = 0;
    unsigned activeGroup = 0;
    auto nextSymbol = [&](std::uint32_t* symbol) -> bool {
        if (groupRemaining == 0) {
            if (selectorIndex >= selectors.size()) return false;
            activeGroup = selectors[selectorIndex++];
            groupRemaining = 50;
        }
        --groupRemaining;
        return trees[activeGroup].decode(bits, symbol);
    };

    const std::uint32_t endOfBlock = usedCount + 1;
    std::uint32_t symbol = 0;
    while (true) {
        if (!nextSymbol(&symbol)) return false;
        if (symbol == endOfBlock) break;
        if (symbol <= 1) {
            std::uint64_t run = UINT64_MAX;
            std::uint64_t weight = 1;
            do {
                const std::uint64_t add = symbol == 0 ? weight : weight * 2;
                if (run == UINT64_MAX) run = add - 1;
                else if (add > blockLimit || run > blockLimit - add) return false;
                else run += add;
                if (weight > blockLimit / 2) return false;
                weight <<= 1;
                if (!nextSymbol(&symbol)) return false;
            } while (symbol <= 1);
            if (++run > blockLimit - lastColumn.size() || stackSize == 0 ||
                !appendRepeated(lastColumn, symbolStack[0], std::size_t(run), blockLimit))
                return false;
            if (symbol == endOfBlock) break;
        }
        if (symbol <= 1 || symbol >= endOfBlock) return false;
        const unsigned mtfIndex = symbol - 1;
        if (mtfIndex >= stackSize || lastColumn.size() >= blockLimit) return false;
        const std::uint8_t value = symbolStack[mtfIndex];
        if (mtfIndex)
            std::memmove(symbolStack.data() + 1, symbolStack.data(), mtfIndex);
        symbolStack[0] = value;
        lastColumn.push_back(value);
    }

    if (lastColumn.empty() || originalPointer >= lastColumn.size()) return false;
    decoded->lastColumn.swap(lastColumn);
    decoded->originalPointer = originalPointer;
    decoded->randomized = randomized;
    return true;
}

bool finishBlock(const BzipBlockSymbols& decoded, std::uint32_t expectedCrc,
                 std::size_t outputLimit, bool checkCrc,
                 std::vector<std::uint8_t>* output, std::uint32_t* decodedCrc) {
    if (!output || !decodedCrc || output->size() > outputLimit) return false;
    const std::vector<std::uint8_t>& lastColumn = decoded.lastColumn;
    const std::uint32_t originalPointer = decoded.originalPointer;
    const bool randomized = decoded.randomized;
    if (lastColumn.empty() || originalPointer >= lastColumn.size()) return false;
    std::array<std::uint32_t, 257> cumulative{};
    for (std::uint8_t value : lastColumn) ++cumulative[std::size_t(value) + 1];
    for (unsigned i = 1; i < cumulative.size(); ++i) cumulative[i] += cumulative[i - 1];
    std::vector<std::uint32_t> links(lastColumn.size());
    std::array<std::uint32_t, 256> positions{};
    std::copy(cumulative.begin(), cumulative.end() - 1, positions.begin());
    for (std::uint32_t i = 0; i < lastColumn.size(); ++i) {
        const std::uint32_t target = positions[lastColumn[i]]++;
        links[target] = (i << 8) | lastColumn[target];
    }

    std::uint32_t position = links[originalPointer] >> 8;
    unsigned randomIndex = 0;
    std::uint32_t randomRemaining = 0;
    std::size_t transformedCount = 0;
    auto nextTransformed = [&](std::uint8_t* value) -> bool {
        if (transformedCount == lastColumn.size() || position >= lastColumn.size())
            return false;
        const std::uint32_t link = links[position];
        *value = static_cast<std::uint8_t>(link & 0xFFU);
        if (randomized) {
            if (randomRemaining == 0) {
                randomRemaining = kLegacyRandomization[randomIndex++];
                if (randomIndex == 512) randomIndex = 0;
            }
            if (randomRemaining == 1) *value ^= 1;
            --randomRemaining;
        }
        position = link >> 8;
        ++transformedCount;
        return true;
    };

    const std::size_t blockStart = output->size();
    bool hasPending = false;
    std::uint8_t pendingValue = 0;
    while (hasPending || transformedCount < lastColumn.size()) {
        std::uint8_t value = 0;
        if (hasPending) {
            value = pendingValue;
            hasPending = false;
        } else if (!nextTransformed(&value)) {
            return false;
        }
        unsigned runLength = 1;
        while (runLength < 4 && transformedCount < lastColumn.size()) {
            std::uint8_t next = 0;
            if (!nextTransformed(&next)) return false;
            if (next == value) {
                ++runLength;
            } else {
                pendingValue = next;
                hasPending = true;
                break;
            }
        }
        std::size_t repeatCount = runLength;
        if (runLength == 4) {
            std::uint8_t extra = 0;
            if (!nextTransformed(&extra)) return false;
            repeatCount += extra;
        }
        const std::size_t blockLimitRemaining = outputLimit - output->size();
        if (repeatCount > blockLimitRemaining) return false;
        output->insert(output->end(), repeatCount, value);
    }
    const std::size_t blockSize = output->size() - blockStart;
    const std::uint32_t crc = blockCrc(output->data() + blockStart, blockSize);
    if (checkCrc && crc != expectedCrc)
        return false;
    if (decodedCrc) *decodedCrc = crc;
    return true;
}

bool decodeNsisStream(BitReader& bits, std::size_t outputLimit,
                      std::vector<std::uint8_t>* output, bool hasRandomizedFlag) {
    const std::size_t blockLimit = 900000;
    while (true) {
        std::uint32_t marker = 0;
        if (!bits.read(8, &marker)) return false;
        if (marker == 0x17) {
            bits.alignByte();
            return true;
        }
        if (marker != 0x31) return false;
        std::uint32_t randomized = 0;
        if (hasRandomizedFlag && !bits.read(1, &randomized)) return false;
        std::uint32_t originalPointer = 0;
        if (!bits.read(24, &originalPointer)) return false;
        std::shared_ptr<BzipBlockSymbols> decoded = std::make_shared<BzipBlockSymbols>();
        if (!decodeBlockSymbols(bits, originalPointer, randomized != 0, blockLimit, decoded.get()))
            return false;
        std::uint32_t crc = 0;
        if (!finishBlock(*decoded, 0, outputLimit, false, output, &crc)) return false;
    }
}

bool decodeStream(BitReader& bits, std::size_t outputLimit, std::vector<std::uint8_t>* output) {
    std::uint32_t byte = 0;
    if (!bits.read(8, &byte) || byte != 'B' || !bits.read(8, &byte) || byte != 'Z' ||
        !bits.read(8, &byte) || byte != 'h' || !bits.read(8, &byte) ||
        byte < '1' || byte > '9')
        return false;
    const std::size_t blockLimit = std::size_t(byte - '0') * 100000;
    std::uint32_t combinedCrc = 0;
    std::future<BzipBlockResult> pending;
    bool hasPending = false;
    auto includeBlockCrc = [&](std::uint32_t crc) {
        combinedCrc = (combinedCrc << 1) | (combinedCrc >> 31);
        combinedCrc ^= crc;
    };
    auto finishPending = [&]() -> bool {
        if (!hasPending) return true;
        const BzipBlockResult result = pending.get();
        hasPending = false;
        if (!result.success) return false;
        includeBlockCrc(result.crc);
        return true;
    };
    while (true) {
        std::uint64_t marker = 0;
        std::uint32_t storedCrc = 0;
        if (!bits.read64(48, &marker) || !bits.read(32, &storedCrc)) return false;
        if (marker == kEndMagic) {
            if (!finishPending()) return false;
            if (storedCrc != combinedCrc) return false;
            bits.alignByte();
            return true;
        }
        if (marker != kBlockMagic) return false;
        std::uint32_t randomized = 0;
        std::uint32_t originalPointer = 0;
        if (!bits.read(1, &randomized) || !bits.read(24, &originalPointer)) return false;
        std::shared_ptr<BzipBlockSymbols> decoded =
            std::make_shared<BzipBlockSymbols>();
        if (!decodeBlockSymbols(bits, originalPointer, randomized != 0,
                                blockLimit, decoded.get()) ||
            !finishPending())
            return false;
        try {
            pending = std::async(std::launch::async,
                [decoded, storedCrc, outputLimit, output]() {
                    std::uint32_t crc = 0;
                    const bool success = finishBlock(
                        *decoded, storedCrc, outputLimit, true, output, &crc);
                    return BzipBlockResult{success, crc};
                });
            hasPending = true;
        } catch (...) {
            std::uint32_t crc = 0;
            if (!finishBlock(*decoded, storedCrc, outputLimit, true, output, &crc))
                return false;
            includeBlockCrc(crc);
        }
    }
}

}  // namespace

bool decompressBzip2(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputLimit, bool allowConcatenated,
                     std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (!input || inputSize < 4) return false;
    try {
        BitReader bits(input, inputSize);
        if (!decodeStream(bits, outputLimit, output)) {
            output->clear();
            return false;
        }
        while (bits.byteOffset() < inputSize) {
            if (!allowConcatenated ||
                !decodeStream(bits, outputLimit, output)) {
                output->clear();
                return false;
            }
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

bool decompressNsisBzip2(const std::uint8_t* input, std::size_t inputSize,
                         std::size_t outputLimit,
                         std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (!input || inputSize < 2) return false;
    try {
        BitReader bits(input, inputSize);
        if (!decodeNsisStream(bits, outputLimit, output, false)) {
            output->clear();
            return false;
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

bool decompressNsisV1Bzip2(const std::uint8_t* input, std::size_t inputSize,
                           std::size_t outputLimit,
                           std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (!input || inputSize < 2) return false;
    try {
        BitReader bits(input, inputSize);
        if (!decodeNsisStream(bits, outputLimit, output, true)) {
            output->clear();
            return false;
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

}  // namespace compression
}  // namespace peare
