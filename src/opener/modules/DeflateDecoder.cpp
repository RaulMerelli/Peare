#include "DeflateDecoder.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace peare {
namespace compression {
namespace {

class DecodeFailure : public std::runtime_error {
public:
    explicit DecodeFailure(const char* message) : std::runtime_error(message) {}
};

class BitStream {
public:
    explicit BitStream(InflateInput& input)
        : input_(input), bits_(0), available_(0) {}

    unsigned readBits(unsigned count) {
        if (count > 24) throw DecodeFailure("invalid Deflate bit request");
        while (available_ < count) {
            std::uint8_t value = 0;
            if (!input_.readByte(&value))
                throw DecodeFailure("truncated Deflate stream");
            bits_ |= static_cast<std::uint64_t>(value) << available_;
            available_ += 8;
        }
        const std::uint64_t mask = count == 0 ? 0 : ((std::uint64_t(1) << count) - 1);
        const unsigned value = static_cast<unsigned>(bits_ & mask);
        bits_ >>= count;
        available_ -= count;
        return value;
    }

    void discardToByteBoundary() {
        bits_ = 0;
        available_ = 0;
    }

private:
    InflateInput& input_;
    std::uint64_t bits_;
    unsigned available_;
};

class HuffmanTree {
public:
    HuffmanTree() : maximumLength_(0) {}

    void build(const std::vector<unsigned>& lengths, unsigned permittedMaximum) {
        counts_.assign(permittedMaximum + 1, 0);
        maximumLength_ = 0;
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length > permittedMaximum)
                throw DecodeFailure("invalid Huffman code length");
            if (length != 0) {
                ++counts_[length];
                maximumLength_ = std::max(maximumLength_, length);
            }
        }

        int unused = 1;
        for (unsigned length = 1; length <= permittedMaximum; ++length) {
            unused = (unused << 1) - static_cast<int>(counts_[length]);
            if (unused < 0) throw DecodeFailure("oversubscribed Huffman tree");
        }

        std::vector<unsigned> offsets(permittedMaximum + 1, 0);
        for (unsigned length = 1; length < permittedMaximum; ++length)
            offsets[length + 1] = offsets[length] + counts_[length];

        symbols_.assign(lengths.size(), 0);
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length != 0) symbols_[offsets[length]++] = symbol;
        }
    }

    bool empty() const { return maximumLength_ == 0; }

    unsigned decode(BitStream& input) const {
        if (empty()) throw DecodeFailure("empty Huffman tree");
        unsigned code = 0;
        unsigned firstCode = 0;
        unsigned firstSymbol = 0;
        for (unsigned length = 1; length <= maximumLength_; ++length) {
            code |= input.readBits(1);
            const unsigned count = counts_[length];
            if (code >= firstCode && code - firstCode < count)
                return symbols_[firstSymbol + (code - firstCode)];
            firstSymbol += count;
            firstCode = (firstCode + count) << 1;
            code <<= 1;
        }
        throw DecodeFailure("invalid Huffman symbol");
    }

private:
    std::vector<unsigned> counts_;
    std::vector<unsigned> symbols_;
    unsigned maximumLength_;
};

