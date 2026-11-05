#include "LhaLk7Decoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {

const unsigned kLiteralLengthSymbols = 289;
const unsigned kPositionSymbols = 32;
const unsigned kCodeLengthSymbols = 19;
const unsigned kHistoryBits = 16;
const unsigned kHistorySize = 1U << kHistoryBits;
const unsigned kHistoryMask = kHistorySize - 1;
const unsigned kMaxCopyLength = 514;

class BitReader {
public:
    BitReader(const uint8_t* input, size_t inputSize)
        : input_(input), inputSize_(inputSize), inputOffset_(0), buffer_(0), available_(0) {}

    bool read(unsigned bitCount, uint32_t* value) {
        if (value == nullptr || bitCount > 32)
            return false;
        while (available_ < bitCount) {
            if (inputOffset_ >= inputSize_)
                return false;
            buffer_ = (buffer_ << 8) | input_[inputOffset_++];
            available_ += 8;
        }
        const unsigned shift = available_ - bitCount;
        const uint64_t mask = bitCount == 32 ? UINT64_C(0xffffffff)
                                              : ((UINT64_C(1) << bitCount) - 1);
        *value = static_cast<uint32_t>((buffer_ >> shift) & mask);
        available_ -= bitCount;
        if (available_ == 0)
            buffer_ = 0;
        else
            buffer_ &= (UINT64_C(1) << available_) - 1;
        return true;
    }

private:
    const uint8_t* input_;
    size_t inputSize_;
    size_t inputOffset_;
    uint64_t buffer_;
    unsigned available_;
};

struct HuffmanTree {
    std::array<uint32_t, 17> firstCode;
    std::array<uint32_t, 17> firstSymbol;
    std::array<uint32_t, 17> counts;
    std::vector<uint16_t> symbols;
    unsigned maximumLength;
    bool constant;
    uint32_t constantSymbol;

    HuffmanTree() : maximumLength(0), constant(false), constantSymbol(0) {
        firstCode.fill(0);
        firstSymbol.fill(0);
        counts.fill(0);
    }

    void setConstant(uint32_t symbol) {
        constant = true;
        constantSymbol = symbol;
        maximumLength = 0;
        symbols.clear();
    }

    bool build(const std::vector<uint8_t>& codeLengths) {
        counts.fill(0);
        maximumLength = 0;
        constant = false;
        symbols.clear();
        for (size_t i = 0; i < codeLengths.size(); ++i) {
            const unsigned length = codeLengths[i];
            if (length > 16)
                return false;
            if (length != 0) {
                ++counts[length];
                maximumLength = std::max(maximumLength, length);
            }
        }
        if (maximumLength == 0)
            return false;

        uint32_t code = 0;
        uint32_t index = 0;
        for (unsigned length = 1; length <= maximumLength; ++length) {
            firstCode[length] = code;
            firstSymbol[length] = index;
            code += counts[length];
            if (code > (UINT32_C(1) << length))
                return false;
            index += counts[length];
            code <<= 1;
        }
        symbols.reserve(index);
        for (unsigned length = 1; length <= maximumLength; ++length) {
            for (size_t symbol = 0; symbol < codeLengths.size(); ++symbol) {
                if (codeLengths[symbol] == length)
                    symbols.push_back(static_cast<uint16_t>(symbol));
            }
        }
        return !symbols.empty();
    }

    bool decode(BitReader* bits, uint32_t* symbol) const {
        if (bits == nullptr || symbol == nullptr)
            return false;
        if (constant) {
            *symbol = constantSymbol;
            return true;
        }
        uint32_t code = 0;
        for (unsigned length = 1; length <= maximumLength; ++length) {
            uint32_t bit = 0;
            if (!bits->read(1, &bit))
                return false;
            code = (code << 1) | bit;
            if (code < firstCode[length])
                continue;
            const uint32_t delta = code - firstCode[length];
            if (delta < counts[length]) {
                const uint32_t position = firstSymbol[length] + delta;
                if (position >= symbols.size())
                    return false;
                *symbol = symbols[position];
                return true;
            }
        }
        return false;
    }
};

