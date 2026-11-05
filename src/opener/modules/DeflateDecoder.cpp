#include "DeflateDecoder.h"

#include <algorithm>
#include <cstring>
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
        : input_(input), bits_(0), available_(0), directData_(nullptr),
          directSize_(0), directPosition_(0), directPositionTarget_(nullptr),
          hasDirectData_(input.memoryView(&directData_, &directSize_,
                                          &directPositionTarget_)) {
        if (hasDirectData_ && directPositionTarget_)
            directPosition_ = *directPositionTarget_;
    }

    ~BitStream() {
        if (hasDirectData_ && directPositionTarget_)
            *directPositionTarget_ = directPosition_ - available_ / 8;
    }

    unsigned readBits(unsigned count) {
        if (count == 0) return 0;
        if (count > 24) throw DecodeFailure("invalid Deflate bit request");
        while (available_ < count) {
            std::uint8_t value = 0;
            if (!readByte(&value))
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

    bool tryPeekBits(unsigned count, unsigned* value) {
        if (!value || count > 24) return false;
        while (available_ < count) {
            std::uint8_t next = 0;
            if (!readByte(&next)) break;
            bits_ |= static_cast<std::uint64_t>(next) << available_;
            available_ += 8;
        }
        if (available_ < count) return false;
        *value = static_cast<unsigned>(bits_ & ((std::uint64_t(1) << count) - 1));
        return true;
    }

    void dropBits(unsigned count) {
        bits_ >>= count;
        available_ -= count;
    }

    void discardToByteBoundary() {
        const unsigned padding = available_ & 7U;
        dropBits(padding);
    }

    void readAlignedBytes(std::uint8_t* output, std::size_t count) {
        if ((available_ & 7U) != 0)
            throw DecodeFailure("unaligned Deflate byte copy");
        while (count && available_ >= 8) {
            *output++ = static_cast<std::uint8_t>(bits_ & 0xffU);
            bits_ >>= 8;
            available_ -= 8;
            --count;
        }
        if (!count) return;
        if (hasDirectData_) {
            if (count > directSize_ - directPosition_)
                throw DecodeFailure("truncated Deflate stored block");
            std::memcpy(output, directData_ + directPosition_, count);
            directPosition_ += count;
            return;
        }
        while (count--) {
            if (!readByte(output++))
                throw DecodeFailure("truncated Deflate stored block");
        }
    }

private:
    bool readByte(std::uint8_t* value) {
        if (!hasDirectData_) return input_.readByte(value);
        if (!value || directPosition_ >= directSize_) return false;
        *value = directData_[directPosition_++];
        return true;
    }

    InflateInput& input_;
    std::uint64_t bits_;
    unsigned available_;
    const std::uint8_t* directData_;
    std::size_t directSize_;
    std::size_t directPosition_;
    std::size_t* directPositionTarget_;
    bool hasDirectData_;
};

class HuffmanTree {
public:
    HuffmanTree() : maximumLength_(0), fastBits_(kDefaultFastBits) {}

    void build(const std::vector<unsigned>& lengths, unsigned permittedMaximum,
               unsigned fastBits = kDefaultFastBits) {
        if (fastBits == 0 || fastBits > kMaxFastBits)
            throw DecodeFailure("invalid Huffman lookup width");
        fastBits_ = fastBits;
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

        offsets_.assign(permittedMaximum + 1, 0);
        for (unsigned length = 1; length < permittedMaximum; ++length)
            offsets_[length + 1] = offsets_[length] + counts_[length];

        symbols_.assign(lengths.size(), 0);
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length != 0) symbols_[offsets_[length]++] = symbol;
        }

        fastTable_.assign(std::size_t(1) << fastBits_, 0);
        nextCode_.assign(permittedMaximum + 1, 0);
        unsigned code = 0;
        for (unsigned length = 1; length <= permittedMaximum; ++length) {
            code = (code + counts_[length - 1]) << 1;
            nextCode_[length] = code;
        }
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (length == 0 || length > fastBits_) continue;
            unsigned canonical = nextCode_[length]++;
            unsigned reversed = 0;
            for (unsigned bit = 0; bit < length; ++bit) {
                reversed = (reversed << 1) | (canonical & 1U);
                canonical >>= 1;
            }
            const std::size_t stride = std::size_t(1) << length;
            for (std::size_t index = reversed; index < fastTable_.size(); index += stride)
                fastTable_[index] = static_cast<std::uint16_t>((symbol << 4) | length);
        }
    }

    bool empty() const { return maximumLength_ == 0; }

    unsigned decode(BitStream& input) const {
        if (empty()) throw DecodeFailure("empty Huffman tree");
        unsigned prefix = 0;
        if (input.tryPeekBits(fastBits_, &prefix)) {
            const std::uint16_t entry = fastTable_[prefix];
            const unsigned length = entry & 0x0fU;
            if (length) {
                input.dropBits(length);
                return entry >> 4;
            }
        }
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
    static const unsigned kDefaultFastBits = 13;
    static const unsigned kMaxFastBits = 15;

    std::vector<unsigned> counts_;
    std::vector<unsigned> symbols_;
    std::vector<unsigned> offsets_;
    std::vector<unsigned> nextCode_;
    std::vector<std::uint16_t> fastTable_;
    unsigned maximumLength_;
    unsigned fastBits_;
};

