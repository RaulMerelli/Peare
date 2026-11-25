#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace peare {
namespace fs {

enum class TibxRsaResult {
    Success,
    InvalidArgument,
    InvalidKey,
    Unsupported,
    DecryptionFailed,
    ResourceError
};

// Unencrypted two-prime RSA PEM, with fixed-base probable-prime validation.
// Base and exponent blinding require tibxSecureRandom; not constant-time.
// Secret wiping is best-effort and does not cover all copies or system storage.
class TibxRsaPrivateKey {
public:
    TibxRsaPrivateKey();
    ~TibxRsaPrivateKey();
    TibxRsaPrivateKey(const TibxRsaPrivateKey&) = delete;
    TibxRsaPrivateKey& operator=(const TibxRsaPrivateKey&) = delete;

    // Clears any previous key, including on failure. Parse and validate once.
    TibxRsaResult loadPem(const std::string& privateKeyPem);
    // Operations return InvalidKey when no key is loaded. Failures clear output.
    TibxRsaResult publicSpkiDer(std::vector<std::uint8_t>* spkiDer) const;
    // Ciphertext and plaintext must not be the same vector.
    TibxRsaResult decryptPkcs1v15(const std::vector<std::uint8_t>& ciphertext,
                                 std::vector<std::uint8_t>* plaintext) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

TibxRsaResult tibxRsaPublicSpkiDer(const std::string& privateKeyPem,
                                   std::vector<std::uint8_t>* spkiDer);
// Ciphertext and plaintext must not be the same vector. Failures clear plaintext.
// Ciphertexts 0 and 1 are rejected before key parsing.
TibxRsaResult tibxRsaDecryptPkcs1v15(const std::string& privateKeyPem,
                                   const std::vector<std::uint8_t>& ciphertext,
                                   std::vector<std::uint8_t>* plaintext);

} // namespace fs
} // namespace peare
