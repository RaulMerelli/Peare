#include "SevenZipAesDecoder.h"

#include "third_party/lzma/Aes.h"
#include "third_party/lzma/Sha256.h"

#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace peare {
namespace compression {
namespace {

bool deriveKey(const std::uint8_t* salt, std::size_t saltSize,
               const QByteArray& passwordBytes, unsigned cyclesPower,
               std::array<std::uint8_t, 32>* key)
{
    if (!key || saltSize > 16 || passwordBytes.size() > 198) return false;
    key->fill(0);
    if (cyclesPower == 0x3f) {
        std::size_t position = std::min(saltSize, key->size());
        if (position) std::copy(salt, salt + position, key->begin());
        const std::size_t count = std::min(
            std::size_t(passwordBytes.size()), key->size() - position);
        std::memcpy(key->data() + position, passwordBytes.constData(), count);
        return true;
    }
    if (cyclesPower > 24) return false;

    CSha256 sha = {};
    Sha256_Init(&sha);
    std::array<std::uint8_t, 8> counter = {};
    const std::uint64_t rounds = std::uint64_t(1) << cyclesPower;
    for (std::uint64_t i = 0; i < rounds; ++i) {
        if (saltSize) Sha256_Update(&sha, salt, saltSize);
        if (!passwordBytes.isEmpty())
            Sha256_Update(&sha,
                reinterpret_cast<const std::uint8_t*>(passwordBytes.constData()),
                std::size_t(passwordBytes.size()));
        qToLittleEndian<std::uint64_t>(i, counter.data());
        Sha256_Update(&sha, counter.data(), counter.size());
    }
    Sha256_Final(&sha, key->data());
    std::memset(&sha, 0, sizeof(sha));
    return true;
}

QByteArray utf16LittleEndian(const QString& password)
{
    QByteArray bytes(password.size() * 2, char(0));
    for (int i = 0; i < password.size(); ++i) {
        const std::uint16_t unit = password.at(i).unicode();
        qToLittleEndian<std::uint16_t>(unit,
            reinterpret_cast<uchar*>(bytes.data() + i * 2));
    }
    return bytes;
}

}  // namespace

bool decryptSevenZipAes(const QByteArray& properties, const QString& password,
                        const std::uint8_t* input, std::size_t inputSize,
                        std::size_t outputSize, std::vector<std::uint8_t>* output)
{
    if (!output || (!input && inputSize) || (inputSize & (AES_BLOCK_SIZE - 1)) != 0 ||
        outputSize > inputSize || properties.size() > 34) return false;
    const auto* props = reinterpret_cast<const std::uint8_t*>(properties.constData());
    const std::size_t propsSize = std::size_t(properties.size());
    const unsigned b0 = propsSize ? props[0] : 0;
    const unsigned cyclesPower = b0 & 0x3f;
    std::size_t saltSize = 0;
    std::size_t ivSize = 0;
    std::size_t offset = 0;
    if (b0 & 0xc0) {
        if (propsSize < 2) return false;
        const unsigned b1 = props[1];
        saltSize = ((b0 >> 7) & 1) + (b1 >> 4);
        ivSize = ((b0 >> 6) & 1) + (b1 & 0x0f);
        if (saltSize > 16 || ivSize > AES_BLOCK_SIZE ||
            propsSize != 2 + saltSize + ivSize) return false;
        offset = 2;
    } else if (propsSize != 1) {
        return false;
    }
    if (cyclesPower > 24 && cyclesPower != 0x3f) return false;

    QByteArray passwordBytes = utf16LittleEndian(password);
    std::array<std::uint8_t, 32> key = {};
    const std::uint8_t* salt = saltSize ? props + offset : nullptr;
    if (!deriveKey(salt, saltSize, passwordBytes, cyclesPower, &key)) return false;
    std::array<std::uint8_t, AES_BLOCK_SIZE> iv = {};
    if (ivSize) std::memcpy(iv.data(), props + offset + saltSize, ivSize);

    static std::once_flag aesTablesOnce;
    std::call_once(aesTablesOnce, AesGenTables);
    alignas(16) UInt32 ivAndSchedule[AES_NUM_IVMRK_WORDS] = {};
    Aes_SetKey_Dec(ivAndSchedule + 4, key.data(), unsigned(key.size()));
    AesCbc_Init(ivAndSchedule, iv.data());
    std::vector<std::uint8_t> decrypted;
    if (inputSize) decrypted.assign(input, input + inputSize);
    if (inputSize && (!g_AesCbc_Decode || !decrypted.data())) {
        std::fill(key.begin(), key.end(), 0);
        std::fill(passwordBytes.begin(), passwordBytes.end(), char(0));
        std::memset(ivAndSchedule, 0, sizeof(ivAndSchedule));
        return false;
    }
    if (inputSize)
        g_AesCbc_Decode(ivAndSchedule, decrypted.data(), inputSize / AES_BLOCK_SIZE);
    decrypted.resize(outputSize);
    std::fill(key.begin(), key.end(), 0);
    std::fill(passwordBytes.begin(), passwordBytes.end(), char(0));
    std::memset(ivAndSchedule, 0, sizeof(ivAndSchedule));
    output->swap(decrypted);
    return true;
}

}  // namespace compression
}  // namespace peare
