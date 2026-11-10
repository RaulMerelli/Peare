#pragma once

#include <QByteArray>

#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool deriveRar5Keys(const QByteArray& password, const std::uint8_t* salt,
                    std::uint8_t kdfCount, QByteArray* key, QByteArray* hashKey);
bool decryptRar5AesCbc(const QByteArray& key, const std::uint8_t* iv,
                       std::vector<std::uint8_t>* data);
bool decryptRar5Data(const QByteArray& password, const std::uint8_t* salt,
                     const std::uint8_t* iv, std::uint8_t kdfCount,
                     const std::vector<std::uint8_t>& encrypted,
                     std::vector<std::uint8_t>* decrypted, QByteArray* hashKey);
std::uint32_t rar5Checksum(const std::uint8_t* hashKey, std::uint32_t crc);

} // namespace compression
} // namespace peare
