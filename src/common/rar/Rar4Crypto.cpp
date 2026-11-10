#include "Rar4Crypto.h"

#include "../../opener/compression/third_party/lzma/Aes.h"

#include <QCryptographicHash>

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace peare {
namespace compression {
namespace {

bool deriveRar3Key(const QString& password, const std::uint8_t* salt,
                   std::array<std::uint8_t, 16>* key,
                   std::array<std::uint8_t, 16>* iv)
{
    if (!salt || !key || !iv || password.size() > 127) return false;
    QByteArray input(password.size() * 2 + 8, char(0));
    for (int i = 0; i < password.size(); ++i) {
        const std::uint16_t unit = password.at(i).unicode();
        input[i * 2] = char(unit & 0xff);
        input[i * 2 + 1] = char(unit >> 8);
    }
    std::memcpy(input.data() + password.size() * 2, salt, 8);

    QCryptographicHash hash(QCryptographicHash::Sha1);
    std::array<char, 3> counter = {};
    QByteArray digest;
    const unsigned rounds = 0x40000;
    for (unsigned i = 0; i < rounds; ++i) {
        counter[0] = char(i);
        counter[1] = char(i >> 8);
        counter[2] = char(i >> 16);
        hash.addData(input.constData(), input.size());
        hash.addData(counter.data(), int(counter.size()));
        if (i % (rounds / 16) == 0) {
            digest = hash.result();
            (*iv)[i / (rounds / 16)] = std::uint8_t(digest.at(19));
        }
    }
    digest = hash.result();
    for (unsigned word = 0; word < 4; ++word) {
        for (unsigned byte = 0; byte < 4; ++byte)
            (*key)[word * 4 + byte] = std::uint8_t(digest.at(int(word * 4 + (3 - byte))));
    }
    std::fill(input.begin(), input.end(), char(0));
    std::fill(digest.begin(), digest.end(), char(0));
    std::fill(counter.begin(), counter.end(), char(0));
    return true;
}

} // namespace

bool decryptRar4Data(const QString& password, const std::uint8_t* salt,
                     const std::uint8_t* input, std::size_t inputSize,
                     std::vector<std::uint8_t>* output)
{
    if (!output || (!input && inputSize) || inputSize % AES_BLOCK_SIZE != 0) return false;
    std::array<std::uint8_t, 16> key = {};
    std::array<std::uint8_t, 16> iv = {};
    if (!deriveRar3Key(password, salt, &key, &iv)) return false;
    static std::once_flag tablesOnce;
    std::call_once(tablesOnce, AesGenTables);
    alignas(16) UInt32 state[AES_NUM_IVMRK_WORDS] = {};
    Aes_SetKey_Dec(state + 4, key.data(), unsigned(key.size()));
    AesCbc_Init(state, iv.data());
    std::vector<std::uint8_t> decrypted;
    if (inputSize) decrypted.assign(input, input + inputSize);
    if (inputSize && !g_AesCbc_Decode) return false;
    if (inputSize) g_AesCbc_Decode(state, decrypted.data(), inputSize / AES_BLOCK_SIZE);
    std::fill(key.begin(), key.end(), 0);
    std::fill(iv.begin(), iv.end(), 0);
    std::memset(state, 0, sizeof(state));
    output->swap(decrypted);
    return true;
}

} // namespace compression
} // namespace peare
