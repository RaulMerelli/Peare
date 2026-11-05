#pragma once

#include "BitmapFont.h"

namespace peare {
namespace font {

bool hasSunNewsFontHeader(const QByteArray& data);
bool parseSunNewsFont(const QByteArray& data, BitmapFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
