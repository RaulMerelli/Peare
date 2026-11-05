#include "LhaDecoder.h"
#include "LhaLh23Decoder.h"
#include "LhaLk7Decoder.h"
#include "LhaPm1Decoder.h"
#include "LhaPm2Decoder.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace peare {
namespace compression {
namespace {

const unsigned kPositionTreeSymbols = 19;
const unsigned kLiteralTreeSymbols = 510;
const unsigned kMaxMatch = 256;
const unsigned kLh1WindowSize = 4096;
const unsigned kLh1WindowMask = kLh1WindowSize - 1;
const unsigned kLh1MaxMatch = 60;
const unsigned kLh1CharacterCount = 314;
const unsigned kLh1TreeSize = kLh1CharacterCount * 2 - 1;
const unsigned kLh1Root = kLh1TreeSize - 1;
const unsigned kLh1MaxFrequency = 0x8000;

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

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_;
    std::uint64_t buffer_;
    unsigned available_;
};

class Lh1AdaptiveTree {
public:
    Lh1AdaptiveTree()
        : frequency_(kLh1TreeSize + 1, 0), child_(kLh1TreeSize, 0),
          parent_(kLh1TreeSize + kLh1CharacterCount, 0) {
        for (unsigned symbol = 0; symbol < kLh1CharacterCount; ++symbol) {
            frequency_[symbol] = 1;
            child_[symbol] = kLh1TreeSize + symbol;
            parent_[kLh1TreeSize + symbol] = symbol;
        }
        unsigned firstChild = 0;
        for (unsigned node = kLh1CharacterCount; node <= kLh1Root; ++node) {
            frequency_[node] = frequency_[firstChild] + frequency_[firstChild + 1];
            child_[node] = firstChild;
            parent_[firstChild] = node;
            parent_[firstChild + 1] = node;
            firstChild += 2;
        }
        frequency_[kLh1TreeSize] = 0xffff;
    }

    bool decode(BitReader& bits, unsigned* symbol) {
        if (!symbol) return false;
        unsigned node = child_[kLh1Root];
        while (node < kLh1TreeSize) {
            std::uint32_t branch = 0;
            if (!bits.read(1, &branch)) return false;
            node = child_[node + branch];
        }
        *symbol = node - kLh1TreeSize;
        update(*symbol);
        return true;
    }

private:
    std::vector<unsigned> frequency_;
    std::vector<unsigned> child_;
    std::vector<unsigned> parent_;

    void setChildParents(unsigned node) {
        const unsigned firstChild = child_[node];
        parent_[firstChild] = node;
        if (firstChild < kLh1TreeSize) parent_[firstChild + 1] = node;
    }

    void rebuild() {
        unsigned leafCount = 0;
        for (unsigned node = 0; node < kLh1TreeSize; ++node) {
            if (child_[node] >= kLh1TreeSize) {
                frequency_[leafCount] = (frequency_[node] + 1) / 2;
                child_[leafCount] = child_[node];
                ++leafCount;
            }
        }

        unsigned pair = 0;
        for (unsigned nodeCount = kLh1CharacterCount; nodeCount < kLh1TreeSize;
             ++nodeCount, pair += 2) {
            const unsigned weight = frequency_[pair] + frequency_[pair + 1];
            unsigned insertion = nodeCount;
            while (insertion > 0 && weight < frequency_[insertion - 1]) {
                frequency_[insertion] = frequency_[insertion - 1];
                child_[insertion] = child_[insertion - 1];
                --insertion;
            }
            frequency_[insertion] = weight;
            child_[insertion] = pair;
        }
        for (unsigned node = 0; node < kLh1TreeSize; ++node) setChildParents(node);
        parent_[kLh1Root] = 0;
        frequency_[kLh1TreeSize] = 0xffff;
    }

