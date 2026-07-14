using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.Text;

namespace PeareModule
{
    public static class OS2_RT_FONT
    {
        private const int SignatureSize = 20;
        private const int DefaultMetricsSize = 168;
        private const int DefinitionSize = 28;
        private const int MaximumCells = 1024;

        public static DecodedFont Decode(byte[] resData)
        {
            if (resData == null || resData.Length < SignatureSize + DefaultMetricsSize + DefinitionSize)
                throw new InvalidOperationException("Invalid OS/2 font data: insufficient length.");

            int metricsOffset = SignatureSize;
            int metricsSize = ReadPositiveInt32(resData, metricsOffset + 4, DefaultMetricsSize);
            if (metricsSize < 136 || metricsOffset + metricsSize + DefinitionSize > resData.Length)
                metricsSize = DefaultMetricsSize;

            int definitionOffset = metricsOffset + metricsSize;
            // ulSize in FONTDEFINITIONHEADER describes the complete definition
            // record (header, cell table and glyph data), not the header length.
            int definitionSize = DefinitionSize;

            int cellSize = BitConverter.ToUInt16(resData, definitionOffset + 12);
            int defaultCellWidth = Math.Abs((int)BitConverter.ToInt16(resData, definitionOffset + 14));
            int rawCellHeight = Math.Abs((int)BitConverter.ToInt16(resData, definitionOffset + 16));
            int defaultCellIncrement = Math.Abs((int)BitConverter.ToInt16(resData, definitionOffset + 18));
            int defaultASpace = BitConverter.ToInt16(resData, definitionOffset + 20);
            int defaultBSpace = BitConverter.ToUInt16(resData, definitionOffset + 22);
            int defaultCSpace = BitConverter.ToInt16(resData, definitionOffset + 24);
            if (cellSize != 6 && cellSize != 10)
                throw new InvalidOperationException("Unsupported OS/2 font cell size: " + cellSize.ToString());
            if (rawCellHeight <= 0)
                throw new InvalidOperationException("Invalid OS/2 font cell height: " + rawCellHeight.ToString());

            int rawFirstCharacter = BitConverter.ToUInt16(resData, metricsOffset + 114);
            int rawLastCharacter = BitConverter.ToUInt16(resData, metricsOffset + 116);
            int rawDefaultCharacter = BitConverter.ToUInt16(resData, metricsOffset + 118);
            int rawBreakCharacter = BitConverter.ToUInt16(resData, metricsOffset + 120);
            int codePage = BitConverter.ToUInt16(resData, metricsOffset + 74);
            int ascentUnits = Math.Abs((int)BitConverter.ToInt16(resData, metricsOffset + 80));
            int descentUnits = Math.Abs((int)BitConverter.ToInt16(resData, metricsOffset + 82));
            int emUnits = Math.Abs((int)BitConverter.ToInt16(resData, metricsOffset + 76));
            int nominalPointTenths = Math.Abs((int)BitConverter.ToInt16(resData, metricsOffset + 122));
            int definitionFlags = BitConverter.ToUInt16(resData, metricsOffset + 130);

            int characterCount = rawLastCharacter >= rawFirstCharacter
                ? rawLastCharacter - rawFirstCharacter + 1
                : 0;
            if (characterCount <= 0 || characterCount > MaximumCells)
                throw new InvalidOperationException("Invalid OS/2 font character range.");

            int cellArrayOffset = definitionOffset + definitionSize;
            if ((long)cellArrayOffset + (long)characterCount * cellSize > resData.Length)
                throw new InvalidOperationException("Invalid OS/2 font data: truncated cell table.");

            // fsDefn is not a reliable raster/outline discriminator across old
            // OS/2 font producers. Prefer the actual cell table: bitmap entries
            // must point to enough bytes for width x height monochrome data.
            bool bitmapDataIsPlausible = LooksLikeBitmapFont(
                resData, cellArrayOffset, cellSize, characterCount, rawCellHeight,
                defaultCellWidth, defaultCellIncrement, defaultBSpace);

            if (bitmapDataIsPlausible)
            {
                return DecodeBitmapFont(resData, metricsOffset, cellArrayOffset, cellSize,
                    rawFirstCharacter, rawLastCharacter, rawDefaultCharacter, rawBreakCharacter,
                    codePage, rawCellHeight, ascentUnits, descentUnits,
                    defaultCellWidth, defaultCellIncrement, defaultASpace, defaultBSpace, defaultCSpace);
            }

            return DecodeOutlineMetadata(resData, metricsOffset,
                rawFirstCharacter, rawLastCharacter, rawDefaultCharacter, rawBreakCharacter,
                codePage, rawCellHeight, emUnits, ascentUnits, descentUnits,
                nominalPointTenths, definitionFlags);
        }

