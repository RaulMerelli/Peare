#include "LhaPm2Decoder.h"

#include <algorithm>
#include <array>

namespace peare {
namespace compression {
namespace {

const std::size_t kWindowSize = 8192;

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

struct HuffmanTree {
    struct Node {
        int child[2];
        int symbol;
        Node() : child{-1, -1}, symbol(-1) {}
    };

    std::vector<Node> nodes;
    int soleSymbol;

    HuffmanTree() : soleSymbol(-1) {}

    bool build(const std::vector<std::uint8_t>& lengths,
               bool allowSingleSymbol = false) {
        nodes.clear();
        soleSymbol = -1;
        std::array<unsigned, 32> counts = {{0}};
        unsigned active = 0;
        unsigned maximum = 0;
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            if (lengths[i] > 31) return false;
            if (lengths[i]) {
                ++counts[lengths[i]];
                ++active;
                maximum = std::max(maximum, unsigned(lengths[i]));
                soleSymbol = static_cast<int>(i);
            }
        }
        if (active == 0) return false;
        if (active == 1) return allowSingleSymbol;

        std::uint64_t remaining = 1;
        for (unsigned length = 1; length <= maximum; ++length) {
            remaining <<= 1;
            if (counts[length] > remaining) return false;
            remaining -= counts[length];
        }
        if (remaining != 0) return false;

        std::uint32_t code = 0;
        std::array<std::uint32_t, 32> nextCode = {{0}};
        for (unsigned length = 1; length <= maximum; ++length) {
            code = (code + counts[length - 1]) << 1;
            if (code + counts[length] > (UINT32_C(1) << length)) return false;
            nextCode[length] = code;
        }

        nodes.push_back(Node());
        for (std::size_t symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned length = lengths[symbol];
            if (!length) continue;
            const std::uint32_t value = nextCode[length]++;
            int node = 0;
            for (unsigned bitIndex = length; bitIndex > 0; --bitIndex) {
                if (nodes[node].symbol >= 0) return false;
                const unsigned bit = (value >> (bitIndex - 1)) & 1U;
                int child = nodes[node].child[bit];
                if (child < 0) {
                    child = static_cast<int>(nodes.size());
                    nodes[node].child[bit] = child;
                    nodes.push_back(Node());
                }
                node = child;
            }
            if (nodes[node].symbol >= 0 || nodes[node].child[0] >= 0 ||
                nodes[node].child[1] >= 0)
                return false;
            nodes[node].symbol = static_cast<int>(symbol);
        }
        soleSymbol = -1;
        return true;
    }

    bool decode(BitReader& bits, unsigned symbolCount, unsigned* symbol) const {
        if (!symbol) return false;
        if (soleSymbol >= 0) {
            *symbol = static_cast<unsigned>(soleSymbol);
            return true;
        }
        if (nodes.empty()) return false;
        int node = 0;
        for (std::size_t depth = 0; depth <= nodes.size(); ++depth) {
            if (nodes[node].symbol >= 0) {
                *symbol = static_cast<unsigned>(nodes[node].symbol);
                return *symbol < symbolCount;
            }
            std::uint32_t bit = 0;
            if (!bits.read(1, &bit)) return false;
            node = nodes[node].child[bit];
            if (node < 0 || static_cast<std::size_t>(node) >= nodes.size()) return false;
        }
        return false;
    }
};

struct CodeTable {
    HuffmanTree tree;
    unsigned symbolCount;
    unsigned minimumLength;

    CodeTable() : symbolCount(0), minimumLength(0) {}

    bool hasPositionTable() const {
        return symbolCount >= 10 && !(symbolCount == 29 && minimumLength == 0);
    }
};

bool readCodeTable(BitReader& bits, CodeTable* table) {
    std::uint32_t countField = 0;
    std::uint32_t minimum = 0;
    if (!table || !bits.read(5, &countField) || !bits.read(3, &minimum)) return false;
    const unsigned count = countField;
    if (count == 0 || count > 29) return false;
    table->symbolCount = count;
    table->minimumLength = minimum;

    std::vector<std::uint8_t> lengths(count, 0);
    if (minimum == 0) {
        lengths[count - 1] = 1;
        return table->tree.build(lengths, true);
    }

    std::uint32_t fieldWidth = 0;
    if (!bits.read(3, &fieldWidth) || fieldWidth == 0 || fieldWidth > 7) return false;
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t relativeLength = 0;
        if (!bits.read(fieldWidth, &relativeLength)) return false;
        if (relativeLength != 0) {
            const unsigned length = minimum + relativeLength - 1;
            if (length > 31) return false;
            lengths[i] = static_cast<std::uint8_t>(length);
        }
    }
    return table->tree.build(lengths);
}

struct PositionTable {
    HuffmanTree tree;
    unsigned symbolCount;
    PositionTable() : symbolCount(0) {}
};

bool readPositionTable(BitReader& bits, unsigned count, const CodeTable& codes,
                       PositionTable* table) {
    if (!table || count < 5 || count > 8) return false;
    if (!codes.hasPositionTable()) return true;
    std::vector<std::uint8_t> lengths(count);
    for (unsigned i = 0; i < count; ++i) {
        std::uint32_t length = 0;
        if (!bits.read(3, &length)) return false;
        lengths[i] = static_cast<std::uint8_t>(length);
    }
    const std::size_t activeCount = static_cast<std::size_t>(std::count_if(
        lengths.begin(), lengths.end(), [](std::uint8_t length) { return length != 0; }));
    if (activeCount == 0) return table->symbolCount != 0;
    table->symbolCount = count;
    return table->tree.build(lengths, activeCount == 1);
}

class Decoder {
public:
    Decoder(const std::uint8_t* input, std::size_t inputSize, std::size_t outputSize)
        : bits_(input, inputSize), outputSize_(outputSize), writePosition_(0) {
        history_.fill(static_cast<std::uint8_t>(' '));
        const std::uint8_t ranges[][2] = {
            {0x20, 0x7f}, {0x00, 0x1f}, {0xa0, 0xdf}, {0x80, 0x9f}, {0xe0, 0xff}
        };
        std::size_t index = 0;
        for (std::size_t range = 0; range < sizeof(ranges) / sizeof(ranges[0]); ++range)
            for (unsigned value = ranges[range][0]; value <= ranges[range][1]; ++value)
                mtf_[index++] = static_cast<std::uint8_t>(value);
    }