    void update(unsigned symbol) {
        if (frequency_[kLh1Root] >= kLh1MaxFrequency) rebuild();
        unsigned node = parent_[kLh1TreeSize + symbol];
        do {
            const unsigned updatedFrequency = ++frequency_[node];
            unsigned leader = node + 1;
            if (updatedFrequency > frequency_[leader]) {
                while (updatedFrequency > frequency_[++leader]) {
                }
                --leader;
                frequency_[node] = frequency_[leader];
                frequency_[leader] = updatedFrequency;
                const unsigned firstChild = child_[node];
                parent_[firstChild] = leader;
                if (firstChild < kLh1TreeSize) parent_[firstChild + 1] = leader;
                const unsigned otherChild = child_[leader];
                child_[leader] = firstChild;
                parent_[otherChild] = node;
                if (otherChild < kLh1TreeSize) parent_[otherChild + 1] = node;
                child_[node] = otherChild;
                node = leader;
            }
            node = parent_[node];
        } while (node != 0);
    }
};

struct HuffmanTree {
    std::array<std::uint32_t, 17> firstCode{};
    std::array<std::uint32_t, 17> firstIndex{};
    std::array<std::uint32_t, 17> counts{};
    std::vector<std::uint16_t> symbols;
    unsigned maximum = 0;
    bool constant = false;
    std::uint32_t constantValue = 0;

    void setConstant(std::uint32_t value) {
        constant = true;
        constantValue = value;
        maximum = 0;
    }

    bool build(const std::vector<std::uint8_t>& lengths) {
        counts.fill(0);
        maximum = 0;
        constant = false;
        for (std::uint8_t length : lengths) {
            if (length > 16) return false;
            if (length != 0) {
                ++counts[length];
                maximum = std::max(maximum, unsigned(length));
            }
        }
        if (maximum == 0) return false;
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
        return !symbols.empty();
    }