const unsigned kLengthBase[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
const unsigned kLengthExtra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
const unsigned kDistanceBase[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577
};
const unsigned kDistanceExtra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

void makeFixedTrees(HuffmanTree* literalLength, HuffmanTree* distance) {
    std::vector<unsigned> literalLengths(288, 0);
    for (unsigned symbol = 0; symbol <= 143; ++symbol) literalLengths[symbol] = 8;
    for (unsigned symbol = 144; symbol <= 255; ++symbol) literalLengths[symbol] = 9;
    for (unsigned symbol = 256; symbol <= 279; ++symbol) literalLengths[symbol] = 7;
    for (unsigned symbol = 280; symbol <= 287; ++symbol) literalLengths[symbol] = 8;
    literalLength->build(literalLengths, 15);
    distance->build(std::vector<unsigned>(32, 5), 15);
}

void readDynamicTrees(BitStream& input, HuffmanTree* literalLength,
                      HuffmanTree* distance) {
    const unsigned literalCount = input.readBits(5) + 257;
    const unsigned distanceCount = input.readBits(5) + 1;
    const unsigned codeLengthCount = input.readBits(4) + 4;
    if (literalCount > 286 || distanceCount > 32)
        throw DecodeFailure("invalid dynamic Huffman table size");

    static const unsigned order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::vector<unsigned> codeLengths(19, 0);
    for (unsigned index = 0; index < codeLengthCount; ++index)
        codeLengths[order[index]] = input.readBits(3);

    HuffmanTree codeLengthTree;
    codeLengthTree.build(codeLengths, 7);
    if (codeLengthTree.empty())
        throw DecodeFailure("empty code-length Huffman tree");

    const std::size_t total = literalCount + distanceCount;
    std::vector<unsigned> lengths;
    lengths.reserve(total);
    while (lengths.size() < total) {
        const unsigned symbol = codeLengthTree.decode(input);
        if (symbol <= 15) {
            lengths.push_back(symbol);
            continue;
        }

        unsigned repeat = 0;
        unsigned value = 0;
        if (symbol == 16) {
            if (lengths.empty())
                throw DecodeFailure("repeated Huffman length has no predecessor");
            repeat = input.readBits(2) + 3;
            value = lengths.back();
        } else if (symbol == 17) {
            repeat = input.readBits(3) + 3;
        } else if (symbol == 18) {
            repeat = input.readBits(7) + 11;
        } else {
            throw DecodeFailure("invalid code-length symbol");
        }
        if (repeat > total - lengths.size())
            throw DecodeFailure("Huffman code lengths overflow");
        lengths.insert(lengths.end(), repeat, value);
    }

    std::vector<unsigned> literalLengths(lengths.begin(),
                                         lengths.begin() + literalCount);
    if (literalLengths.size() <= 256 || literalLengths[256] == 0)
        throw DecodeFailure("Deflate block has no end marker");
    std::vector<unsigned> distanceLengths(lengths.begin() + literalCount,
                                          lengths.end());
    literalLength->build(literalLengths, 15);
    distance->build(distanceLengths, 15);
}

std::uint32_t computeAdler32(const std::vector<std::uint8_t>& data) {
    const std::uint32_t modulus = 65521U;
    std::uint32_t low = 1;
    std::uint32_t high = 0;
    std::size_t position = 0;
    while (position < data.size()) {
        const std::size_t end = std::min<std::size_t>(data.size(), position + 5552);
        while (position < end) {
            low += data[position++];
            high += low;
        }
        low %= modulus;
        high %= modulus;
    }
    return (high << 16) | low;
}

enum class CompletionPolicy {
    StopAtOutputLimit,
    RequireEndOfStream
};

bool appendMatch(std::vector<std::uint8_t>* output,
                 const std::vector<std::uint8_t>* dictionary,
                 std::size_t dictionaryStart, unsigned distance,
                 unsigned length, std::size_t outputLimit,
                 std::size_t windowSize, CompletionPolicy policy) {
    const std::size_t dictionarySize = dictionary ? dictionary->size() - dictionaryStart : 0;
    if (distance == 0 || distance > windowSize ||
        distance > dictionarySize + output->size())
        throw DecodeFailure("invalid Deflate match distance");

    for (unsigned index = 0; index < length; ++index) {
        if (output->size() == outputLimit) {
            if (policy == CompletionPolicy::StopAtOutputLimit) return false;
            throw DecodeFailure("Deflate output exceeds limit");
        }
        const std::size_t virtualPosition =
            dictionarySize + output->size() - distance;
        std::uint8_t value = 0;
        if (virtualPosition < dictionarySize) {
            value = (*dictionary)[dictionaryStart + virtualPosition];
        } else {
            value = (*output)[virtualPosition - dictionarySize];
        }
        output->push_back(value);
    }
    return true;
}

bool decodeRaw(InflateInput& source, std::size_t outputLimit,
               CompletionPolicy policy,
               const std::vector<std::uint8_t>* dictionary,
               std::size_t windowSize, std::vector<std::uint8_t>* output) {
    if (!output) throw DecodeFailure("null Deflate output");
    output->clear();
    if (policy == CompletionPolicy::StopAtOutputLimit && outputLimit == 0)
        return true;
    output->reserve(std::min<std::size_t>(outputLimit, 1024 * 1024));

    const std::size_t dictionaryStart =
        dictionary && dictionary->size() > windowSize
            ? dictionary->size() - windowSize
            : 0;
    BitStream input(source);
    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = input.readBits(1) != 0;
        const unsigned blockType = input.readBits(2);

        if (blockType == 0) {
            input.discardToByteBoundary();
            const unsigned length = input.readBits(16);
            const unsigned inverseLength = input.readBits(16);
            if ((length ^ 0xffffU) != inverseLength)
                throw DecodeFailure("invalid stored Deflate block length");
            for (unsigned index = 0; index < length; ++index) {
                const unsigned value = input.readBits(8);
                if (output->size() == outputLimit) {
                    if (policy == CompletionPolicy::StopAtOutputLimit) return true;
                    throw DecodeFailure("Deflate output exceeds limit");
                }
                output->push_back(static_cast<std::uint8_t>(value));
            }
            continue;
        }
        if (blockType == 3)
            throw DecodeFailure("reserved Deflate block type");

        HuffmanTree literalLength;
        HuffmanTree distance;
        if (blockType == 1)
            makeFixedTrees(&literalLength, &distance);
        else
            readDynamicTrees(input, &literalLength, &distance);

        for (;;) {
            const unsigned symbol = literalLength.decode(input);
            if (symbol < 256) {
                if (output->size() == outputLimit) {
                    if (policy == CompletionPolicy::StopAtOutputLimit) return true;
                    throw DecodeFailure("Deflate output exceeds limit");
                }
                output->push_back(static_cast<std::uint8_t>(symbol));
            } else if (symbol == 256) {
                break;
            } else if (symbol <= 285) {
                const unsigned lengthIndex = symbol - 257;
                const unsigned length =
                    kLengthBase[lengthIndex] + input.readBits(kLengthExtra[lengthIndex]);
                const unsigned distanceSymbol = distance.decode(input);
                if (distanceSymbol >= 30)
                    throw DecodeFailure("invalid Deflate distance symbol");
                const unsigned matchDistance =
                    kDistanceBase[distanceSymbol] +
                    input.readBits(kDistanceExtra[distanceSymbol]);
                if (!appendMatch(output, dictionary, dictionaryStart,
                                 matchDistance, length, outputLimit,
                                 windowSize, policy))
                    return true;
            } else {
                throw DecodeFailure("invalid Deflate literal/length symbol");
            }
        }
    }
    return true;
}