struct DeflateWorkspace {
    HuffmanTree literalLength;
    HuffmanTree distance;
    HuffmanTree codeLength;
    std::vector<unsigned> codeLengths;
    std::vector<unsigned> lengths;
    std::vector<unsigned> literalLengths;
    std::vector<unsigned> distanceLengths;
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
const unsigned kDistanceBase64[32] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
    6145, 8193, 12289, 16385, 24577, 32769, 49153
};
const unsigned kDistanceExtra64[32] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13, 14, 14
};

void makeFixedTrees(HuffmanTree* literalLength, HuffmanTree* distance) {
    static const std::vector<unsigned> literalLengths = [] {
        std::vector<unsigned> lengths(288, 0);
        for (unsigned symbol = 0; symbol <= 143; ++symbol) lengths[symbol] = 8;
        for (unsigned symbol = 144; symbol <= 255; ++symbol) lengths[symbol] = 9;
        for (unsigned symbol = 256; symbol <= 279; ++symbol) lengths[symbol] = 7;
        for (unsigned symbol = 280; symbol <= 287; ++symbol) lengths[symbol] = 8;
        return lengths;
    }();
    static const std::vector<unsigned> distanceLengths(32, 5);
    literalLength->build(literalLengths, 15);
    distance->build(distanceLengths, 15);
}

