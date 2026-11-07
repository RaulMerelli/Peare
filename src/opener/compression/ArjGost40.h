#pragma once

#include <QByteArray>

#include <cstdint>
#include <vector>

namespace peare {
namespace compression {

bool decryptArjGost40(std::vector<std::uint8_t>* data, const QByteArray& password,
                      std::uint8_t modifier, std::uint32_t fileTime);
bool decryptArjGost256(std::vector<std::uint8_t>* data, const QByteArray& password,
                       std::uint8_t modifier, std::uint32_t fileTime,
                       bool longPasswordMode);

} // namespace compression
} // namespace peare
