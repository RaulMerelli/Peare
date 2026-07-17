using System;
using System.Drawing;
using System.Drawing.Imaging;

namespace PeareModule
{
    /// <summary>
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
    /// pixel.  Padding following the two planes is allowed because NE resource
    /// lengths are stored in alignment units.
    /// </summary>
    internal static class Win12MonochromeResource
    {
        internal const byte FigureCursor = 1;
        internal const byte FigureBitmap = 2;
        internal const byte FigureIcon = 3;

        private const int HeaderSize = 14;
        private const int MaximumDimension = 4096;

        internal static bool LooksLike(byte[] data)
        {
            int headerOffset;
            return TryLocateHeader(data, 0, out headerOffset);
        }

        internal static bool TryDecode(byte[] data, byte expectedFigure, out Img image)
        {
            image = null;

            int headerOffset;
            if (!TryLocateHeader(data, expectedFigure, out headerOffset))
                return false;

            byte figure = data[headerOffset];
            ushort width = BitConverter.ToUInt16(data, headerOffset + 6);
            ushort height = BitConverter.ToUInt16(data, headerOffset + 8);
            ushort bytesPerLine = BitConverter.ToUInt16(data, headerOffset + 10);

            int planeSize;
            try
            {
                planeSize = checked((int)bytesPerLine * (int)height);
            }
            catch (OverflowException)
            {
                return false;
            }

            int andOffset = headerOffset + HeaderSize;
            int xorOffset = andOffset + planeSize;

            Bitmap bitmap;
            try
            {
                bitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            }
            catch (ArgumentException)
            {
                return false;
            }

            try
            {
                for (int y = 0; y < height; y++)
                {
                    int rowOffset = y * bytesPerLine;
                    for (int x = 0; x < width; x++)
                    {
                        int byteIndex = rowOffset + (x >> 3);
                        int bitIndex = 7 - (x & 7);

                        bool andBit = ((data[andOffset + byteIndex] >> bitIndex) & 1) != 0;
                        bool xorBit = ((data[xorOffset + byteIndex] >> bitIndex) & 1) != 0;

                        Color pixel;
                        if (!andBit)
                        {
                            // AND=0: output does not depend on the destination.
                            pixel = xorBit ? Color.White : Color.Black;
                        }
                        else if (!xorBit)
                        {
                            // AND=1, XOR=0: preserve destination => transparent.
                            pixel = Color.Transparent;
                        }
                        else
                        {
                            // AND=1, XOR=1: invert destination.  ARGB has no
                            // destination-dependent pixel, therefore use a visible
                            // neutral preview value without changing Img's API.
                            pixel = Color.FromArgb(255, 128, 128, 128);
                        }

                        bitmap.SetPixel(x, y, pixel);
                    }
                }
            }
            catch
            {
                bitmap.Dispose();
                throw;
            }

            image = new Img
            {
                // This describes the returned Bitmap, not the source planes.
                BitCount = Image.GetPixelFormatSize(bitmap.PixelFormat),
                Size = new Size(width, height),
                Bitmap = bitmap
            };

            Console.WriteLine(
                "Decoded Windows 1.x/2.x {0}: {1}x{2}, stride={3}, headerOffset={4}.",
                figure == FigureCursor ? "cursor" : "icon",
                width,
                height,
                bytesPerLine,
                headerOffset);

            return true;
        }

        private static bool TryLocateHeader(
            byte[] data,
            byte expectedFigure,
            out int headerOffset)
        {
            headerOffset = -1;
            if (data == null)
                return false;

            // Normal Win1/Win2 resource payload.
            if (IsValidHeaderAt(data, 0, expectedFigure))
            {
                headerOffset = 0;
                return true;
            }

            // Be tolerant of cursor payloads that have already acquired the
            // later four-byte hotspot prefix before the historical header.
            // This is not the canonical Win1/Win2 layout, but accepting it makes
            // extraction paths and standalone resource dumps interoperable.
            if (IsValidHeaderAt(data, 4, expectedFigure))
            {
                headerOffset = 4;
                return true;
            }

            return false;
        }

        private static bool IsValidHeaderAt(
            byte[] data,
            int offset,
            byte expectedFigure)
        {
            if (offset < 0 || data.Length - offset < HeaderSize)
                return false;

            byte figure = data[offset];
            byte independent = data[offset + 1];

            if (figure != FigureCursor && figure != FigureIcon)
                return false;
            if (expectedFigure != 0 && figure != expectedFigure)
                return false;
            if (independent != 0 && independent != 1)
                return false;

            ushort width = BitConverter.ToUInt16(data, offset + 6);
            ushort height = BitConverter.ToUInt16(data, offset + 8);
            ushort bytesPerLine = BitConverter.ToUInt16(data, offset + 10);
            ushort colorPlanes = BitConverter.ToUInt16(data, offset + 12);

            if (width == 0 || height == 0 ||
                width > MaximumDimension || height > MaximumDimension ||
                bytesPerLine == 0 || colorPlanes != 0)
            {
                return false;
            }

            int minimumStride = (width + 7) / 8;
            if (bytesPerLine < minimumStride || (bytesPerLine & 1) != 0)
                return false;

            long planeSize = (long)bytesPerLine * height;
            long requiredSize = offset + HeaderSize + planeSize * 2L;
            return requiredSize <= data.LongLength;
        }
    }
}
