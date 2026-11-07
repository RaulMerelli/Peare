#include "ArjGost40.h"

#include <algorithm>
#include <array>

namespace peare {
namespace compression {
namespace {

const std::uint8_t kSbox[8][16] = {
    {1, 15, 13, 0, 5, 7, 10, 4, 9, 2, 3, 14, 6, 11, 8, 12},
    {13, 11, 4, 1, 3, 15, 5, 9, 0, 10, 14, 7, 6, 8, 2, 12},
    {4, 11, 10, 0, 7, 2, 1, 13, 3, 6, 8, 5, 9, 12, 15, 14},
    {6, 12, 7, 1, 5, 15, 13, 8, 4, 10, 9, 14, 0, 3, 11, 2},
    {7, 13, 10, 1, 0, 8, 9, 15, 14, 4, 6, 12, 11, 2, 5, 3},
    {5, 8, 1, 13, 10, 3, 4, 2, 14, 15, 12, 7, 6, 0, 9, 11},
    {14, 11, 4, 12, 6, 13, 15, 10, 2, 3, 8, 1, 0, 7, 5, 9},
    {4, 10, 9, 2, 13, 8, 0, 14, 6, 11, 1, 12, 7, 15, 5, 3}
};

std::uint32_t read32(const std::uint8_t* p)
{
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

void write32(std::uint8_t* p, std::uint32_t value)
{
    p[0] = std::uint8_t(value);
    p[1] = std::uint8_t(value >> 8);
    p[2] = std::uint8_t(value >> 16);
    p[3] = std::uint8_t(value >> 24);
}

std::uint32_t roundFunction(std::uint32_t value)
{
    std::uint32_t substituted = 0;
    for (unsigned i = 0; i < 8; ++i)
        substituted |= std::uint32_t(kSbox[7 - i][(value >> (i * 4)) & 0x0fU]) << (i * 4);
    return (substituted << 11) | (substituted >> 21);
}

typedef std::array<std::uint32_t, 8> Key;
typedef std::array<std::uint32_t, 2> Block;

Block encryptBlock(const Key& key, Block value)
{
    for (unsigned round = 0; round < 32; ++round) {
        const unsigned index = round < 24 ? round % 8 : 31 - round;
        const std::uint32_t next = value[1] ^ roundFunction(value[0] + key[index]);
        value[1] = value[0];
        value[0] = next;
    }
    return Block{{value[1], value[0]}};
}

void cryptFeedback(Key const& key, Block* feedback, std::uint8_t* bytes,
                   std::size_t size, bool decrypt)
{
    for (std::size_t offset = 0; offset < size; offset += 8) {
        *feedback = encryptBlock(key, *feedback);
        std::uint8_t stream[8];
        write32(stream, (*feedback)[0]);
        write32(stream + 4, (*feedback)[1]);
        const std::size_t count = size - offset < 8 ? size - offset : 8;
        std::uint8_t ciphertext[8] = {};
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t input = bytes[offset + i];
            ciphertext[i] = decrypt ? input : std::uint8_t(input ^ stream[i]);
            bytes[offset + i] = std::uint8_t(input ^ stream[i]);
        }
        if (!decrypt)
            for (std::size_t i = 0; i < count; ++i) ciphertext[i] = bytes[offset + i];
        std::uint8_t feedbackBytes[8];
        write32(feedbackBytes, (*feedback)[0]);
        write32(feedbackBytes + 4, (*feedback)[1]);
        for (std::size_t i = 0; i < count; ++i) feedbackBytes[i] = ciphertext[i];
        (*feedback)[0] = read32(feedbackBytes);
        (*feedback)[1] = read32(feedbackBytes + 4);
    }
}

Key keyFromPassword(const QByteArray& password)
{
    std::uint8_t bytes[32] = {};
    int passwordIndex = 0;
    for (int round = 0; round < 64; ++round) {
        const int index = round % 5;
        const unsigned shift = unsigned(round % 7);
        bytes[index] = std::uint8_t(bytes[index] +
            (std::uint8_t(password.at(passwordIndex)) << shift));
        if (++passwordIndex == password.size()) passwordIndex = 0;
    }
    Key key;
    for (unsigned i = 0; i < key.size(); ++i) key[i] = read32(bytes + i * 4);
    return key;
}

Block gostTransform(const Key& key, Block value)
{
    for (unsigned cycle = 0; cycle < 3; ++cycle) {
        for (unsigned i = 0; i < 8; ++i) {
            const std::uint32_t next = value[1] ^ roundFunction(value[0] + key[i]);
            value[1] = value[0];
            value[0] = next;
        }
    }
    for (int i = 7; i >= 0; --i) {
        const std::uint32_t next = value[1] ^ roundFunction(value[0] + key[unsigned(i)]);
        value[1] = value[0];
        value[0] = next;
    }
    return Block{{value[1], value[0]}};
}

void gostProcess(std::uint8_t* bytes, std::size_t size, const Key& key,
                 Block* state, bool decrypt)
{
    for (std::size_t offset = 0; offset < size; offset += 8) {
        *state = gostTransform(key, *state);
        std::uint8_t stream[8];
        write32(stream, (*state)[0]);
        write32(stream + 4, (*state)[1]);
        const std::size_t count = std::min<std::size_t>(8, size - offset);
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint8_t input = bytes[offset + i];
            bytes[offset + i] = std::uint8_t(input ^ stream[i]);
            stream[i] = decrypt ? input : bytes[offset + i];
        }
        if (count == 8) {
            (*state)[0] = read32(stream);
            (*state)[1] = read32(stream + 4);
        } else {
            std::uint8_t feedback[8];
            write32(feedback, (*state)[0]);
            write32(feedback + 4, (*state)[1]);
            std::copy(stream, stream + count, feedback);
            (*state)[0] = read32(feedback);
            (*state)[1] = read32(feedback + 4);
        }
    }
}

} // namespace

bool decryptArjGost40(std::vector<std::uint8_t>* data, const QByteArray& password,
                      std::uint8_t modifier, std::uint32_t fileTime)
{
    if (!data || password.isEmpty()) return false;
    Key key = keyFromPassword(password);
    const Block iv{{fileTime, std::uint32_t(std::int32_t(std::int8_t(modifier)))}};
    const Key bootstrap{{3, 10, 6, 12, 5, 9, 0, 7}};
    Block feedback = encryptBlock(bootstrap, iv);
    std::uint8_t work[32];
    for (unsigned i = 0; i < key.size(); ++i) write32(work + i * 4, key[i]);
    for (unsigned i = 0; i < 2048; ++i)
        cryptFeedback(key, &feedback, work, sizeof(work), false);
    for (unsigned i = 0; i < key.size(); ++i) key[i] = read32(work + i * 4);
    feedback = encryptBlock(key, iv);
    if (!data->empty()) cryptFeedback(key, &feedback, data->data(), data->size(), true);
    return true;
}

bool decryptArjGost256(std::vector<std::uint8_t>* data, const QByteArray& password,
                       std::uint8_t modifier, std::uint32_t fileTime,
                       bool longPasswordMode)
{
    if (!data || password.isEmpty() || (longPasswordMode && password.size() <= 32))
        return false;

    Key key{};
    for (int i = 0; i < std::min(password.size(), 32); ++i)
        key[unsigned(i / 4)] |= std::uint32_t(std::uint8_t(password.at(i))) <<
                                (unsigned(i % 4) * 8);
    const Block iv{{fileTime, std::uint32_t(std::int32_t(std::int8_t(modifier)))}};
    const Key bootstrap{{3, 10, 6, 12, 5, 9, 0, 7}};
    Block state = gostTransform(bootstrap, iv);
    Key work = key;
    std::uint8_t material[32];
    for (unsigned i = 0; i < 8; ++i) write32(material + i * 4, work[i]);
    for (unsigned i = 0; i < 2048; ++i)
        gostProcess(material, sizeof(material), key, &state, false);

    if (longPasswordMode) {
        for (unsigned i = 0; i < 8; ++i) {
            const int index = int(i + 8) * 4;
            work[i] = 0;
            for (unsigned b = 0; b < 4; ++b) {
                const int at = index + int(b);
                if (at < password.size()) work[i] |= std::uint32_t(std::uint8_t(password.at(at))) << (b * 8);
            }
        }
        for (unsigned i = 0; i < 2048; ++i)
            gostProcess(material, sizeof(material), work, &state, false);
    }

    for (unsigned i = 0; i < 8; ++i) key[i] = read32(material + i * 4);
    state = gostTransform(key, iv);
    gostProcess(data->data(), data->size(), key, &state, true);
    return true;
}

} // namespace compression
} // namespace peare
