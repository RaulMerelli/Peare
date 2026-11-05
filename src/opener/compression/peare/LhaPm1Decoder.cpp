#include "LhaPm1Decoder.h"

#include <array>
#include <vector>

namespace peare {
namespace compression {
namespace {

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size), byteOffset_(0), buffer_(0), available_(0) {}

    bool read(unsigned count, unsigned* value) {
        if (!value || count > 32) return false;
        while (available_ < count) {
            if (byteOffset_ >= size_) return false;
            buffer_ = (buffer_ << 8) | data_[byteOffset_++];
            available_ += 8;
        }
        const unsigned shift = available_ - count;
        const std::uint64_t mask = count == 32 ? UINT64_C(0xffffffff)
                                                : ((UINT64_C(1) << count) - 1);
        *value = static_cast<unsigned>((buffer_ >> shift) & mask);
        available_ -= count;
        if (available_ == 0) buffer_ = 0;
        else buffer_ &= (UINT64_C(1) << available_) - 1;
        return true;
    }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t byteOffset_;
    std::uint64_t buffer_;
    unsigned available_;
};

struct Codeword {
    const char* path;
    unsigned category;
};

struct CategoryTable {
    const Codeword* words;
    std::size_t count;
};

#define PM1_TABLE(name, ...) \
    static const Codeword name[] = {__VA_ARGS__}

PM1_TABLE(t0, {"0000", 0}, {"0001", 1}, {"001", 2}, {"01", 3}, {"10", 4}, {"11", 5});
PM1_TABLE(t1, {"000", 0}, {"001", 1}, {"010", 2}, {"10", 3}, {"11", 4}, {"011", 5});
PM1_TABLE(t2, {"000", 0}, {"001", 1}, {"01", 2}, {"10", 3}, {"110", 4}, {"111", 5});
PM1_TABLE(t3, {"00", 0}, {"010", 1}, {"011", 2}, {"10", 3}, {"110", 4}, {"111", 5});
PM1_TABLE(t4, {"00", 0}, {"010", 1}, {"10", 2}, {"011", 3}, {"110", 4}, {"111", 5});
PM1_TABLE(t5, {"00", 0}, {"010", 1}, {"10", 2}, {"11", 3}, {"0110", 4}, {"0111", 5});
PM1_TABLE(t6, {"00", 0}, {"01", 1}, {"100", 2}, {"101", 3}, {"110", 4}, {"111", 5});
PM1_TABLE(t7, {"00", 0}, {"01", 1}, {"100", 2}, {"11", 3}, {"1010", 4}, {"1011", 5});
PM1_TABLE(t8, {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"1110", 4}, {"1111", 5});
PM1_TABLE(t9, {"0", 0}, {"1000", 1}, {"101", 2}, {"110", 3}, {"111", 4}, {"1001", 5});
PM1_TABLE(t10, {"0", 0}, {"1000", 1}, {"101", 2}, {"11", 3}, {"10010", 4}, {"10011", 5});
PM1_TABLE(t11, {"0", 0}, {"1000", 1}, {"1001", 2}, {"101", 3}, {"110", 4}, {"111", 5});
PM1_TABLE(t12, {"0", 0}, {"100", 1}, {"1010", 2}, {"110", 3}, {"111", 4}, {"1011", 5});
PM1_TABLE(t13, {"0", 0}, {"100", 1}, {"101", 2}, {"110", 3}, {"1110", 4}, {"1111", 5});
PM1_TABLE(t14, {"0", 0}, {"100", 1}, {"11", 2}, {"1010", 3}, {"10110", 4}, {"10111", 5});
PM1_TABLE(t15, {"0", 0}, {"10", 1}, {"1100", 2}, {"1101", 3}, {"1110", 4}, {"1111", 5});
PM1_TABLE(t16, {"0", 0}, {"10", 1}, {"110", 2}, {"1110", 3}, {"11110", 4}, {"11111", 5});
PM1_TABLE(t17, {"01", 2}, {"000", 3}, {"10", 3}, {"001", 4}, {"11", 4});
PM1_TABLE(t18, {"00", 0}, {"010", 1}, {"10", 2}, {"11", 3}, {"011", 4});
PM1_TABLE(t19, {"00", 0}, {"01", 1}, {"10", 2}, {"110", 3}, {"111", 4});
PM1_TABLE(t20, {"0", 0}, {"1000", 1}, {"101", 2}, {"11", 3}, {"1001", 4});
PM1_TABLE(t21, {"0", 0}, {"100", 1}, {"101", 2}, {"110", 3}, {"111", 4});
PM1_TABLE(t22, {"0", 0}, {"100", 1}, {"11", 2}, {"1010", 3}, {"1011", 4});
PM1_TABLE(t23, {"0", 0}, {"10", 1}, {"110", 2}, {"1110", 3}, {"1111", 4});
PM1_TABLE(t24, {"000", 0}, {"001", 1}, {"01", 2}, {"1", 3});
PM1_TABLE(t25, {"00", 0}, {"010", 1}, {"1", 2}, {"011", 3});
PM1_TABLE(t26, {"00", 0}, {"01", 1}, {"10", 2}, {"11", 3});
PM1_TABLE(t27, {"0", 0}, {"100", 1}, {"11", 2}, {"101", 3});
PM1_TABLE(t28, {"0", 0}, {"10", 1}, {"110", 2}, {"111", 3});
PM1_TABLE(t29, {"0", 0}, {"10", 1}, {"11", 2});
PM1_TABLE(t30, {"0", 0}, {"1", 1});

#undef PM1_TABLE

const CategoryTable kCategoryTables[32] = {
    {t0, sizeof(t0) / sizeof(t0[0])}, {t1, sizeof(t1) / sizeof(t1[0])},
    {t2, sizeof(t2) / sizeof(t2[0])}, {t3, sizeof(t3) / sizeof(t3[0])},
    {t4, sizeof(t4) / sizeof(t4[0])}, {t5, sizeof(t5) / sizeof(t5[0])},
    {t6, sizeof(t6) / sizeof(t6[0])}, {t7, sizeof(t7) / sizeof(t7[0])},
    {t8, sizeof(t8) / sizeof(t8[0])}, {t9, sizeof(t9) / sizeof(t9[0])},
    {t10, sizeof(t10) / sizeof(t10[0])}, {t11, sizeof(t11) / sizeof(t11[0])},
    {t12, sizeof(t12) / sizeof(t12[0])}, {t13, sizeof(t13) / sizeof(t13[0])},
    {t14, sizeof(t14) / sizeof(t14[0])}, {t15, sizeof(t15) / sizeof(t15[0])},
    {t16, sizeof(t16) / sizeof(t16[0])},
    {t17, sizeof(t17) / sizeof(t17[0])},
    {t18, sizeof(t18) / sizeof(t18[0])}, {t19, sizeof(t19) / sizeof(t19[0])},
    {t20, sizeof(t20) / sizeof(t20[0])}, {t21, sizeof(t21) / sizeof(t21[0])},
    {t22, sizeof(t22) / sizeof(t22[0])}, {t23, sizeof(t23) / sizeof(t23[0])},
    {t24, sizeof(t24) / sizeof(t24[0])}, {t25, sizeof(t25) / sizeof(t25[0])},
    {t26, sizeof(t26) / sizeof(t26[0])}, {t27, sizeof(t27) / sizeof(t27[0])},
    {t28, sizeof(t28) / sizeof(t28[0])}, {t29, sizeof(t29) / sizeof(t29[0])},
    {t30, sizeof(t30) / sizeof(t30[0])}
};

struct TrieNode {
    int child[2];
    int category;
    TrieNode() : category(-1) {
        child[0] = -1;
        child[1] = -1;
    }
};

class CategoryTree {
public:
    bool build(unsigned selector) {
        nodes_.clear();
        nodes_.push_back(TrieNode());
        if (selector == 31) {
            nodes_[0].category = 0;
            return true;
        }
        if (selector >= sizeof(kCategoryTables) / sizeof(kCategoryTables[0]))
            return false;
        const CategoryTable& table = kCategoryTables[selector];
        for (std::size_t i = 0; i < table.count; ++i)
            if (!insert(table.words[i].path, table.words[i].category)) return false;
        return true;
    }

