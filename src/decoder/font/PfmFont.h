#pragma once

#include "AfmFont.h"

namespace peare {
namespace font {

bool hasPfmHeader(const QByteArray& data);
bool parsePfm(const QByteArray& data, AfmFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