    bool run(std::vector<std::uint8_t>* output) {
        std::uint32_t ignored = 0;
        if (!bits_.read(1, &ignored)) return false;
        CodeTable codes;
        if (!readCodeTable(bits_, &codes)) return false;
        PositionTable positions;
        if (!readPositionTable(bits_, 5, codes, &positions)) return false;

        output->reserve(outputSize_);
        std::size_t nextBoundary = 1024;
        while (output->size() < outputSize_) {
            const std::size_t produced = output->size();
            while (produced >= nextBoundary) {
                if (nextBoundary == 1024) {
                    if (!readPositionTable(bits_, 6, codes, &positions)) {
                        return false;
                    }
                    nextBoundary = 2048;
                } else if (nextBoundary == 2048) {
                    if (!readPositionTable(bits_, 7, codes, &positions)) {
                        return false;
                    }
                    nextBoundary = 4096;
                } else if (nextBoundary == 4096) {
                    std::uint32_t rebuild = 0;
                    if (!bits_.read(1, &rebuild)) return false;
                    if (rebuild && !readCodeTable(bits_, &codes)) return false;
                    if (!readPositionTable(bits_, 8, codes, &positions)) {
                        return false;
                    }
                    nextBoundary = 8192;
                } else {
                    std::uint32_t rebuild = 0;
                    if (!bits_.read(1, &rebuild)) return false;
                    if (rebuild && !readCodeTable(bits_, &codes)) return false;
                    if (rebuild && !readPositionTable(bits_, 8, codes, &positions)) return false;
                    if (nextBoundary > static_cast<std::size_t>(-1) - 4096) return false;
                    nextBoundary += 4096;
                }
            }

            unsigned symbol = 0;
            if (!codes.tree.decode(bits_, codes.symbolCount, &symbol)) return false;
            if (symbol < 8) {
                static const unsigned bases[8] = {0, 8, 16, 32, 64, 96, 128, 192};
                static const unsigned widths[8] = {3, 3, 4, 5, 5, 5, 6, 6};
                std::uint32_t extra = 0;
                if (!bits_.read(widths[symbol], &extra)) return false;
                const unsigned rank = bases[symbol] + extra;
                if (rank >= 256) return false;
                const std::uint8_t value = mtf_[rank];
                for (unsigned i = rank; i > 0; --i) mtf_[i] = mtf_[i - 1];
                mtf_[0] = value;
                if (!emit(value, output)) return false;
                continue;
            }

            unsigned length = 0;
            if (symbol <= 22) length = symbol - 6;
            else if (symbol <= 27) {
                static const unsigned bases[5] = {17, 25, 33, 65, 129};
                static const unsigned widths[5] = {3, 3, 5, 6, 7};
                const unsigned group = symbol - 23;
                std::uint32_t extra = 0;
                if (!bits_.read(widths[group], &extra)) return false;
                length = bases[group] + extra;
            } else if (symbol == 28) length = 256;
            else return false;

            unsigned distance = 0;
            if (symbol == 28) {
                distance = 1;
            } else if (symbol == 8) {
                std::uint32_t direct = 0;
                if (!bits_.read(6, &direct)) return false;
                distance = direct + 1;
            } else {
                unsigned positionSymbol = 0;
                if (!positions.tree.decode(bits_, positions.symbolCount, &positionSymbol))
                    return false;
                if (positionSymbol == 0) {
                    std::uint32_t direct = 0;
                    if (!bits_.read(6, &direct)) return false;
                    distance = direct + 1;
                } else {
                    const unsigned width = positionSymbol + 5;
                    if (width > 12) return false;
                    std::uint32_t extra = 0;
                    if (!bits_.read(width, &extra)) return false;
                    const unsigned base = UINT32_C(1) << width;
                    distance = base + extra + 1;
                }
            }
            if (distance == 0 || distance > kWindowSize ||
                length > outputSize_ - output->size()) return false;
            for (unsigned i = 0; i < length; ++i) {
                const std::size_t source = (writePosition_ + kWindowSize - distance) % kWindowSize;
                const std::uint8_t value = history_[source];
                if (!emit(value, output)) return false;
            }
        }
        return true;
    }

private:
    bool emit(std::uint8_t value, std::vector<std::uint8_t>* output) {
        if (output->size() >= outputSize_) return false;
        history_[writePosition_] = value;
        writePosition_ = (writePosition_ + 1) % kWindowSize;
        output->push_back(value);
        for (unsigned i = 255; i > 0; --i)
            if (mtf_[i] == value) {
                for (unsigned j = i; j > 0; --j) mtf_[j] = mtf_[j - 1];
                mtf_[0] = value;
                break;
            }
        return true;
    }

    BitReader bits_;
    std::size_t outputSize_;
    std::array<std::uint8_t, kWindowSize> history_;
    std::array<std::uint8_t, 256> mtf_;
    std::size_t writePosition_;
};

}  // namespace

bool decompressLhaPm2(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if ((!input && inputSize != 0) || inputSize == 0) return false;
    try {
        Decoder decoder(input, inputSize, outputSize);
        if (!decoder.run(output)) {
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