    bool decode(BitReader& bits, unsigned* category) const {
        if (!category || nodes_.empty()) return false;
        int node = 0;
        for (std::size_t depth = 0; depth <= nodes_.size(); ++depth) {
            if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) return false;
            if (nodes_[node].category >= 0) {
                *category = static_cast<unsigned>(nodes_[node].category);
                return true;
            }
            unsigned bit = 0;
            if (!bits.read(1, &bit)) return false;
            node = nodes_[node].child[bit];
            if (node < 0 || static_cast<std::size_t>(node) >= nodes_.size()) return false;
        }
        return false;
    }

private:
    bool insert(const char* path, unsigned category) {
        int node = 0;
        for (const char* bit = path; *bit; ++bit) {
            if (nodes_[node].category >= 0) return false;
            const unsigned direction = *bit == '1' ? 1 : 0;
            int child = nodes_[node].child[direction];
            if (child < 0) {
                child = static_cast<int>(nodes_.size());
                nodes_[node].child[direction] = child;
                nodes_.push_back(TrieNode());
            }
            node = child;
        }
        if (nodes_[node].category >= 0 || nodes_[node].child[0] >= 0 ||
            nodes_[node].child[1] >= 0)
            return false;
        nodes_[node].category = static_cast<int>(category);
        return true;
    }

