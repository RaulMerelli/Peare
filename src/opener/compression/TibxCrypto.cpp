#include "TibxCrypto.h"
#include "Grasshopper.h"
#include "../../common/compression/AesInitialization.h"

#include "third_party/lzma/Aes.h"

#include <QCryptographicHash>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace peare {
namespace fs {
namespace {

class HmacSha256 {
public:
    explicit HmacSha256(const QByteArray& password) : inner_(64, char(0x36)), outer_(64, char(0x5c)) {
        QByteArray key = password.size() > 64 ? QCryptographicHash::hash(password, QCryptographicHash::Sha256) : password;
        key.append(QByteArray(64 - key.size(), char(0)));
        for (int i = 0; i < 64; ++i) {
            inner_[i] = char(static_cast<unsigned char>(inner_[i]) ^ static_cast<unsigned char>(key[i]));
            outer_[i] = char(static_cast<unsigned char>(outer_[i]) ^ static_cast<unsigned char>(key[i]));
        }
    }
    QByteArray digest(const QByteArray& data) const {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(inner_);
        hash.addData(data);
        const QByteArray innerDigest = hash.result();
        hash.reset();
        hash.addData(outer_);
        hash.addData(innerDigest);
        return hash.result();
    }
private:
    QByteArray inner_, outer_;
};

typedef std::array<std::uint8_t, 16> Block;

// GHASH and counter handling follow NIST SP 800-38D, sections 6 and 7.
void multiply(Block* x, const Block& h) {
    Block z = {}, v = h;
    for (unsigned bit = 0; bit < 128; ++bit) {
        if (((*x)[bit / 8] >> (7 - bit % 8)) & 1U)
            for (unsigned i = 0; i < 16; ++i) z[i] ^= v[i];
        const bool low = (v[15] & 1U) != 0;
        for (int i = 15; i > 0; --i) v[i] = std::uint8_t((v[i] >> 1) | (v[i - 1] << 7));
        v[0] >>= 1;
        if (low) v[0] ^= 0xe1;
    }
    *x = z;
}

void hashBlock(Block* state, const Block& h, const std::uint8_t* input, std::size_t length) {
    for (std::size_t i = 0; i < length; ++i) (*state)[i] ^= input[i];
    multiply(state, h);
}

void length64(std::uint8_t* out, std::uint64_t bits) {
    for (unsigned i = 0; i < 8; ++i) out[7 - i] = std::uint8_t(bits >> (i * 8));
}

void encryptBlock(UInt32* state, const Block& input, Block* output) {
    alignas(16) Byte data[16];
    std::memcpy(data, input.data(), 16);
    const Byte zero[16] = {};
    AesCbc_Init(state, zero);
    AesCbc_Encode(state, data, 1);
    std::copy(data, data + 16, output->begin());
}

} // namespace

bool tibxUnwrapKey(const std::string& password, const std::vector<std::uint8_t>& wrapped,
                   std::vector<std::uint8_t>* key, std::uint8_t* algorithm) {
    if (!key || !algorithm || wrapped.size() < 36 || wrapped[0] != 1 || wrapped[2] < 10 || wrapped[2] > 24 ||
        ((wrapped.size() - 20) % 16) != 0) return false;
    const unsigned alg = wrapped[1];
    const unsigned keySize = alg == 1 || alg == 5 ? 16 : alg == 2 || alg == 6 ? 24 : alg == 3 || alg == 4 || alg == 7 ? 32 : 0;
    if (!keySize) return false;
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) return false;
    const QByteArray passwordBytes(password.data(), static_cast<int>(password.size()));
    const HmacSha256 hmac(passwordBytes);
    QByteArray salt(reinterpret_cast<const char*>(wrapped.data() + 4), 16);
    salt.append("\0\0\0\1", 4);
    QByteArray u = hmac.digest(salt), derived = u;
    const std::uint32_t iterations = std::uint32_t(1) << wrapped[2];
    for (std::uint32_t i = 1; i < iterations; ++i) {
        u = hmac.digest(u);
        for (int j = 0; j < derived.size(); ++j) derived[j] = char(derived[j] ^ u[j]);
    }
    compression::initializeAesTables();
    alignas(16) UInt32 aes[AES_NUM_IVMRK_WORDS] = {};
    Aes_SetKey_Dec(aes + 4, reinterpret_cast<const Byte*>(derived.constData()), 32);
    const Byte iv[16] = {};
    AesCbc_Init(aes, iv);
    std::vector<std::uint8_t> clear(wrapped.begin() + 20, wrapped.end());
    AesCbc_Decode(aes, clear.data(), clear.size() / 16);
    const unsigned padding = clear.back();
    if (!padding || padding > 16 || clear.size() != keySize + padding) return false;
    for (std::size_t i = clear.size() - padding; i < clear.size(); ++i) if (clear[i] != padding) return false;
    clear.resize(keySize);
    key->swap(clear);
    *algorithm = std::uint8_t(alg);
    return true;
}

