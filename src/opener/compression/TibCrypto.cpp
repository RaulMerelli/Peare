#include "TibCrypto.h"
#include "../../common/compression/AesInitialization.h"
#include "third_party/lzma/Aes.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QMessageAuthenticationCode>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace peare {
namespace fs {
namespace {

void addBytes(QCryptographicHash* hash, const std::vector<std::uint8_t>& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const std::size_t count = std::min<std::size_t>(
            data.size() - offset, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        hash->addData(reinterpret_cast<const char*>(data.data() + offset), static_cast<int>(count));
        offset += count;
    }
}

} // namespace

bool tibLegacyKdf(const std::vector<std::uint8_t>& password,
                  const std::vector<std::uint8_t>& salt,
                  std::array<std::uint8_t, 32>* output) {
    if (!output) return false;
    const std::uint64_t maxBytes = std::numeric_limits<std::uint64_t>::max() / 8;
    if (std::uint64_t(salt.size()) > maxBytes - 32 ||
        std::uint64_t(password.size()) > maxBytes - std::uint64_t(salt.size())) return false;

    QCryptographicHash hash(QCryptographicHash::Sha256);
    addBytes(&hash, password);
    addBytes(&hash, salt);
    QByteArray digest = hash.result();
    for (unsigned i = 0; i < 1000; ++i) {
        hash.reset();
        hash.addData(digest);
        addBytes(&hash, salt);
        digest = hash.result();
    }
    if (digest.size() != 32) return false;
    std::array<std::uint8_t, 32> result;
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::uint8_t>(static_cast<unsigned char>(digest.at(static_cast<int>(i))));
    *output = result;
    return true;
}

TibKeyResult tibRecoverKey(const std::vector<std::uint8_t>& password,
                           const std::vector<std::uint8_t>& records,
                           std::uint64_t maxIterations,
                           std::vector<std::uint8_t>* key,
                           std::size_t* consumed) {
    if (!key || !consumed || &password == key || &records == key)
        return TibKeyResult::InvalidArgument;
    key->clear();
    *consumed = 0;
    if (password.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return TibKeyResult::InvalidArgument;

    const std::uint8_t* verification = nullptr;
    const std::uint8_t* comparison = nullptr;
    std::size_t pos = 0;
    bool terminated = false;
    while (pos < records.size()) {
        const std::uint8_t type = records[pos];
        if (type == 0xff) { ++pos; terminated = true; break; }
        if (records.size() - pos < 2) return TibKeyResult::Truncated;
        const std::size_t length = 6 + 4 * std::size_t(records[pos + 1]);
        if (length > records.size() - pos) return TibKeyResult::Truncated;
        const std::uint8_t* record = records.data() + pos;
        if (type == 4 && record[1] == 4 && !verification) {
            verification = record + 2;
            comparison = verification + 16;
        }
        pos += length;
    }
    if (!terminated) return TibKeyResult::Truncated;
    if (!verification) return TibKeyResult::InvalidRecord;

    QByteArray passwordBytes;
    if (!password.empty())
        passwordBytes = QByteArray(reinterpret_cast<const char*>(password.data()), static_cast<int>(password.size()));
    bool unsupported = false, limited = false, attempted = false, candidateFound = false;
    std::uint64_t remaining = maxIterations;
    const std::size_t end = pos;
    pos = 0;
    while (pos < end && records[pos] != 0xff) {
    const std::uint8_t* record = records.data() + pos;
    const std::size_t length = 6 + 4 * std::size_t(record[1]);
    pos += length;
    if (record[0] != 2 && record[0] != 8) continue;
    candidateFound = true;
    const bool legacy = record[0] == 2;
    if ((legacy && record[1] != 7 && record[1] != 11 && record[1] != 15) ||
        (!legacy && record[1] != 8 && record[1] != 12 && record[1] != 16)) {
        unsupported = true;
        continue;
    }
    const std::size_t keySize = legacy ? 2 * (std::size_t(record[1]) + 1)
                                       : 2 * std::size_t(record[1]);
    const std::uint8_t* salt = record + 2;
    const std::uint8_t* wrapped = salt + keySize;
    QByteArray derived;
    if (legacy) {
        const std::vector<std::uint8_t> saltBytes(salt, salt + keySize);
        std::array<std::uint8_t, 32> legacyKey = {};
        if (!tibLegacyKdf(password, saltBytes, &legacyKey)) {
            passwordBytes.fill(0);
            return TibKeyResult::InvalidRecord;
        }
        derived = QByteArray(reinterpret_cast<const char*>(legacyKey.data()), int(legacyKey.size()));
        std::fill(legacyKey.begin(), legacyKey.end(), 0);
        attempted = true;
    } else {
        const std::uint8_t* params = wrapped + keySize;
        if (params[1] || params[2] || params[3] || params[0] < 1 || params[0] > 24) {
            unsupported = true;
            continue;
        }
        const std::uint32_t iterations = std::uint32_t(1) << params[0];
        if (iterations > remaining) { limited = true; continue; }
        remaining -= iterations;
        attempted = true;
        QByteArray first(reinterpret_cast<const char*>(salt), static_cast<int>(keySize));
        first.append("\0\0\0\1", 4);
        QByteArray u = QMessageAuthenticationCode::hash(first, passwordBytes, QCryptographicHash::Sha256);
        if (u.size() != 32) { passwordBytes.fill(0); return TibKeyResult::InvalidRecord; }
        derived = u;
        for (std::uint32_t i = 1; i < iterations; ++i) {
            u = QMessageAuthenticationCode::hash(u, passwordBytes, QCryptographicHash::Sha256);
            if (u.size() != 32) {
                passwordBytes.fill(0);
                derived.fill(0);
                return TibKeyResult::InvalidRecord;
            }
            for (int j = 0; j < derived.size(); ++j) derived[j] = char(derived[j] ^ u[j]);
        }
        u.fill(0);
    }
    if (derived.size() != 32) { passwordBytes.fill(0); return TibKeyResult::InvalidRecord; }

    compression::initializeAesTables();
    alignas(16) UInt32 aes[AES_NUM_IVMRK_WORDS] = {};
    alignas(16) std::array<std::uint8_t, 32> clear = {};
    Aes_SetKey_Dec(aes + 4, reinterpret_cast<const Byte*>(derived.constData()), unsigned(keySize));
    if (keySize == 24) {
        alignas(16) std::array<std::uint8_t, 16> block;
        const std::uint8_t zeroIv[16] = {};
        std::copy(wrapped, wrapped + 16, block.begin());
        AesCbc_Init(aes, zeroIv);
        AesCbc_Decode(aes, block.data(), 1);
        for (std::size_t i = 0; i < 8; ++i) clear[16 + i] = block[i] ^ wrapped[16 + i];
        std::copy(block.begin() + 8, block.end(), clear.begin() + 8);
        std::copy(wrapped + 16, wrapped + 24, clear.begin());
        AesCbc_Init(aes, zeroIv);
        AesCbc_Decode(aes, clear.data(), 1);
        for (std::size_t i = 0; i < 16; ++i) clear[i] ^= salt[keySize - 16 + i];
        std::fill(block.begin(), block.end(), 0);
    } else {
        std::copy(wrapped, wrapped + keySize, clear.begin());
        AesCbc_Init(aes, salt + keySize - 16);
        AesCbc_Decode(aes, clear.data(), keySize / 16);
    }
    derived.fill(0);

    alignas(16) std::array<std::uint8_t, 16> check;
    std::copy(verification, verification + check.size(), check.begin());
    const std::uint8_t zeroIv[16] = {};
    Aes_SetKey_Enc(aes + 4, clear.data(), unsigned(keySize));
    AesCbc_Init(aes, zeroIv);
    AesCbc_Encode(aes, check.data(), 1);
    std::memset(aes, 0, sizeof(aes));
    const std::vector<std::uint8_t> checkBytes(check.begin(), check.end());
    const std::vector<std::uint8_t> verificationBytes(verification, verification + 16);
    std::array<std::uint8_t, 32> digest = {};
    if (!tibLegacyKdf(checkBytes, verificationBytes, &digest)) {
        std::fill(clear.begin(), clear.end(), 0);
        passwordBytes.fill(0);
        return TibKeyResult::InvalidRecord;
    }
    unsigned difference = 0;
    for (std::size_t i = 0; i < 4; ++i) difference |= digest[i] ^ comparison[i];
    std::fill(digest.begin(), digest.end(), 0);
    if (difference) {
        std::fill(clear.begin(), clear.end(), 0);
        continue;
    }
    key->assign(clear.begin(), clear.begin() + keySize);
    std::fill(clear.begin(), clear.end(), 0);
    passwordBytes.fill(0);
    *consumed = end;
    return TibKeyResult::Success;
    }
    passwordBytes.fill(0);
    if (!candidateFound) return TibKeyResult::InvalidRecord;
    if (unsupported) return TibKeyResult::Unsupported;
    if (limited) return TibKeyResult::IterationLimit;
    return attempted ? TibKeyResult::PasswordMismatch : TibKeyResult::InvalidRecord;
}

} // namespace fs
} // namespace peare