    std::vector<TrieNode> nodes_;
};

class Pm1Decoder {
public:
    Pm1Decoder(const std::uint8_t* input, std::size_t inputSize,
               std::size_t outputSize)
        : bits_(input, inputSize), outputSize_(outputSize), writePosition_(0) {
        history_.fill(0);
        const std::uint8_t ranges[][2] = {
            {0x20, 0x7f}, {0x00, 0x1f}, {0xa0, 0xdf}, {0x80, 0x9f}, {0xe0, 0xff}
        };
        std::size_t index = 0;
        for (std::size_t range = 0; range < sizeof(ranges) / sizeof(ranges[0]); ++range)
            for (unsigned value = ranges[range][0]; value <= ranges[range][1]; ++value)
                mtf_[index++] = static_cast<std::uint8_t>(value);
    }

    bool run(std::vector<std::uint8_t>* output) {
        if (outputSize_ == 0) return true;
        unsigned selector = 0;
        if (!bits_.read(5, &selector) || !tree_.build(selector)) return false;
        output->reserve(outputSize_);

        while (output->size() < outputSize_) {
            unsigned command = 0;
            if (!bits_.read(1, &command)) return false;
            if (command == 0) {
                if (!decodeMatch(output)) return false;
                continue;
            }

            unsigned literalCount = 0;
            if (!readLiteralCount(&literalCount))
                return false;
            literalCount = static_cast<unsigned>(std::min<std::size_t>(
                literalCount, outputSize_ - output->size()));
            for (unsigned i = 0; i < literalCount; ++i) {
                unsigned category = 0;
                if (!tree_.decode(bits_, &category) || category >= 6) return false;
                static const unsigned starts[6] = {0, 16, 32, 64, 128, 192};
                static const unsigned widths[6] = {4, 4, 5, 6, 6, 6};
                unsigned extra = 0;
                if (!bits_.read(widths[category], &extra)) return false;
                const unsigned rank = starts[category] + extra;
                if (rank >= 256 || !emit(mtf_[rank], output)) return false;
            }

            if (output->size() == outputSize_) return true;
            if (literalCount < 216 && !decodeMatch(output)) return false;
        }
        return output->size() == outputSize_;
    }

private:
    bool decodeMatch(std::vector<std::uint8_t>* output) {
        unsigned matchClass = 0;
        if (!readMatchClass(output->size(), &matchClass))
            return false;

        unsigned length = 2;
        if (matchClass >= 2 && !readMatchLength(&length)) return false;

        unsigned width = 0;
        unsigned base = 0;
        if (matchClass == 0) { width = 6; base = 0; }
        else if (matchClass == 1) { width = 8; base = 64; }
        else if (matchClass == 2) { width = 6; base = 0; }
        else if (matchClass == 3) {
            base = 64;
            width = output->size() < 320 ? 8 : 9;
        } else if (matchClass == 4) {
            base = 576;
            const std::size_t produced = output->size();
            width = produced < 832 ? 8 : produced < 1088 ? 9 :
                    produced < 1600 ? 10 : 11;
        } else if (matchClass == 5) {
            base = 2624;
            const std::size_t produced = output->size();
            width = produced < 2880 ? 8 : produced < 3136 ? 9 :
                    produced < 3648 ? 10 : produced < 4672 ? 11 :
                    produced < 6720 ? 12 : 13;
        } else {
            return false;
        }

        unsigned extra = 0;
        if (!bits_.read(width, &extra)) return false;
        const std::size_t distance = static_cast<std::size_t>(base) + extra + 1;
        if (distance > output->size() || distance > history_.size())
            return false;
        const std::size_t copyLength = std::min<std::size_t>(
            length, outputSize_ - output->size());
        for (std::size_t i = 0; i < copyLength; ++i) {
            const std::size_t source = (writePosition_ + history_.size() - distance) %
                                       history_.size();
            if (!emit(history_[source], output)) return false;
        }
        return true;
    }

