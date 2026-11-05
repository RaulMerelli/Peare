#pragma once

#include "Type1Font.h"

namespace peare {
namespace font {

bool hasType3Header(const QByteArray& data);
bool parseType3Font(const QByteArray& data, Type1Font* font, QString* error = nullptr);

} // namespace font
} // namespace peare
