#include "Rar5Crypto.h"

#include "../../opener/compression/third_party/lzma/Aes.h"

#include <QCryptographicHash>

#include <array>
#include <cstring>
#include <mutex>

namespace peare {
namespace compression {
namespace {

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
    QByteArray digest;
    const QByteArray* source = &key;
    if (key.size() > 64) {
        digest = QCryptographicHash::hash(key, QCryptographicHash::Sha256);
        source = &digest;
    }
    QByteArray normalized(64, char(0));
    std::memcpy(normalized.data(), source->constData(), std::size_t(source->size()));
    QByteArray innerPad(64, char(0x36));
    QByteArray outerPad(64, char(0x5c));
    for (int i = 0; i < 64; ++i) {
        innerPad[i] = char(static_cast<unsigned char>(innerPad[i]) ^
                           static_cast<unsigned char>(normalized[i]));
        outerPad[i] = char(static_cast<unsigned char>(outerPad[i]) ^
                           static_cast<unsigned char>(normalized[i]));
    }
    QCryptographicHash inner(QCryptographicHash::Sha256);
    inner.addData(innerPad);
    inner.addData(data);
    QByteArray outer = outerPad;
    outer.append(inner.result());
    return QCryptographicHash::hash(outer, QCryptographicHash::Sha256);
}

} // namespace

bool deriveRar5Keys(const QByteArray& password, const std::uint8_t* salt,
                    std::uint8_t kdfCount, QByteArray* key, QByteArray* hashKey)
{
    if (!salt || !key || !hashKey || kdfCount > 20) return false;
    const QByteArray saltBytes(reinterpret_cast<const char*>(salt), 16);
    QByteArray block = saltBytes;
    block.append(char(0));
    block.append(char(0));
    block.append(char(0));
    block.append(char(1));
    QByteArray value = hmacSha256(password, block);
    std::array<unsigned char, 32> result = {};
    for (int i = 0; i < 32; ++i)
        result[std::size_t(i)] = static_cast<unsigned char>(value.at(i));
    const auto accumulate = [&password, &value, &result]() {
        value = hmacSha256(password, value);
        for (int i = 0; i < 32; ++i)
            result[std::size_t(i)] ^= static_cast<unsigned char>(value.at(i));
    };
    const std::uint32_t iterations = 1U << kdfCount;
    for (std::uint32_t i = 1; i < iterations; ++i) accumulate();
    *key = QByteArray(reinterpret_cast<const char*>(result.data()), int(result.size()));
    for (int i = 0; i < 16; ++i) accumulate();
    *hashKey = QByteArray(reinterpret_cast<const char*>(result.data()), int(result.size()));
    return true;
}

bool decryptRar5AesCbc(const QByteArray& key, const std::uint8_t* iv,
                       std::vector<std::uint8_t>* data)
{
    if (key.size() != 32 || !iv || !data || (data->size() & 15U) != 0)
        return false;
    static std::once_flag aesTablesOnce;
    std::call_once(aesTablesOnce, AesGenTables);
    alignas(16) UInt32 ivAndSchedule[AES_NUM_IVMRK_WORDS] = {};
    Aes_SetKey_Dec(ivAndSchedule + 4,
                   reinterpret_cast<const Byte*>(key.constData()), 32);
    AesCbc_Init(ivAndSchedule, iv);
    if (!data->empty()) {
        if (!g_AesCbc_Decode) return false;
        g_AesCbc_Decode(ivAndSchedule, data->data(), data->size() / AES_BLOCK_SIZE);
    }
    return true;
}

bool decryptRar5Data(const QByteArray& password, const std::uint8_t* salt,
                     const std::uint8_t* iv, std::uint8_t kdfCount,
                     const std::vector<std::uint8_t>& encrypted,
                     std::vector<std::uint8_t>* decrypted, QByteArray* hashKey)
{
    if (!decrypted || !hashKey || (encrypted.size() & 15U) != 0) return false;
    QByteArray key;
    if (!deriveRar5Keys(password, salt, kdfCount, &key, hashKey)) return false;
    *decrypted = encrypted;
    const bool ok = decryptRar5AesCbc(key, iv, decrypted);
    std::fill(key.begin(), key.end(), char(0));
    return ok;
}

std::uint32_t rar5Checksum(const std::uint8_t* hashKey, std::uint32_t crc)
{
    QByteArray key(reinterpret_cast<const char*>(hashKey), 32);
    QByteArray value(4, char(0));
    for (unsigned i = 0; i < 4; ++i) value[int(i)] = char(crc >> (i * 8));
    const QByteArray digest = hmacSha256(key, value);
    std::uint32_t result = 0;
    for (int i = 0; i < digest.size(); ++i)
        result ^= std::uint32_t(static_cast<unsigned char>(digest[i])) << ((i & 3) * 8);
    return result;
}

} // namespace compression
} // namespace peare
