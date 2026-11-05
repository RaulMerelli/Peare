#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decryptRar4Data(const QString& password, const std::uint8_t* salt,
                     const std::uint8_t* input, std::size_t inputSize,
                     std::vector<std::uint8_t>* output);

} // namespace compression
} // namespace peare
