#pragma once

#include "../RT_BITMAP/RT_BITMAP.h"

namespace peare {
namespace resources {

/// Decoder for the monochrome cursor/icon resource format used by
/// Windows 1.x and Windows 2.x.
///
/// Layout:
///   BYTE  bFigure       (1 cursor, 3 icon)
///   BYTE  bIndependent  (0 device-dependent, 1 device-independent)
///   WORD  xHotspot
///   WORD  yHotspot
///   WORD  cx
///   WORD  cy
///   WORD  widthBytes
///   WORD  clr           (always zero)
///   BYTE  andMask[cy * widthBytes]
///   BYTE  xorMask[cy * widthBytes]
///
/// Rows are word-aligned and the most-significant bit is the leftmost
/// pixel. Padding following the two planes is allowed because NE resource
/// lengths are stored in alignment units.
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
