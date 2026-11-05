#ifndef PEARE_COMPRESSION_CRC32_H
#define PEARE_COMPRESSION_CRC32_H

#include <array>
#include <cstddef>
#include <cstdint>

namespace peare {
namespace compression {

inline const std::array<std::array<std::uint32_t, 256>, 8>& crc32Table()
{
    static const std::array<std::array<std::uint32_t, 256>, 8> table = [] {
        std::array<std::array<std::uint32_t, 256>, 8> values = {};
        for (std::uint32_t i = 0; i < values[0].size(); ++i) {
            std::uint32_t crc = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
            values[0][i] = crc;
        }
        for (std::size_t slice = 1; slice < values.size(); ++slice) {
            for (std::size_t i = 0; i < values[slice].size(); ++i) {
                const std::uint32_t crc = values[slice - 1][i];
                values[slice][i] = (crc >> 8) ^ values[0][crc & 0xFFU];
            }
        }
        return values;
    }();
    return table;
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
    const auto& table = crc32Table();
    std::uint32_t crc = 0xFFFFFFFFU;
    while (size >= 8) {
        const std::uint32_t first = std::uint32_t(data[0]) |
            (std::uint32_t(data[1]) << 8) | (std::uint32_t(data[2]) << 16) |
            (std::uint32_t(data[3]) << 24);
        crc ^= first;
        crc = table[7][crc & 0xFFU] ^ table[6][(crc >> 8) & 0xFFU] ^
            table[5][(crc >> 16) & 0xFFU] ^ table[4][crc >> 24] ^
            table[3][data[4]] ^ table[2][data[5]] ^
            table[1][data[6]] ^ table[0][data[7]];
        data += 8;
        size -= 8;
    }
    while (size--)
        crc = (crc >> 8) ^ table[0][(crc ^ *data++) & 0xFFU];
    return ~crc;
}

}
}

#endif
