#pragma once

#include <QByteArray>
#include <QString>

#include "BitmapFont.h"

namespace peare {
namespace font {

bool hasDwfHeader(const QByteArray& data);
bool parseDwf(const QByteArray& data, BitmapFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
