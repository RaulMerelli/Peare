#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace prosave_embedded {

class InflateError : public std::runtime_error {
public:
    explicit InflateError(const char* message) : std::runtime_error(message) {}
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size), pos_(0), bits_(0), count_(0) {}

    unsigned readBits(unsigned count) {
        while (count_ < count) {
            if (pos_ >= size_) throw InflateError("truncated deflate stream");
            bits_ |= static_cast<std::uint64_t>(data_[pos_++]) << count_;
            count_ += 8;
        }
        const unsigned value = static_cast<unsigned>(bits_ & ((std::uint64_t(1) << count) - 1));
        bits_ >>= count;
        count_ -= count;
        return value;
    }

    void alignByte() { bits_ = 0; count_ = 0; }
    std::size_t bytePosition() const { return pos_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
    std::uint64_t bits_;
    unsigned count_;
};

struct Huffman {
    std::vector<unsigned> counts;
    std::vector<unsigned> symbols;

    void build(const std::vector<unsigned>& lengths, unsigned maxBits = 15) {
        counts.assign(maxBits + 1, 0);
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            if (lengths[i] > maxBits) throw InflateError("invalid huffman code length");
            if (lengths[i]) ++counts[lengths[i]];
        }
        unsigned left = 1;
        for (unsigned len = 1; len <= maxBits; ++len) {
            left <<= 1;
            if (counts[len] > left) throw InflateError("oversubscribed huffman tree");
            left -= counts[len];
        }
        std::vector<unsigned> offsets(maxBits + 1, 0);
        for (unsigned len = 1; len < maxBits; ++len) offsets[len + 1] = offsets[len] + counts[len];
        symbols.assign(lengths.size(), 0);
        for (unsigned symbol = 0; symbol < lengths.size(); ++symbol) {
            const unsigned len = lengths[symbol];
            if (len) symbols[offsets[len]++] = symbol;
        }
    }

    unsigned decode(BitReader& reader) const {
        unsigned code = 0;
        unsigned first = 0;
        unsigned index = 0;
        for (unsigned len = 1; len < counts.size(); ++len) {
            code |= reader.readBits(1);
            const unsigned count = counts[len];
            if (code < first + count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        throw InflateError("invalid huffman symbol");
    }
};

inline std::uint32_t adler32(const std::vector<std::uint8_t>& data) {
    const std::uint32_t mod = 65521U;
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < data.size(); ++i) {
        a = (a + data[i]) % mod;
        b = (b + a) % mod;
    }
    return (b << 16) | a;
}

inline void fixedTrees(Huffman& litlen, Huffman& dist) {
    std::vector<unsigned> ll(288, 0), dd(32, 5);
    for (unsigned i = 0; i <= 143; ++i) ll[i] = 8;
    for (unsigned i = 144; i <= 255; ++i) ll[i] = 9;
    for (unsigned i = 256; i <= 279; ++i) ll[i] = 7;
    for (unsigned i = 280; i <= 287; ++i) ll[i] = 8;
    litlen.build(ll);
    dist.build(dd);
}

