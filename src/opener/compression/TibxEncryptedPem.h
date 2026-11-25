#pragma once

#include <cstdint>
#include <vector>

namespace peare {
namespace fs {

enum class TibxEncryptedPemResult {
    Success,
    PassphraseRequired,
    InvalidArgument,
    InvalidEncoding,
    Unsupported,
    IncorrectPassphraseOrCorrupt,
    WorkLimitExceeded
};

// PBES2/PBKDF2 with HMAC-SHA and AES-CBC; CBC does not authenticate plaintext.
// An empty passphrase differs from an absent one. The caller validates and erases DER.
// PBKDF2 is limited to 1,000,000 total HMAC evaluations across all blocks.
TibxEncryptedPemResult tibxDecryptEncryptedPem(
    const std::vector<std::uint8_t>& pem, bool passphrasePresent,
    const std::vector<std::uint8_t>& passphrase, std::vector<std::uint8_t>* der);

} // namespace fs
} // namespace peare