void setError(std::string* error, const std::string& text) {
    if (error) *error = text;
}

bool runRaw(InflateInput& input, std::size_t outputLimit,
            CompletionPolicy policy,
            const std::vector<std::uint8_t>* dictionary,
            std::size_t windowSize, std::vector<std::uint8_t>* output,
            std::string* error) {
    if (error) error->clear();
    try {
        return decodeRaw(input, outputLimit, policy, dictionary,
                         windowSize, output);
    } catch (const DecodeFailure& failure) {
        if (output) output->clear();
        setError(error, failure.what());
        return false;
    } catch (const std::bad_alloc&) {
        if (output) output->clear();
        setError(error, "Deflate allocation failed");
        return false;
    }
}

}  // namespace

MemoryInflateInput::MemoryInflateInput(const std::uint8_t* data, std::size_t size)
    : data_(data), size_(size), position_(0) {}

bool MemoryInflateInput::readByte(std::uint8_t* value) {
    if (!value || position_ >= size_ || (!data_ && size_ != 0)) return false;
    *value = data_[position_++];
    return true;
}

std::size_t MemoryInflateInput::consumed() const { return position_; }

bool inflateRawPrefix(InflateInput& input, std::size_t wanted,
                      std::vector<std::uint8_t>* output,
                      std::string* error) {
    if (!runRaw(input, wanted, CompletionPolicy::StopAtOutputLimit,
                nullptr, 32768, output, error))
        return false;
    if (!output || output->size() != wanted) {
        if (output) output->clear();
        setError(error, "Deflate stream ended before requested prefix");
        return false;
    }
    return true;
}