// Hypothesis: LK7 retains the common LH5 block and tree-description grammar.
bool readCodeLengthTree(BitReader* bits, HuffmanTree* tree) {
    uint32_t count = 0;
    if (bits == nullptr || tree == nullptr || !bits->read(5, &count))
        return false;
    if (count == 0) {
        uint32_t symbol = 0;
        if (!bits->read(5, &symbol) || symbol >= kCodeLengthSymbols)
            return false;
        tree->setConstant(symbol);
        return true;
    }
    if (count > kCodeLengthSymbols)
        return false;

    std::vector<uint8_t> lengths(kCodeLengthSymbols, 0);
    unsigned index = 0;
    while (index < count) {
        uint32_t length = 0;
        if (!bits->read(3, &length))
            return false;
        if (length == 7) {
            uint32_t continuation = 0;
            do {
                if (!bits->read(1, &continuation))
                    return false;
                if (continuation != 0)
                    ++length;
            } while (continuation != 0);
        }
        if (length > 16)
            return false;
        lengths[index++] = static_cast<uint8_t>(length);
        if (index == 3) {
            uint32_t zeroCount = 0;
            if (!bits->read(2, &zeroCount) || zeroCount > kCodeLengthSymbols - index)
                return false;
            index += zeroCount;
        }
    }
    return tree->build(lengths);
}

bool readLiteralLengthTree(BitReader* bits, const HuffmanTree& codeLengthTree,
                           HuffmanTree* tree) {
    uint32_t count = 0;
    if (bits == nullptr || tree == nullptr || !bits->read(9, &count))
        return false;
    std::vector<uint8_t> lengths(kLiteralLengthSymbols, 0);
    if (count == 0) {
        uint32_t uniformLength = 0;
        if (!bits->read(9, &uniformLength) || uniformLength > 16)
            return false;
        std::fill(lengths.begin(), lengths.end(), static_cast<uint8_t>(uniformLength));
        return tree->build(lengths);
    }
    if (count > kLiteralLengthSymbols)
        return false;

    unsigned index = 0;
    while (index < count) {
        uint32_t code = 0;
        if (!codeLengthTree.decode(bits, &code))
            return false;
        if (code <= 2) {
            uint32_t run = 1;
            if (code == 1) {
                uint32_t extra = 0;
                if (!bits->read(4, &extra))
                    return false;
                run = extra + 3;
            } else if (code == 2) {
                uint32_t extra = 0;
                if (!bits->read(9, &extra))
                    return false;
                run = extra + 20;
            }
            if (run > count - index)
                return false;
            index += run;
            continue;
        }
        const uint32_t length = code - 2;
        if (length > 16)
            return false;
        lengths[index++] = static_cast<uint8_t>(length);
    }
    return tree->build(lengths);
}

bool readPositionTree(BitReader* bits, HuffmanTree* tree) {
    uint32_t count = 0;
    if (bits == nullptr || tree == nullptr || !bits->read(6, &count))
        return false;
    if (count == 0) {
        uint32_t symbol = 0;
        if (!bits->read(6, &symbol) || symbol >= kPositionSymbols)
            return false;
        tree->setConstant(symbol);
        return true;
    }
    if (count > kPositionSymbols)
        return false;

    std::vector<uint8_t> lengths(kPositionSymbols, 0);
    for (unsigned index = 0; index < count; ++index) {
        uint32_t length = 0;
        if (!bits->read(3, &length))
            return false;
        if (length == 7) {
            uint32_t continuation = 0;
            do {
                if (!bits->read(1, &continuation))
                    return false;
                if (continuation != 0)
                    ++length;
            } while (continuation != 0);
        }
        if (length > 16)
            return false;
        lengths[index] = static_cast<uint8_t>(length);
    }
    return tree->build(lengths);
}

bool readBlock(BitReader* bits, HuffmanTree* literalLengthTree,
               HuffmanTree* positionTree, uint32_t* commandCount) {
    if (bits == nullptr || literalLengthTree == nullptr || positionTree == nullptr ||
        commandCount == nullptr || !bits->read(16, commandCount) || *commandCount == 0)
        return false;
    HuffmanTree codeLengthTree;
    return readCodeLengthTree(bits, &codeLengthTree) &&
           readLiteralLengthTree(bits, codeLengthTree, literalLengthTree) &&
           readPositionTree(bits, positionTree);
}

