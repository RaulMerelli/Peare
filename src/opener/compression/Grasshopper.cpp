#include "Grasshopper.h"

#include <algorithm>
#include <array>
#include <cstddef>

namespace grasshopper {
namespace {

typedef std::array<std::uint8_t, 16> Block;

// RFC 7801, sections 4.1-4.5: substitution, field, and round functions.
const std::uint8_t substitution[256] = {
    252,238,221,17,207,110,49,22,251,196,250,218,35,197,4,77,
    233,119,240,219,147,46,153,186,23,54,241,187,20,205,95,193,
    249,24,101,90,226,92,239,33,129,28,60,66,139,1,142,79,
    5,132,2,174,227,106,143,160,6,11,237,152,127,212,211,31,
    235,52,44,81,234,200,72,171,242,42,104,162,253,58,206,204,
    181,112,14,86,8,12,118,18,191,114,19,71,156,183,93,135,
    21,161,150,41,16,123,154,199,243,145,120,111,157,158,178,177,
    50,117,25,61,255,53,138,126,109,84,198,128,195,189,13,87,
    223,245,36,169,62,168,67,201,215,121,214,246,124,34,185,3,
    224,15,236,222,122,148,176,188,220,232,40,80,78,51,10,74,
    167,151,96,115,30,0,98,68,26,184,56,130,100,159,38,65,
    173,69,70,146,39,94,85,47,140,163,165,125,105,213,149,59,
    7,88,179,64,134,172,29,247,48,55,107,228,136,217,231,137,
    225,27,131,73,76,63,248,254,141,83,170,144,202,216,133,97,
    32,113,103,164,45,43,9,91,203,155,37,208,190,229,108,82,
    89,166,116,210,230,244,180,192,209,102,175,194,57,75,99,182
};

const std::uint8_t coefficients[16] = {
    148,32,133,16,194,192,1,251,1,192,194,16,133,32,148,1
};

std::uint8_t multiply(std::uint8_t a, std::uint8_t b)
{
    unsigned int x = a;
    unsigned int y = b;
    unsigned int product = 0;
    for (int i = 0; i < 8; ++i) {
        if (y & 1U) product ^= x;
        x = (x << 1) ^ ((x & 128U) ? 0x1c3U : 0U);
        y >>= 1;
    }
    return static_cast<std::uint8_t>(product);
}

typedef std::array<std::array<std::uint8_t, 256>, 16> MultiplicationTable;

const MultiplicationTable& multiplicationTable()
{
    static const MultiplicationTable table = []() {
        MultiplicationTable values = {{}};
        for (std::size_t i = 0; i < values.size(); ++i)
            for (std::size_t byte = 0; byte < values[i].size(); ++byte)
                values[i][byte] = multiply(static_cast<std::uint8_t>(byte),
                                           coefficients[i]);
        return values;
    }();
    return table;
}

const std::array<std::uint8_t, 256>& inverseSubstitution()
{
    static const std::array<std::uint8_t, 256> inverse = []() {
        std::array<std::uint8_t, 256> values = {{}};
        for (std::size_t i = 0; i < values.size(); ++i)
            values[substitution[i]] = static_cast<std::uint8_t>(i);
        return values;
    }();
    return inverse;
}

void linear(Block& block)
{
    const MultiplicationTable& table = multiplicationTable();
    for (int round = 0; round < 16; ++round) {
        std::uint8_t value = 0;
        for (std::size_t i = 0; i < block.size(); ++i)
            value ^= table[i][block[i]];
        for (std::size_t i = 15; i > 0; --i) block[i] = block[i - 1];
        block[0] = value;
    }
}

void inverseLinear(Block& block)
{
    const MultiplicationTable& table = multiplicationTable();
    for (int round = 0; round < 16; ++round) {
        const std::uint8_t first = block[0];
        for (std::size_t i = 0; i < 15; ++i) block[i] = block[i + 1];
        block[15] = first;
        std::uint8_t value = 0;
        for (std::size_t i = 0; i < block.size(); ++i)
            value ^= table[i][block[i]];
        block[15] = value;
    }
}

void xorBlock(Block& block, const Block& other)
{
    for (std::size_t i = 0; i < block.size(); ++i) block[i] ^= other[i];
}

std::array<Block, 10> expandKey(const std::vector<std::uint8_t>& key)
{
    std::array<Block, 10> keys;
    std::copy(key.begin(), key.begin() + 16, keys[0].begin());
    std::copy(key.begin() + 16, key.end(), keys[1].begin());
    Block left = keys[0];
    Block right = keys[1];
    for (int i = 1; i <= 32; ++i) {
        Block constant = {{0}};
        constant[15] = static_cast<std::uint8_t>(i);
        linear(constant);
        Block next = left;
        xorBlock(next, constant);
        for (std::size_t j = 0; j < next.size(); ++j)
            next[j] = substitution[next[j]];
        linear(next);
        xorBlock(next, right);
        right = left;
        left = next;
        if (i % 8 == 0) {
            keys[2 * (i / 8)] = left;
            keys[2 * (i / 8) + 1] = right;
        }
    }
    return keys;
}

void decryptBlock(Block& block, const std::array<Block, 10>& keys,
                  const std::array<std::uint8_t, 256>& inverse)
{
    xorBlock(block, keys[9]);
    for (int round = 8; round >= 0; --round) {
        inverseLinear(block);
        for (std::size_t i = 0; i < block.size(); ++i)
            block[i] = inverse[block[i]];
        xorBlock(block, keys[round]);
    }
}

} // namespace

bool decryptCbcPkcs7(const std::vector<std::uint8_t>& key,
                     const std::vector<std::uint8_t>& iv,
                     const std::vector<std::uint8_t>& ciphertext,
                     std::vector<std::uint8_t>& plaintext)
{
    if (key.size() != 32 || iv.size() != 16 || ciphertext.empty() ||
        ciphertext.size() % 16 != 0) {
        plaintext.clear();
        return false;
    }

    const std::array<Block, 10> keys = expandKey(key);
    const std::array<std::uint8_t, 256>& inverse = inverseSubstitution();
    Block previous;
    std::copy(iv.begin(), iv.end(), previous.begin());
    std::vector<std::uint8_t> result(ciphertext.size());

    // NIST SP 800-38A section 6.2: CBC decryption and chaining.
    for (std::size_t offset = 0; offset < ciphertext.size(); offset += 16) {
        Block current;
        std::copy(ciphertext.begin() + offset, ciphertext.begin() + offset + 16,
                  current.begin());
        Block decoded = current;
        decryptBlock(decoded, keys, inverse);
        xorBlock(decoded, previous);
        std::copy(decoded.begin(), decoded.end(), result.begin() + offset);
        previous = current;
    }

    // RFC 5652 section 6.3: all messages have padding, including full blocks.
    const std::size_t padding = result.back();
    if (padding == 0 || padding > 16 ||
        !std::all_of(result.end() - padding, result.end(),
                     [padding](std::uint8_t byte) { return byte == padding; })) {
        plaintext.clear();
        return false;
    }
    result.resize(result.size() - padding);
    plaintext.swap(result);
    return true;
}

} // namespace grasshopper
