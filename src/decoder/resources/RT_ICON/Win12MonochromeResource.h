#pragma once

#include "../RT_BITMAP/RT_BITMAP.h"

namespace peare {
namespace resources {

/// Decode Windows 1.x and 2.x monochrome icon and cursor resources.
/// The payload contains aligned AND and XOR planes; trailing alignment padding is valid.
class Win12MonochromeResource
{
public:
    static constexpr quint8 FigureCursor = 1;
    static constexpr quint8 FigureBitmap = 2;
    static constexpr quint8 FigureIcon = 3;

    static bool LooksLike(const QByteArray& data);
    static bool TryDecode(const QByteArray& data, quint8 expectedFigure, Img& image);

private:
    static constexpr int HeaderSize = 14;
    static constexpr int MaximumDimension = 4096;

    static bool TryLocateHeader(const QByteArray& data, quint8 expectedFigure, int& headerOffset);
    static bool IsValidHeaderAt(const QByteArray& data, int offset, quint8 expectedFigure);
};

} // namespace resources
} // namespace peare
