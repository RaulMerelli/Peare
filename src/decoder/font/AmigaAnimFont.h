#pragma once

#include "BitmapFont.h"

namespace peare {
namespace font {

bool hasAmigaAnimFontHeader(const QByteArray& data);
bool parseAmigaAnimFont(const QByteArray& data, BitmapFont* font,
                        QString* error = nullptr);

} // namespace font
} // namespace peare
