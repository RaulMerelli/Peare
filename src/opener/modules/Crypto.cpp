#include "Crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace peare {
namespace {

using Byte = std::uint8_t;
using Block = std::array<Byte, 16>;
using ExpandedKey = std::array<Byte, 176>;

Byte rotateLeft(Byte value, unsigned count)
{
    return static_cast<Byte>((value << count) | (value >> (8u - count)));
}

Byte multiply(Byte left, Byte right)
{
    Byte product = 0;
    while (right != 0) {
        if ((right & 1u) != 0) product ^= left;
        const bool highBit = (left & 0x80u) != 0;
        left = static_cast<Byte>(left << 1);
        if (highBit) left ^= 0x1bu;
        right = static_cast<Byte>(right >> 1);
    }
    return product;
}

Byte multiplicativeInverse(Byte value)
{
    if (value == 0) return 0;

    // In GF(2^8), a^254 is the inverse of every non-zero element.
    Byte result = 1;
    Byte factor = value;
    unsigned exponent = 254;
    while (exponent != 0) {
        if ((exponent & 1u) != 0) result = multiply(result, factor);
        factor = multiply(factor, factor);
        exponent >>= 1;
    }
    return result;
}

Byte forwardSubstitution(Byte value)
{
    const Byte inverse = multiplicativeInverse(value);
    return static_cast<Byte>(inverse ^ rotateLeft(inverse, 1) ^
                             rotateLeft(inverse, 2) ^ rotateLeft(inverse, 3) ^
                             rotateLeft(inverse, 4) ^ 0x63u);
}

const std::array<Byte, 256>& inverseSubstitutionTable()
{
    static const std::array<Byte, 256> table = [] {
        std::array<Byte, 256> generated{};
        for (unsigned value = 0; value < 256; ++value)
            generated[forwardSubstitution(static_cast<Byte>(value))] =
                static_cast<Byte>(value);
        return generated;
    }();
    return table;
}

ExpandedKey expandAes128Key(const std::array<unsigned char, 16>& key)
{
    ExpandedKey expanded{};
    std::memcpy(expanded.data(), key.data(), key.size());

    Byte roundConstant = 1;
    std::size_t produced = key.size();
    while (produced < expanded.size()) {
        std::array<Byte, 4> word = {
            expanded[produced - 4], expanded[produced - 3],
            expanded[produced - 2], expanded[produced - 1]
        };

        if ((produced % 16u) == 0) {
            const Byte first = word[0];
            word[0] = forwardSubstitution(word[1]);
            word[1] = forwardSubstitution(word[2]);
            word[2] = forwardSubstitution(word[3]);
            word[3] = forwardSubstitution(first);
            word[0] ^= roundConstant;
            roundConstant = multiply(roundConstant, 2);
        }

        for (Byte item : word) {
            expanded[produced] = static_cast<Byte>(expanded[produced - 16] ^ item);
            ++produced;
        }
    }
    return expanded;
}

void addRoundKey(Block& state, const ExpandedKey& key, unsigned round)
{
    const std::size_t start = static_cast<std::size_t>(round) * state.size();
    for (std::size_t i = 0; i < state.size(); ++i) state[i] ^= key[start + i];
}

void inverseShiftRows(Block& state)
{
    const Block original = state;
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned column = 0; column < 4; ++column) {
            const unsigned sourceColumn = (column + 4u - row) & 3u;
            state[column * 4u + row] = original[sourceColumn * 4u + row];
        }
    }
}

void inverseSubstituteBytes(Block& state)
{
    const auto& inverse = inverseSubstitutionTable();
    for (Byte& value : state) value = inverse[value];
}

void inverseMixColumns(Block& state)
{
    for (unsigned column = 0; column < 4; ++column) {
        const unsigned base = column * 4u;
        const Byte a = state[base];
        const Byte b = state[base + 1];
        const Byte c = state[base + 2];
        const Byte d = state[base + 3];

        state[base] = static_cast<Byte>(multiply(a, 14) ^ multiply(b, 11) ^
                                       multiply(c, 13) ^ multiply(d, 9));
        state[base + 1] = static_cast<Byte>(multiply(a, 9) ^ multiply(b, 14) ^
                                           multiply(c, 11) ^ multiply(d, 13));
        state[base + 2] = static_cast<Byte>(multiply(a, 13) ^ multiply(b, 9) ^
                                           multiply(c, 14) ^ multiply(d, 11));
        state[base + 3] = static_cast<Byte>(multiply(a, 11) ^ multiply(b, 13) ^
                                           multiply(c, 9) ^ multiply(d, 14));
    }
}

void decryptAes128Block(Block& block, const ExpandedKey& key)
{
    addRoundKey(block, key, 10);
    for (unsigned round = 9; round != 0; --round) {
        inverseShiftRows(block);
        inverseSubstituteBytes(block);
        addRoundKey(block, key, round);
        inverseMixColumns(block);
    }
    inverseShiftRows(block);
    inverseSubstituteBytes(block);
    addRoundKey(block, key, 0);
}

} // namespace

bool aes128CbcDecryptNoPadding(const QByteArray& ciphertext,
                               const std::array<unsigned char, 16>& key,
                               QByteArray* plaintext,
                               QString* error)
{
    if (!plaintext) return false;
    plaintext->clear();

    if (ciphertext.isEmpty() || (ciphertext.size() % 16) != 0) {
        if (error)
            *error = QStringLiteral("AES-CBC input size is not a non-zero multiple of 16 bytes.");
        return false;
    }

    const ExpandedKey expanded = expandAes128Key(key);
    *plaintext = ciphertext;

    Block previous{}; // This format uses an all-zero initialization vector.
    auto* output = reinterpret_cast<Byte*>(plaintext->data());

    for (std::size_t offset = 0; offset < static_cast<std::size_t>(plaintext->size());
         offset += 16) {
        Block encrypted{};
        std::memcpy(encrypted.data(), output + offset, encrypted.size());

        Block decoded = encrypted;
        decryptAes128Block(decoded, expanded);
        for (std::size_t i = 0; i < decoded.size(); ++i)
            output[offset + i] = static_cast<Byte>(decoded[i] ^ previous[i]);

        previous = encrypted;
    }
    return true;
}

} // namespace peare