bool inflateRawPrefix(const std::uint8_t* data, std::size_t size,
                      std::size_t wanted, std::vector<std::uint8_t>* output,
                      std::string* error) {
    MemoryInflateInput input(data, size);
    return inflateRawPrefix(input, wanted, output, error);
}

bool inflateRawExact(InflateInput& input, std::size_t expected,
                     const std::vector<std::uint8_t>* dictionary,
                     std::vector<std::uint8_t>* output,
                     std::string* error) {
    if (!runRaw(input, expected, CompletionPolicy::RequireEndOfStream,
                dictionary, 32768, output, error))
        return false;
    if (!output || output->size() != expected) {
        if (output) output->clear();
        setError(error, "Deflate output size mismatch");
        return false;
    }
    return true;
}

bool inflateRawExact(const std::uint8_t* data, std::size_t size,
                     std::size_t expected,
                     const std::vector<std::uint8_t>* dictionary,
                     std::vector<std::uint8_t>* output,
                     std::string* error) {
    MemoryInflateInput input(data, size);
    return inflateRawExact(input, expected, dictionary, output, error);
}

bool inflateZlib(const std::uint8_t* data, std::size_t size,
                 std::size_t maximum, std::vector<std::uint8_t>* output,
                 std::string* error) {
    if (error) error->clear();
    if (!output) {
        setError(error, "null zlib output");
        return false;
    }
    output->clear();
    if (!data || size < 6) {
        setError(error, "truncated zlib stream");
        return false;
    }

    const unsigned cmf = data[0];
    const unsigned flg = data[1];
    if ((cmf & 0x0fU) != 8U || (cmf >> 4) > 7U ||
        ((cmf << 8) + flg) % 31U != 0U) {
        setError(error, "invalid zlib header");
        return false;
    }
    if ((flg & 0x20U) != 0) {
        setError(error, "preset zlib dictionaries are unsupported");
        return false;
    }

    const std::size_t payloadSize = size - 6;
    MemoryInflateInput input(data + 2, payloadSize);
    const std::size_t windowSize = std::size_t(1) << ((cmf >> 4) + 8);
    if (!runRaw(input, maximum, CompletionPolicy::RequireEndOfStream,
                nullptr, windowSize, output, error))
        return false;

    const std::uint32_t expectedAdler =
        (static_cast<std::uint32_t>(data[size - 4]) << 24) |
        (static_cast<std::uint32_t>(data[size - 3]) << 16) |
        (static_cast<std::uint32_t>(data[size - 2]) << 8) |
        static_cast<std::uint32_t>(data[size - 1]);
    if (computeAdler32(*output) != expectedAdler) {
        output->clear();
        setError(error, "zlib Adler-32 mismatch");
        return false;
    }
    return true;
}

bool inflateZlib(const std::vector<std::uint8_t>& input,
                 std::size_t maximum, std::vector<std::uint8_t>* output,
                 std::string* error) {
    return inflateZlib(input.empty() ? nullptr : input.data(), input.size(),
                       maximum, output, error);
}

bool inflateZlibExact(const std::uint8_t* data, std::size_t size,
                      std::size_t expected, std::vector<std::uint8_t>* output,
                      std::string* error) {
    if (!inflateZlib(data, size, expected, output, error)) return false;
    if (output->size() != expected) {
        output->clear();
        setError(error, "zlib output size mismatch");
        return false;
    }
    return true;
}

bool inflateZlibExact(const std::vector<std::uint8_t>& input,
                      std::size_t expected, std::vector<std::uint8_t>* output,
                      std::string* error) {
    return inflateZlibExact(input.empty() ? nullptr : input.data(), input.size(),
                            expected, output, error);
}

}  // namespace compression
}  // namespace peare
