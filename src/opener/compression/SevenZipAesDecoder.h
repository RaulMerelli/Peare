#pragma once

#include <QByteArray>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decryptSevenZipAes(const QByteArray& properties, const QString& password,
                        const std::uint8_t* input, std::size_t inputSize,
                        std::size_t outputSize, std::vector<std::uint8_t>* output);

}  // namespace compression
}  // namespace peare