        private static bool LooksLikeBitmapFont(
            byte[] data,
            int cellArrayOffset,
            int cellSize,
            int characterCount,
            int cellHeight,
            int defaultCellWidth,
            int defaultCellIncrement,
            int defaultBSpace)
        {
            if (cellHeight <= 0 || cellHeight > 4096)
                return false;

            int samples = Math.Min(characterCount, 32);
            int plausible = 0;
            int nonEmpty = 0;

            for (int i = 0; i < samples; i++)
            {
                int entry = cellArrayOffset + i * cellSize;
                if (entry < 0 || entry + cellSize > data.Length)
                    break;

                uint glyphOffset = BitConverter.ToUInt32(data, entry);
                int width = cellSize == 6
                    ? BitConverter.ToUInt16(data, entry + 4)
                    : BitConverter.ToUInt16(data, entry + 6);

                if (width == 0 || width == 0x8000)
                {
                    width = defaultBSpace;
                    if (width == 0 || width == 0x8000)
                        width = defaultCellWidth > 0 ? defaultCellWidth : defaultCellIncrement;
                }

                if (width <= 0)
                    continue;

                nonEmpty++;
                long bytesPerColumn = (width + 7L) / 8L;
                long requiredEnd = (long)glyphOffset + bytesPerColumn * cellHeight;
                if (glyphOffset < data.Length && requiredEnd <= data.Length)
                    plausible++;
            }

            if (nonEmpty == 0)
                return false;

            return plausible * 4 >= nonEmpty * 3;
        }

        private static DecodedFont DecodeOutlineMetadata(
            byte[] data,
            int metricsOffset,
            int firstCharacter,
            int lastCharacter,
            int defaultCharacter,
            int breakCharacter,
            int codePage,
            int cellHeightUnits,
            int emUnits,
            int ascentUnits,
            int descentUnits,
            int nominalPointTenths,
            int definitionFlags)
        {
            int baseHeight = nominalPointTenths > 0
                ? Math.Max(12, Math.Min(32, (nominalPointTenths + 5) / 10 + 4))
                : 16;
            double unitScale = (double)baseHeight / Math.Max(1, cellHeightUnits);

            DecodedFont font = CreateFontHeader(data, metricsOffset, codePage,
                firstCharacter + 1, lastCharacter + 1,
                defaultCharacter + 1, breakCharacter + 1);
            font.FormatName = "OS/2 FNT outline";
            font.PixelHeight = baseHeight;
            font.Ascent = Math.Max(1, ScaleUnit(ascentUnits, unitScale));
            font.Descent = Math.Max(0, ScaleUnit(descentUnits, unitScale));
            font.LineHeight = Math.Max(baseHeight, font.Ascent + font.Descent);
            font.IsVector = true;
            font.DeclaredGlyphCount = Math.Max(0, lastCharacter - firstCharacter + 1);
            font.PreviewMessage =
                "OS/2 outline font recognized. Glyph rendering is disabled because " +
                "the FOCA command stream is not decoded reliably yet.";
            return font;
        }

        private static DecodedFont DecodeBitmapFont(
            byte[] data, int metricsOffset, int cellArrayOffset, int cellSize,
            int firstCharacter, int lastCharacter, int defaultCharacter, int breakCharacter,
            int codePage, int cellHeight, int ascent, int descent,
            int defaultCellWidth, int defaultCellIncrement,
            int defaultASpace, int defaultBSpace, int defaultCSpace)
        {
            if (cellHeight > 1024)
                throw new InvalidOperationException("Invalid OS/2 bitmap font cell height: " + cellHeight.ToString());

            int characterCount = lastCharacter - firstCharacter + 1;
            DecodedFont font = CreateFontHeader(data, metricsOffset, codePage, firstCharacter, lastCharacter, defaultCharacter, breakCharacter);
            font.FormatName = cellSize == 10 ? "OS/2 FNT type 3" : "OS/2 FNT type 1/2";
            font.PixelHeight = cellHeight;
            font.Ascent = ascent > 0 ? ascent : cellHeight;
            font.Descent = descent;
            font.LineHeight = Math.Max(cellHeight, font.Ascent + font.Descent);
            font.IsVector = false;

            for (int i = 0; i < characterCount; i++)
            {
                int cellOffset = cellArrayOffset + i * cellSize;
                uint glyphDataOffset = BitConverter.ToUInt32(data, cellOffset);
                int aSpace = 0;
                int bSpace;
                int cSpace = 0;

                if (cellSize == 6)
                {
                    bSpace = BitConverter.ToUInt16(data, cellOffset + 4);
                    if (bSpace <= 0)
                        bSpace = defaultCellWidth > 0 ? defaultCellWidth : defaultCellIncrement;
                }
                else
                {
                    aSpace = BitConverter.ToInt16(data, cellOffset + 4);
                    bSpace = BitConverter.ToUInt16(data, cellOffset + 6);
                    cSpace = BitConverter.ToInt16(data, cellOffset + 8);
                    if (aSpace == short.MinValue)
                        aSpace = defaultASpace == short.MinValue ? 0 : defaultASpace;
                    if (bSpace == 0x8000 || bSpace == 0)
                        bSpace = defaultBSpace == 0x8000 || defaultBSpace == 0 ? defaultCellWidth : defaultBSpace;
                    if (cSpace == short.MinValue)
                        cSpace = defaultCSpace == short.MinValue ? 0 : defaultCSpace;
                }

                if (bSpace <= 0)
                    bSpace = defaultCellWidth > 0 ? defaultCellWidth : 1;
                if (bSpace > 4096)
                    throw new InvalidOperationException("Invalid OS/2 glyph width " + bSpace.ToString() + ".");

                Bitmap glyphBitmap = DecodeGlyphBitmap(data, glyphDataOffset, bSpace, cellHeight);
                int advance = aSpace + bSpace + cSpace;
                if (advance <= 0)
                    advance = defaultCellIncrement > 0 ? defaultCellIncrement : bSpace;

                font.Glyphs.Add(new FontGlyph
                {
                    CharacterCode = font.FirstCharacter + i,
                    Width = bSpace,
                    Height = cellHeight,
                    AdvanceX = advance,
                    OffsetX = aSpace,
                    OffsetY = 0,
                    Bitmap = glyphBitmap
                });
            }
            return font;
        }