    bool readMatchClass(std::size_t produced, unsigned* matchClass) {
        unsigned first = 0;
        if (!bits_.read(1, &first)) return false;
        if (produced < 64) {
            *matchClass = first ? 2 : 0;
            return true;
        }

        unsigned second = 0;
        if (!bits_.read(1, &second)) return false;
        if (produced < 576) {
            *matchClass = first ? (second ? 2 : 3) : (second ? 1 : 0);
            return true;
        }

        if (!first) {
            if (second) {
                *matchClass = 4;
                return true;
            }
            unsigned third = 0;
            if (!bits_.read(1, &third)) return false;
            *matchClass = third ? 1 : 0;
            return true;
        }
        if (!second) {
            *matchClass = 3;
            return true;
        }
        if (produced < 2624) {
            *matchClass = 2;
            return true;
        }
        unsigned third = 0;
        if (!bits_.read(1, &third)) return false;
        *matchClass = third ? 2 : 5;
        return true;
    }

    bool readMatchLength(unsigned* length) {
        unsigned prefix = 0;
        if (!bits_.read(2, &prefix)) return false;
        if (prefix < 3) {
            *length = 3 + prefix;
            return true;
        }

        unsigned value = 0;
        if (!bits_.read(3, &value)) return false;
        if (value < 5) {
            *length = 6 + value;
            return true;
        }
        if (value == 5) {
            if (!bits_.read(2, &value)) return false;
            *length = 11 + value;
            return true;
        }
        if (value == 6) {
            if (!bits_.read(3, &value)) return false;
            *length = 15 + value;
            return true;
        }

        if (!bits_.read(6, &value)) return false;
        if (value < 62) {
            *length = 23 + value;
            return true;
        }
        if (value == 62) {
            if (!bits_.read(5, &value)) return false;
            *length = 85 + value;
            return true;
        }
        if (!bits_.read(7, &value)) return false;
        *length = 117 + value;
        return true;
    }

    bool readLiteralCount(unsigned* count) {
        unsigned prefix = 0;
        if (!bits_.read(2, &prefix)) return false;
        if (prefix < 3) {
            *count = prefix + 1;
            return true;
        }
        unsigned value = 0;
        if (!bits_.read(3, &value)) return false;
        if (value < 7) {
            *count = 4 + value;
            return true;
        }
        if (!bits_.read(4, &value)) return false;
        if (value < 14) {
            *count = 11 + value;
            return true;
        }
        if (value == 14) {
            if (!bits_.read(6, &value)) return false;
            *count = 25 + value;
            return true;
        }
        if (!bits_.read(7, &value)) return false;
        *count = 89 + value;
        return true;
    }

    bool emit(std::uint8_t value, std::vector<std::uint8_t>* output) {
        if (output->size() >= outputSize_) return false;
        history_[writePosition_] = value;
        writePosition_ = (writePosition_ + 1) % history_.size();
        output->push_back(value);
        for (unsigned i = 255; i > 0; --i) {
            if (mtf_[i] == value) {
                for (unsigned j = i; j > 0; --j) mtf_[j] = mtf_[j - 1];
                mtf_[0] = value;
                break;
            }
        }
        return true;
    }

    BitReader bits_;
    std::size_t outputSize_;
    std::array<std::uint8_t, 16384> history_;
    std::array<std::uint8_t, 256> mtf_;
    std::size_t writePosition_;
    CategoryTree tree_;
};

}  // namespace

bool decompressLhaPm1(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputSize, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    output->clear();
    if (outputSize == 0) return inputSize == 0;
    if (!input || inputSize == 0) return false;
    try {
        Pm1Decoder decoder(input, inputSize, outputSize);
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