    bool decode(BitReader& bits, std::uint32_t* symbol) const {
        if (!symbol) return false;
        if (constant) {
            *symbol = constantValue;
            return true;
        }
        std::uint32_t code = 0;
        for (unsigned length = 1; length <= maximum; ++length) {
            std::uint32_t bit = 0;
            if (!bits.read(1, &bit)) return false;
            code = (code << 1) | bit;
            if (code < firstCode[length]) continue;
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

bool readPositionTree(BitReader& bits, unsigned symbolCount, unsigned bitCount,
                      int specialIndex, HuffmanTree* tree) {
    std::uint32_t count = 0;
    if (!tree || !bits.read(bitCount, &count)) return false;
    if (count == 0) {
        std::uint32_t symbol = 0;
        if (!bits.read(bitCount, &symbol) || symbol >= symbolCount) return false;
        tree->setConstant(symbol);
        return true;
    }
    if (count > symbolCount) return false;
    std::vector<std::uint8_t> lengths(symbolCount, 0);
    unsigned index = 0;
    while (index < count) {
        std::uint32_t length = 0;
        if (!bits.read(3, &length)) return false;
        if (length == 7) {
            std::uint32_t bit = 0;
            do {
                if (!bits.read(1, &bit)) return false;
                if (bit) ++length;
            } while (bit);
        }
        if (length > 16) return false;
        lengths[index++] = static_cast<std::uint8_t>(length);
        if (specialIndex >= 0 && index == static_cast<unsigned>(specialIndex)) {
            std::uint32_t zeros = 0;
            if (!bits.read(2, &zeros) || zeros > symbolCount - index) return false;
            index += zeros;
        }
    }
    return tree->build(lengths);
}

bool readLiteralTree(BitReader& bits, const HuffmanTree& positionTree,
                     HuffmanTree* tree) {
    std::uint32_t count = 0;
    if (!tree || !bits.read(9, &count)) return false;
    std::vector<std::uint8_t> lengths(kLiteralTreeSymbols, 0);
    if (count == 0) {
        std::uint32_t length = 0;
        if (!bits.read(9, &length) || length == 0 || length > 16) return false;
        std::fill(lengths.begin(), lengths.end(), static_cast<std::uint8_t>(length));
        return tree->build(lengths);
    }
    if (count > lengths.size()) return false;

    unsigned index = 0;
    while (index < count) {
        std::uint32_t symbol = 0;
        if (!positionTree.decode(bits, &symbol)) return false;
        if (symbol <= 2) {
            std::uint32_t run = 1;
            if (symbol == 1) {
                std::uint32_t extra = 0;
                if (!bits.read(4, &extra)) return false;
                run = extra + 3;
            } else if (symbol == 2) {
                std::uint32_t extra = 0;
                if (!bits.read(9, &extra)) return false;
                run = extra + 20;
            }
            if (run > count - index) return false;
            index += run;
            continue;
        }
        const std::uint32_t length = symbol - 2;
        if (length > 16) return false;
        lengths[index++] = static_cast<std::uint8_t>(length);
    }
    return tree->build(lengths);
}

bool readBlockTrees(BitReader& bits, unsigned positionSymbols, unsigned positionBitCount,
                    HuffmanTree* literalTree, HuffmanTree* positionTree,
                    std::uint32_t* blockSymbols) {
    if (!literalTree || !positionTree || !blockSymbols ||
        !bits.read(16, blockSymbols) || *blockSymbols == 0)
        return false;
    HuffmanTree codeLengthTree;
    return readPositionTree(bits, kPositionTreeSymbols, 5, 3, &codeLengthTree) &&
           readLiteralTree(bits, codeLengthTree, literalTree) &&
           readPositionTree(bits, positionSymbols, positionBitCount, -1, positionTree);
}

}  // namespace

bool decompressLz5(const std::uint8_t* input, std::size_t inputSize,
                   std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (outputSize == 0) return true;
    if (!input || inputSize == 0) return false;
    try {
        std::array<std::uint8_t, 4096> window{};
        for (unsigned i = 0; i < 0xD00; ++i)
            window[i] = static_cast<std::uint8_t>(i / 0x0D);
        for (unsigned i = 0; i < 0x100; ++i) {
            window[0xD00 + i] = static_cast<std::uint8_t>(i);
            window[0xE00 + i] = static_cast<std::uint8_t>(0xFF - i);
        }
        std::fill(window.begin() + 0xF00, window.begin() + 0xF80, 0);
        std::fill(window.begin() + 0xF80, window.begin() + 0xFEE,
                  static_cast<std::uint8_t>(' '));
        std::fill(window.begin() + 0xFEE, window.end(), 0);

        std::size_t inputOffset = 0;
        unsigned writePosition = 0xFEE;
        output->reserve(outputSize);
        while (output->size() < outputSize) {
            if (inputOffset >= inputSize) {
                output->clear();
                return false;
            }
            const std::uint8_t flags = input[inputOffset++];
            for (unsigned bit = 0; bit < 8 && output->size() < outputSize; ++bit) {
                if ((flags & (1U << bit)) != 0) {
                    if (inputOffset >= inputSize) {
                        output->clear();
                        return false;
                    }
                    const std::uint8_t value = input[inputOffset++];
                    output->push_back(value);
                    window[writePosition] = value;
                    writePosition = (writePosition + 1) & 0xFFF;
                    continue;
                }
                if (inputSize - inputOffset < 2) {
                    output->clear();
                    return false;
                }
                const unsigned sourcePosition = input[inputOffset] |
                    (unsigned(input[inputOffset + 1] & 0xF0) << 4);
                const unsigned length = (input[inputOffset + 1] & 0x0F) + 3;
                inputOffset += 2;
                if (length > outputSize - output->size()) {
                    output->clear();
                    return false;
                }
                unsigned source = sourcePosition;
                for (unsigned i = 0; i < length; ++i) {
                    const std::uint8_t value = window[source];
                    source = (source + 1) & 0xFFF;
                    output->push_back(value);
                    window[writePosition] = value;
                    writePosition = (writePosition + 1) & 0xFFF;
                }
            }
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

bool decompressLzs(const std::uint8_t* input, std::size_t inputSize,
                   std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (outputSize == 0) return true;
    if (!input || inputSize == 0) return false;
    try {
        const unsigned windowSize = 1U << 11;
        const unsigned windowMask = windowSize - 1;
        std::array<std::uint8_t, windowSize> window{};
        window.fill(static_cast<std::uint8_t>(' '));
        unsigned writePosition = windowSize - 17;
        BitReader bits(input, inputSize);
        output->reserve(outputSize);
        while (output->size() < outputSize) {
            std::uint32_t literal = 0;
            if (!bits.read(1, &literal)) {
                output->clear();
                return false;
            }
            if (literal) {
                std::uint32_t value = 0;
                if (!bits.read(8, &value)) {
                    output->clear();
                    return false;
                }
                const std::uint8_t byte = static_cast<std::uint8_t>(value);
                output->push_back(byte);
                window[writePosition] = byte;
                writePosition = (writePosition + 1) & windowMask;
                continue;
            }
            std::uint32_t source = 0;
            std::uint32_t lengthCode = 0;
            if (!bits.read(11, &source) || !bits.read(4, &lengthCode)) {
                output->clear();
                return false;
            }
            const std::size_t length = std::size_t(lengthCode) + 2;
            if (length > outputSize - output->size()) {
                output->clear();
                return false;
            }
            for (std::size_t i = 0; i < length; ++i) {
                const std::uint8_t value = window[source];
                source = (source + 1) & windowMask;
                output->push_back(value);
                window[writePosition] = value;
                writePosition = (writePosition + 1) & windowMask;
            }
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

bool decompressLh(const char* method, const std::uint8_t* input, std::size_t inputSize,
                  std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    if (method && std::strcmp(method, "-lk7-") == 0)
        return decompressLhaLk7(method, input, inputSize, outputSize, output);
    if (method && (std::strcmp(method, "-lh2-") == 0 ||
                   std::strcmp(method, "-lh3-") == 0))
        return decompressLhaLh23(method, input, inputSize, outputSize, output);
    if (method && std::strcmp(method, "-pm1-") == 0)
        return decompressLhaPm1(input, inputSize, outputSize, output);
    if (method && std::strcmp(method, "-pm2-") == 0)
        return decompressLhaPm2(input, inputSize, outputSize, output);
    if (method && std::strcmp(method, "-lzs-") == 0)
        return decompressLzs(input, inputSize, outputSize, output);
    if (method && std::strcmp(method, "-lz5-") == 0)
        return decompressLz5(input, inputSize, outputSize, output);
    if (method && std::strcmp(method, "-lh1-") == 0)
        return decompressLh1(input, inputSize, outputSize, output);
    output->clear();
    unsigned windowBits = 0;
    if (method && std::strcmp(method, "-lh4-") == 0) windowBits = 12;
    else if (method && std::strcmp(method, "-lh5-") == 0) windowBits = 13;
    else if (method && std::strcmp(method, "-lh6-") == 0) windowBits = 15;
    else if (method && std::strcmp(method, "-lh7-") == 0) windowBits = 16;
    else if (method && std::strcmp(method, "-lhx-") == 0) windowBits = 20;
    else return false;
    if (outputSize == 0) return true;
    if (!input || inputSize == 0) return false;
    try {
        const unsigned windowSize = 1U << windowBits;
        const unsigned windowMask = windowSize - 1;
        const unsigned positionSymbols = windowBits + 1;
        const unsigned positionBitCount = windowBits <= 13 ? 4 : 5;
        BitReader bits(input, inputSize);
        std::vector<std::uint8_t> window(windowSize, static_cast<std::uint8_t>(' '));
        unsigned windowPosition = windowSize - kMaxMatch;
        output->reserve(outputSize);
        HuffmanTree literalTree;
        HuffmanTree positionTree;
        std::uint32_t blockSymbols = 0;

        while (output->size() < outputSize) {
            if (blockSymbols == 0 &&
                !readBlockTrees(bits, positionSymbols, positionBitCount,
                                &literalTree, &positionTree, &blockSymbols)) {
                output->clear();
                return false;
            }
            --blockSymbols;
            std::uint32_t symbol = 0;
            if (!literalTree.decode(bits, &symbol)) {
                output->clear();
                return false;
            }
            if (symbol < 256) {
                const std::uint8_t value = static_cast<std::uint8_t>(symbol);
                output->push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) & windowMask;
                continue;
            }

            const std::uint32_t matchLength = symbol - 253;
            std::uint32_t positionCode = 0;
            if (matchLength < 3 || matchLength > kMaxMatch ||
                !positionTree.decode(bits, &positionCode) ||
                positionCode >= positionSymbols) {
                output->clear();
                return false;
            }
            std::uint32_t distance = 0;
            if (positionCode != 0) {
                std::uint32_t extra = 0;
                if (!bits.read(positionCode - 1, &extra)) {
                    output->clear();
                    return false;
                }
                distance = (UINT32_C(1) << (positionCode - 1)) | extra;
            }
            if (matchLength > outputSize - output->size()) {
                output->clear();
                return false;
            }
            unsigned source = (windowPosition - distance - 1) & windowMask;
            for (std::uint32_t i = 0; i < matchLength; ++i) {
                const std::uint8_t value = window[source];
                source = (source + 1) & windowMask;
                output->push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) & windowMask;
            }
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

bool decompressLh1(const std::uint8_t* input, std::size_t inputSize,
                   std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (outputSize == 0) return true;
    if (!input || inputSize == 0) return false;
    try {
        BitReader bits(input, inputSize);
        Lh1AdaptiveTree adaptiveTree;
        std::vector<std::uint8_t> positionLengths(64, 0);
        positionLengths[0] = 3;
        for (unsigned i = 1; i <= 3; ++i) positionLengths[i] = 4;
        for (unsigned i = 4; i <= 11; ++i) positionLengths[i] = 5;
        for (unsigned i = 12; i <= 23; ++i) positionLengths[i] = 6;
        for (unsigned i = 24; i <= 47; ++i) positionLengths[i] = 7;
        for (unsigned i = 48; i < positionLengths.size(); ++i) positionLengths[i] = 8;
        HuffmanTree positionTree;
        if (!positionTree.build(positionLengths)) return false;

        std::vector<std::uint8_t> window(kLh1WindowSize, static_cast<std::uint8_t>(' '));
        unsigned windowPosition = kLh1WindowSize - kLh1MaxMatch;
        output->reserve(outputSize);
        while (output->size() < outputSize) {
            unsigned symbol = 0;
            if (!adaptiveTree.decode(bits, &symbol)) {
                output->clear();
                return false;
            }
            if (symbol < 256) {
                const std::uint8_t value = static_cast<std::uint8_t>(symbol);
                output->push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) & kLh1WindowMask;
                continue;
            }

            const unsigned matchLength = symbol - 253;
            std::uint32_t positionClass = 0;
            std::uint32_t lowPosition = 0;
            if (matchLength < 3 || matchLength > kLh1MaxMatch ||
                !positionTree.decode(bits, &positionClass) || positionClass >= 64 ||
                !bits.read(6, &lowPosition)) {
                output->clear();
                return false;
            }
            const unsigned distance = (positionClass << 6) | lowPosition;
            unsigned source = (windowPosition - distance - 1) & kLh1WindowMask;
            const unsigned copyLength = static_cast<unsigned>(std::min<std::size_t>(
                matchLength, outputSize - output->size()));
            for (unsigned i = 0; i < copyLength; ++i) {
                const std::uint8_t value = window[source];
                source = (source + 1) & kLh1WindowMask;
                output->push_back(value);
                window[windowPosition] = value;
                windowPosition = (windowPosition + 1) & kLh1WindowMask;
            }
        }
        return true;
    } catch (...) {
        output->clear();
        return false;
    }
}

}  // namespace compression
}  // namespace peare