        private static DecodedFont CreateFontHeader(byte[] data, int metricsOffset, int codePage,
            int firstCharacter, int lastCharacter, int defaultCharacter, int breakCharacter)
        {
            DecodedFont font = new DecodedFont();
            font.FaceName = ReadFixedString(data, metricsOffset + 40, 32, codePage);
            if (string.IsNullOrEmpty(font.FaceName))
                font.FaceName = "OS/2 font";
            font.FirstCharacter = firstCharacter;
            font.LastCharacter = lastCharacter;
            font.DefaultCharacter = defaultCharacter;
            font.BreakCharacter = breakCharacter;
            font.CodePage = codePage;
            return font;
        }

        private static int ScaleUnit(int value, double scale)
        {
            return (int)Math.Round(value * scale, MidpointRounding.AwayFromZero);
        }

        private static int ReadPositiveInt32(byte[] data, int offset, int fallback)
        {
            if (offset < 0 || offset + 4 > data.Length)
                return fallback;
            uint value = BitConverter.ToUInt32(data, offset);
            return value > 0 && value <= int.MaxValue ? (int)value : fallback;
        }

        private static Bitmap DecodeGlyphBitmap(byte[] data, uint glyphDataOffset, int width, int height)
        {
            Bitmap bitmap = new Bitmap(Math.Max(1, width), Math.Max(1, height), PixelFormat.Format32bppArgb);
            using (Graphics graphics = Graphics.FromImage(bitmap))
                graphics.Clear(Color.Transparent);

            int columns = (width + 7) / 8;
            long requiredEnd = (long)glyphDataOffset + (long)columns * height;
            if (glyphDataOffset >= data.Length || requiredEnd > data.Length)
                return bitmap;

            for (int row = 0; row < height; row++)
            {
                for (int column = 0; column < columns; column++)
                {
                    int dataPosition = (int)glyphDataOffset + column * height + row;
                    byte value = data[dataPosition];
                    int firstPixel = column * 8;
                    for (int bit = 0; bit < 8; bit++)
                    {
                        int x = firstPixel + bit;
                        if (x >= width)
                            break;
                        if ((value & (0x80 >> bit)) != 0)
                            bitmap.SetPixel(x, row, Color.Black);
                    }
                }
            }
            return bitmap;
        }

        private static string ReadFixedString(byte[] data, int offset, int length, int codePage)
        {
            if (offset < 0 || length <= 0 || offset + length > data.Length)
                return string.Empty;
            int actualLength = 0;
            while (actualLength < length && data[offset + actualLength] != 0)
                actualLength++;
            try
            {
                return Encoding.GetEncoding(codePage).GetString(data, offset, actualLength).Trim();
            }
            catch
            {
                return Encoding.ASCII.GetString(data, offset, actualLength).Trim();
            }
        }

        public static Bitmap Get(byte[] resData)
        {
            using (DecodedFont font = Decode(resData))
            {
                if (font.Glyphs.Count == 0)
                    return null;
                int width = 1;
                int height = Math.Max(1, font.LineHeight * font.Glyphs.Count);
                for (int i = 0; i < font.Glyphs.Count; i++)
                {
                    FontGlyph glyph = font.Glyphs[i];
                    if (glyph != null && glyph.Bitmap != null)
                        width = Math.Max(width, glyph.Bitmap.Width);
                }
                Bitmap strip = new Bitmap(width, height, PixelFormat.Format32bppArgb);
                using (Graphics graphics = Graphics.FromImage(strip))
                {
                    graphics.Clear(Color.Transparent);
                    for (int i = 0; i < font.Glyphs.Count; i++)
                    {
                        FontGlyph glyph = font.Glyphs[i];
                        if (glyph != null && glyph.Bitmap != null)
                            graphics.DrawImageUnscaled(glyph.Bitmap, Math.Max(0, glyph.OffsetX), i * font.LineHeight + Math.Max(0, glyph.OffsetY));
                    }
                }
                return strip;
            }
        }
    }
}