void readDynamicTrees(BitStream& input, HuffmanTree* literalLength,
                      HuffmanTree* distance, HuffmanTree* codeLengthTree,
                      bool deflate64, DeflateWorkspace* workspace) {
    const unsigned literalCount = input.readBits(5) + 257;
    const unsigned distanceCount = input.readBits(5) + 1;
    const unsigned codeLengthCount = input.readBits(4) + 4;
    if (literalCount > (deflate64 ? 288U : 286U) ||
        distanceCount > (deflate64 ? 32U : 30U))
        throw DecodeFailure("invalid dynamic Huffman table size");

    static const unsigned order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::vector<unsigned>& codeLengths = workspace->codeLengths;
    codeLengths.assign(19, 0);
    for (unsigned index = 0; index < codeLengthCount; ++index)
        codeLengths[order[index]] = input.readBits(3);

    codeLengthTree->build(codeLengths, 7, 7);
    if (codeLengthTree->empty())
        throw DecodeFailure("empty code-length Huffman tree");

    const std::size_t total = literalCount + distanceCount;
    std::vector<unsigned>& lengths = workspace->lengths;
    lengths.clear();
    lengths.reserve(total);
    while (lengths.size() < total) {
        const unsigned symbol = codeLengthTree->decode(input);
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

    std::vector<unsigned>& literalLengths = workspace->literalLengths;
    literalLengths.assign(lengths.begin(), lengths.begin() + literalCount);
    if (literalLengths.size() <= 256 || literalLengths[256] == 0)
        throw DecodeFailure("Deflate block has no end marker");
    std::vector<unsigned>& distanceLengths = workspace->distanceLengths;
    distanceLengths.assign(lengths.begin() + literalCount, lengths.end());
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

class BufferOutput {
public:
    BufferOutput(std::uint8_t* buffer, std::size_t capacity)
        : buffer_(buffer), capacity_(capacity), size_(0) {}

    void clear() { size_ = 0; }
    void reserve(std::size_t) {}

    std::size_t size() const { return size_; }

    void resize(std::size_t size) {
        if (size > capacity_) throw DecodeFailure("Deflate output exceeds limit");
        size_ = size;
    }

    void push_back(std::uint8_t value) {
        buffer_[size_++] = value;
    }

    std::uint8_t* data() { return buffer_; }

private:
    std::uint8_t* buffer_;
    std::size_t capacity_;
    std::size_t size_;
};

template <typename Output>
bool appendMatch(Output* output,
                 const std::uint8_t* dictionary, std::size_t dictionarySize,
                 unsigned distance,
                 unsigned length, std::size_t outputLimit,
                 std::size_t windowSize, CompletionPolicy policy) {
    const std::size_t oldSize = output->size();
    if (oldSize > outputLimit)
        throw DecodeFailure("Deflate output exceeds limit");
    if (distance == 0 || distance > windowSize ||
        distance > dictionarySize + oldSize)
        throw DecodeFailure("invalid Deflate match distance");

    const std::size_t available = outputLimit - oldSize;
    if (length > available && policy == CompletionPolicy::RequireEndOfStream)
        throw DecodeFailure("Deflate output exceeds limit");
    const std::size_t copyLength = std::min<std::size_t>(length, available);
    output->resize(oldSize + copyLength);

    std::uint8_t* destination = output->data() + oldSize;
    std::size_t copied = 0;
    const std::size_t sourcePosition = dictionarySize + oldSize - distance;
    if (sourcePosition < dictionarySize) {
        const std::size_t fromDictionary = std::min<std::size_t>(
            copyLength, dictionarySize - sourcePosition);
        std::memcpy(destination, dictionary + sourcePosition, fromDictionary);
        copied = fromDictionary;
    }

    if (copied < copyLength) {
        const std::size_t source = sourcePosition + copied - dictionarySize;
        const std::size_t fromOutput = (distance >= copyLength)
            ? copyLength - copied : distance - copied;
        std::memcpy(destination + copied, output->data() + source, fromOutput);
        copied += fromOutput;
    }

    while (copied < copyLength) {
        const std::size_t count = std::min<std::size_t>(copied, copyLength - copied);
        std::memcpy(destination + copied, destination, count);
        copied += count;
    }
    return copyLength == length;
}

template <typename Output>
bool decodeRaw(InflateInput& source, std::size_t outputLimit,
               CompletionPolicy policy,
               const std::uint8_t* dictionary, std::size_t dictionarySize,
               std::size_t windowSize, bool deflate64, bool exactOutputSize,
               Output* output) {
    if (!output) throw DecodeFailure("null Deflate output");
    output->clear();
    if (policy == CompletionPolicy::StopAtOutputLimit && outputLimit == 0)
        return true;
    output->reserve(exactOutputSize
        ? outputLimit : std::min<std::size_t>(outputLimit, 1024 * 1024));

    if (dictionarySize > windowSize) {
        dictionary += dictionarySize - windowSize;
        dictionarySize = windowSize;
    }
    BitStream input(source);
    bool finalBlock = false;
    static thread_local DeflateWorkspace workspace;
    HuffmanTree& literalLength = workspace.literalLength;
    HuffmanTree& distance = workspace.distance;
    HuffmanTree& codeLength = workspace.codeLength;
    while (!finalBlock) {
        finalBlock = input.readBits(1) != 0;
        const unsigned blockType = input.readBits(2);

        if (blockType == 0) {
            input.discardToByteBoundary();
            const unsigned length = input.readBits(16);
            const unsigned inverseLength = input.readBits(16);
            if ((length ^ 0xffffU) != inverseLength)
                throw DecodeFailure("invalid stored Deflate block length");
            const std::size_t available = outputLimit - output->size();
            if (length > available && policy == CompletionPolicy::RequireEndOfStream)
                throw DecodeFailure("Deflate output exceeds limit");
            const std::size_t copyLength = std::min<std::size_t>(length, available);
            const std::size_t oldSize = output->size();
            output->resize(oldSize + copyLength);
            if (copyLength)
                input.readAlignedBytes(output->data() + oldSize, copyLength);
            if (copyLength != length) return true;
            continue;
        }
        if (blockType == 3)
            throw DecodeFailure("reserved Deflate block type");

        if (blockType == 1)
            makeFixedTrees(&literalLength, &distance);
        else
            readDynamicTrees(input, &literalLength, &distance, &codeLength,
                             deflate64, &workspace);

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
            } else if (symbol <= (deflate64 ? 287U : 285U)) {
                const unsigned lengthIndex = symbol - 257;
                unsigned length = 0;
                if (deflate64 && lengthIndex == 28)
                    length = 3 + input.readBits(16);
                else if (deflate64 && lengthIndex >= 29)
                    length = 3;
                else
                    length = kLengthBase[lengthIndex] + input.readBits(kLengthExtra[lengthIndex]);
                const unsigned distanceSymbol = distance.decode(input);
                if (distanceSymbol >= (deflate64 ? 32U : 30U))
                    throw DecodeFailure("invalid Deflate distance symbol");
                const unsigned matchDistance = deflate64
                    ? kDistanceBase64[distanceSymbol] +
                          input.readBits(kDistanceExtra64[distanceSymbol])
                    : kDistanceBase[distanceSymbol] +
                          input.readBits(kDistanceExtra[distanceSymbol]);
                if (!appendMatch(output, dictionary, dictionarySize,
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
            const std::uint8_t* dictionary, std::size_t dictionarySize,
            std::size_t windowSize, bool deflate64, bool exactOutputSize,
            std::vector<std::uint8_t>* output,
            std::string* error, std::uint8_t* outputBuffer = nullptr,
            std::size_t* outputSize = nullptr) {
    if (error) error->clear();
    if (!output && !outputBuffer) {
        setError(error, "invalid Deflate output buffer");
        return false;
    }
    if (dictionarySize && !dictionary) {
        setError(error, "invalid Deflate history buffer");
        return false;
    }
    try {
        if (output) {
            const bool success = decodeRaw(input, outputLimit, policy,
                                           dictionary, dictionarySize,
                                           windowSize, deflate64, exactOutputSize,
                                           output);
            if (outputSize) *outputSize = output->size();
            return success;
        }
        BufferOutput decoded(outputBuffer, outputLimit);
        const bool success = decodeRaw(input, outputLimit, policy,
                                       dictionary, dictionarySize,
                                       windowSize, deflate64, exactOutputSize,
                                       &decoded);
        if (outputSize) *outputSize = decoded.size();
        return success;
    } catch (const DecodeFailure& failure) {
        if (output) output->clear();
        if (outputSize) *outputSize = 0;
        setError(error, failure.what());
        return false;
    } catch (const std::bad_alloc&) {
        if (output) output->clear();
        if (outputSize) *outputSize = 0;
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

bool MemoryInflateInput::memoryView(const std::uint8_t** data, std::size_t* size,
                                    std::size_t** position) {
    if (!data || !size || !position) return false;
    *data = data_;
    *size = size_;
    *position = &position_;
    return true;
}

bool inflateRawPrefix(InflateInput& input, std::size_t wanted,
                      std::vector<std::uint8_t>* output,
                      std::string* error) {
    if (!runRaw(input, wanted, CompletionPolicy::StopAtOutputLimit,
                nullptr, 0, 32768, false, false, output, error))
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
                dictionary && !dictionary->empty() ? dictionary->data() : nullptr,
                dictionary ? dictionary->size() : 0,
                32768, false, true, output, error))
        return false;
    if (!output || output->size() != expected) {
        if (output) output->clear();
        setError(error, "Deflate output size mismatch");
        return false;
    }
    return true;
}

bool inflateRawExactToBuffer(const std::uint8_t* data, std::size_t size,
                             std::size_t expected, std::uint8_t* output,
                             std::string* error) {
    return inflateRawExactToBuffer(data, size, expected, nullptr, output, error);
}

bool inflateRawExactToBuffer(const std::uint8_t* data, std::size_t size,
                             std::size_t expected,
                             const std::vector<std::uint8_t>* dictionary,
                             std::uint8_t* output, std::string* error) {
    return inflateRawExactToBuffer(
        data, size, expected,
        dictionary && !dictionary->empty() ? dictionary->data() : nullptr,
        dictionary ? dictionary->size() : 0, output, error);
}

bool inflateRawExactToBuffer(const std::uint8_t* data, std::size_t size,
                             std::size_t expected,
                             const std::uint8_t* dictionary,
                             std::size_t dictionarySize,
                             std::uint8_t* output, std::string* error) {
    if (data && size >= 5 && (data[0] & 0x07U) == 0x01U) {
        const std::size_t storedSize = std::size_t(data[1]) |
            (std::size_t(data[2]) << 8);
        const std::size_t inverseSize = std::size_t(data[3]) |
            (std::size_t(data[4]) << 8);
        if (storedSize == expected && (storedSize ^ 0xffffU) == inverseSize &&
            storedSize <= size - 5) {
            if (storedSize && !output) {
                setError(error, "invalid Deflate output buffer");
                return false;
            }
            if (storedSize) std::memcpy(output, data + 5, storedSize);
            if (error) error->clear();
            return true;
        }
    }
    MemoryInflateInput input(data, size);
    std::size_t outputSize = 0;
    if (!runRaw(input, expected, CompletionPolicy::RequireEndOfStream,
                dictionary, dictionarySize, 32768, false, true, nullptr, error,
                output, &outputSize))
        return false;
    if (outputSize != expected) {
        setError(error, "Deflate output size mismatch");
        return false;
    }
    return true;
}

bool inflateRawDeflate64Exact(const std::uint8_t* data, std::size_t size,
                              std::size_t expected,
                              std::vector<std::uint8_t>* output,
                              std::string* error) {
    MemoryInflateInput input(data, size);
    if (!runRaw(input, expected, CompletionPolicy::RequireEndOfStream,
                nullptr, 0, 65536, true, true, output, error))
        return false;
    if (!output || output->size() != expected) {
        if (output) output->clear();
        setError(error, "Deflate64 output size mismatch");
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
                nullptr, 0, windowSize, false, false, output, error))
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

bool inflateRawStream(InflateInput& input, std::size_t maximum,
                      std::vector<std::uint8_t>* output,
                      std::string* error) {
    return runRaw(input, maximum, CompletionPolicy::RequireEndOfStream,
                  nullptr, 0, 32768, false, false, output, error);
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
