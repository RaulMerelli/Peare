#pragma once

#include <QByteArray>
#include <QString>
#include <array>

namespace peare {

bool aes128CbcDecryptNoPadding(const QByteArray& ciphertext,
                               const std::array<unsigned char, 16>& key,
                               QByteArray* plaintext,
                               QString* error = nullptr);

} // namespace peare