bool tibxDecrypt(const std::vector<std::uint8_t>& key, std::uint8_t algorithm,
                 const std::vector<std::uint8_t>& input, std::vector<std::uint8_t>* output) {
    if (!output) return false;
    if (algorithm == 4) {
        if (key.size() != 32 || input.size() < 32 || (input.size() - 16) % 16) {
            output->clear();
            return false;
        }
        const std::vector<std::uint8_t> iv(input.begin(), input.begin() + 16);
        const std::vector<std::uint8_t> ciphertext(input.begin() + 16, input.end());
        return grasshopper::decryptCbcPkcs7(key, iv, ciphertext, *output);
    }
    if (input.size() < 32 || (key.size() != 16 && key.size() != 24 && key.size() != 32)) return false;
    compression::initializeAesTables();
    alignas(16) UInt32 aes[AES_NUM_IVMRK_WORDS] = {};
    if (algorithm >= 1 && algorithm <= 3) {
        if ((input.size() - 16) % 16) return false;
        Aes_SetKey_Dec(aes + 4, key.data(), unsigned(key.size()));
        AesCbc_Init(aes, input.data());
        std::vector<std::uint8_t> clear(input.begin() + 16, input.end());
        AesCbc_Decode(aes, clear.data(), clear.size() / 16);
        const unsigned padding = clear.back();
        if (!padding || padding > 16 || padding > clear.size()) return false;
        for (std::size_t i = clear.size() - padding; i < clear.size(); ++i) if (clear[i] != padding) return false;
        clear.resize(clear.size() - padding);
        output->swap(clear);
        return true;
    }
    if (algorithm < 5 || algorithm > 7) return false;
    Aes_SetKey_Enc(aes + 4, key.data(), unsigned(key.size()));
    Block h = {}, zero = {}, j = {}, len = {};
    encryptBlock(aes, zero, &h);
    hashBlock(&j, h, input.data(), 16);
    length64(len.data() + 8, 128);
    hashBlock(&j, h, len.data(), 16);
    Block auth = {};
    const std::size_t size = input.size() - 32;
    const std::uint8_t* cipher = input.data() + 32;
    for (std::size_t pos = 0; pos < size; pos += 16) hashBlock(&auth, h, cipher + pos, std::min<std::size_t>(16, size - pos));
    len.fill(0);
    length64(len.data() + 8, std::uint64_t(size) * 8);
    hashBlock(&auth, h, len.data(), 16);
    Block mask;
    encryptBlock(aes, j, &mask);
    unsigned difference = 0;
    for (unsigned i = 0; i < 16; ++i) difference |= unsigned(auth[i] ^ mask[i] ^ input[16 + i]);
    if (difference) return false;
    std::vector<std::uint8_t> clear(size);
    for (std::size_t pos = 0; pos < size; pos += 16) {
        for (int i = 15; i >= 12; --i) if (++j[i]) break;
        encryptBlock(aes, j, &mask);
        const std::size_t count = std::min<std::size_t>(16, size - pos);
        for (std::size_t i = 0; i < count; ++i) clear[pos + i] = cipher[pos + i] ^ mask[i];
    }
    output->swap(clear);
    return true;
}

} // namespace fs
} // namespace peare