inline void dynamicTrees(BitReader& reader, Huffman& litlen, Huffman& dist) {
    const unsigned hlit = reader.readBits(5) + 257;
    const unsigned hdist = reader.readBits(5) + 1;
    const unsigned hclen = reader.readBits(4) + 4;
    static const unsigned order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    std::vector<unsigned> codeLengths(19, 0);
    for (unsigned i = 0; i < hclen; ++i) codeLengths[order[i]] = reader.readBits(3);
    Huffman codeTree;
    codeTree.build(codeLengths, 7);
    std::vector<unsigned> lengths;
    lengths.reserve(hlit + hdist);
    while (lengths.size() < hlit + hdist) {
        const unsigned symbol = codeTree.decode(reader);
        if (symbol <= 15) {
            lengths.push_back(symbol);
        } else if (symbol == 16) {
            if (lengths.empty()) throw InflateError("invalid repeated code length");
            const unsigned repeat = reader.readBits(2) + 3;
            const unsigned value = lengths.back();
            if (lengths.size() + repeat > hlit + hdist) throw InflateError("code lengths overflow");
            lengths.insert(lengths.end(), repeat, value);
        } else if (symbol == 17) {
            const unsigned repeat = reader.readBits(3) + 3;
            if (lengths.size() + repeat > hlit + hdist) throw InflateError("code lengths overflow");
            lengths.insert(lengths.end(), repeat, 0);
        } else if (symbol == 18) {
            const unsigned repeat = reader.readBits(7) + 11;
            if (lengths.size() + repeat > hlit + hdist) throw InflateError("code lengths overflow");
            lengths.insert(lengths.end(), repeat, 0);
        } else {
            throw InflateError("invalid code-length symbol");
        }
    }
    std::vector<unsigned> ll(lengths.begin(), lengths.begin() + hlit);
    std::vector<unsigned> dd(lengths.begin() + hlit, lengths.end());
    litlen.build(ll);
    dist.build(dd);
}

inline std::vector<std::uint8_t> inflateZlib(const std::vector<std::uint8_t>& input, std::size_t limit = static_cast<std::size_t>(-1)) {
    if (input.size() < 6) throw InflateError("truncated zlib stream");
    const unsigned cmf = input[0], flg = input[1];
    if ((cmf & 15U) != 8U || (cmf >> 4U) > 7U || ((cmf << 8U) + flg) % 31U != 0U || (flg & 32U))
        throw InflateError("invalid zlib header");

    BitReader reader(input.data() + 2, input.size() - 6);
    std::vector<std::uint8_t> out;
    static const unsigned lengthBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const unsigned lengthExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const unsigned distBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const unsigned distExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

    bool finalBlock = false;
    while (!finalBlock) {
        finalBlock = reader.readBits(1) != 0;
        const unsigned type = reader.readBits(2);
        if (type == 0) {
            reader.alignByte();
            const unsigned len = reader.readBits(16);
            const unsigned nlen = reader.readBits(16);
            if ((len ^ 0xFFFFU) != nlen) throw InflateError("invalid stored block length");
            if (out.size() + len > limit) throw InflateError("inflate output limit exceeded");
            for (unsigned i = 0; i < len; ++i) out.push_back(static_cast<std::uint8_t>(reader.readBits(8)));
            continue;
        }
        if (type == 3) throw InflateError("reserved deflate block type");
        Huffman litlen, dist;
        if (type == 1) fixedTrees(litlen, dist); else dynamicTrees(reader, litlen, dist);
        for (;;) {
            const unsigned symbol = litlen.decode(reader);
            if (symbol < 256) {
                if (out.size() == limit) throw InflateError("inflate output limit exceeded");
                out.push_back(static_cast<std::uint8_t>(symbol));
            } else if (symbol == 256) {
                break;
            } else if (symbol <= 285) {
                const unsigned li = symbol - 257;
                const unsigned length = lengthBase[li] + reader.readBits(lengthExtra[li]);
                const unsigned ds = dist.decode(reader);
                if (ds >= 30) throw InflateError("invalid deflate distance symbol");
                const unsigned distance = distBase[ds] + reader.readBits(distExtra[ds]);
                if (distance == 0 || distance > out.size()) throw InflateError("invalid deflate distance");
                if (out.size() + length > limit) throw InflateError("inflate output limit exceeded");
                for (unsigned i = 0; i < length; ++i) out.push_back(out[out.size() - distance]);
            } else {
                throw InflateError("invalid literal/length symbol");
            }
        }
    }
    const std::size_t n = input.size();
    const std::uint32_t expected = (std::uint32_t(input[n-4]) << 24) | (std::uint32_t(input[n-3]) << 16) |
                                   (std::uint32_t(input[n-2]) << 8) | std::uint32_t(input[n-1]);
    if (adler32(out) != expected) throw InflateError("zlib adler32 mismatch");
    return out;
}

} // namespace prosave_embedded
