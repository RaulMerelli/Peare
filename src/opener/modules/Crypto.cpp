#include "Crypto.h"
#include "../../../third_party/tiny_aes/aes.h"

#include <cstring>

namespace peare {

bool aes128CbcDecryptNoPadding(const QByteArray& ciphertext,
                               const std::array<unsigned char, 16>& key,
                               QByteArray* plaintext,
                               QString* error)
{
    if (!plaintext) return false;
    plaintext->clear();
    if (ciphertext.isEmpty() || (ciphertext.size() % 16) != 0) {
        if (error) *error = QStringLiteral("AES-CBC input size is not a non-zero multiple of 16 bytes.");
        return false;
    }

    *plaintext = ciphertext;
    const unsigned char zeroIv[16] = {};
    tiny_aes128_ctx context{};
    tiny_aes128_init_cbc(&context, key.data(), zeroIv);
    tiny_aes128_cbc_decrypt(&context,
                            reinterpret_cast<unsigned char*>(plaintext->data()),
                            static_cast<size_t>(plaintext->size()));
    return true;
}

} // namespace peare