bool decodeMatchLength(BitReader* bits, uint32_t code, size_t* length) {
    if (bits == nullptr || length == nullptr || code < 256 || code >= kLiteralLengthSymbols)
        return false;
    if (code <= 263) {
        *length = code - 253;
        return true;
    }
    if (code <= 287) {
        const unsigned lowBitCount = static_cast<unsigned>((code - 260) / 4);
        uint32_t lowBits = 0;
        if (!bits->read(lowBitCount, &lowBits))
            return false;
        *length = ((4 + (code % 4)) << lowBitCount) + lowBits + 3;
        return *length <= kMaxCopyLength;
    }
    if (code == 288) {
        *length = 514;
        return true;
    }
    return false;
}

bool decodeDistance(BitReader* bits, const HuffmanTree& positionTree,
                    size_t* distance) {
    uint32_t code = 0;
    if (bits == nullptr || distance == nullptr || !positionTree.decode(bits, &code) ||
        code > 31)
        return false;
    uint32_t offset = code;
    if (code >= 4) {
        const unsigned lowBitCount = static_cast<unsigned>((code - 2) / 2);
        uint32_t lowBits = 0;
        if (!bits->read(lowBitCount, &lowBits))
            return false;
        offset = ((2 + (code % 2)) << lowBitCount) + lowBits;
    }
    *distance = static_cast<size_t>(offset) + 1;
    return *distance <= kHistorySize;
}

bool decodeLk7(const uint8_t* input, size_t inputSize, size_t outputSize,
               std::vector<uint8_t>* output) {
    if (outputSize == 0)
        return true;
    if (input == nullptr || inputSize == 0)
        return false;

    BitReader bits(input, inputSize);
    HuffmanTree literalLengthTree;
    HuffmanTree positionTree;
    std::vector<uint8_t> history(kHistorySize, static_cast<uint8_t>(' '));
    // Testable hypothesis: LK7 uses the common LH-family space-filled start state.
    size_t writePosition = kHistorySize - 256;
    uint32_t commandCount = 0;
    output->reserve(outputSize);

    while (output->size() < outputSize) {
        if (commandCount == 0 &&
            !readBlock(&bits, &literalLengthTree, &positionTree, &commandCount))
            return false;
        --commandCount;

        uint32_t code = 0;
        if (!literalLengthTree.decode(&bits, &code))
            return false;
        if (code < 256) {
            const uint8_t value = static_cast<uint8_t>(code);
            output->push_back(value);
            history[writePosition] = value;
            writePosition = (writePosition + 1) & kHistoryMask;
            continue;
        }

        size_t length = 0;
        size_t distance = 0;
        if (!decodeMatchLength(&bits, code, &length) || length == 0 ||
            length > outputSize - output->size() ||
            !decodeDistance(&bits, positionTree, &distance))
            return false;

        size_t source = (writePosition + kHistorySize - distance) & kHistoryMask;
        for (size_t i = 0; i < length; ++i) {
            const uint8_t value = history[source];
            source = (source + 1) & kHistoryMask;
            output->push_back(value);
            history[writePosition] = value;
            writePosition = (writePosition + 1) & kHistoryMask;
        }
    }
    return output->size() == outputSize;
}

}  // namespace

bool decompressLhaLk7(const char* method,
                      const uint8_t* input,
                      size_t inputSize,
                      size_t outputSize,
                      std::vector<uint8_t>* output) {
    if (method == nullptr || std::strcmp(method, "-lk7-") != 0 || output == nullptr ||
        (input == nullptr && inputSize != 0))
        return false;
    try {
        output->clear();
        if (outputSize > output->max_size())
            return false;
        output->reserve(outputSize);
        const bool result = decodeLk7(input, inputSize, outputSize, output);
        if (!result)
            output->clear();
        return result;
    } catch (const std::bad_alloc&) {
        output->clear();
        return false;
    } catch (const std::length_error&) {
        output->clear();
        return false;
    }
}
