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

            // In FOCA outline resources usLastChar is used as the cell count.
            // For the common first-character value 1 this equals the normal
            // inclusive range calculation, but keeping it separate also handles
            // outline resources whose first code is not 1.
            int outlineCharacterCount = rawLastCharacter;
            if (outlineCharacterCount <= 0 || outlineCharacterCount > MaximumCells ||
                (long)cellArrayOffset + (long)outlineCharacterCount * cellSize > resData.Length)
                outlineCharacterCount = characterCount;

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

            int definitionRecordSize = ReadPositiveInt32(
                resData, definitionOffset + 4, resData.Length - definitionOffset);
            return DecodeOutlineFont(resData, metricsOffset, definitionOffset,
                definitionRecordSize, cellArrayOffset, cellSize, outlineCharacterCount,
                rawFirstCharacter, rawDefaultCharacter, rawBreakCharacter,
                codePage, rawCellHeight, emUnits, ascentUnits, descentUnits,
                nominalPointTenths, definitionFlags,
                defaultCellWidth, defaultCellIncrement,
                defaultASpace, defaultBSpace, defaultCSpace);
        }

        private static bool LooksLikeBitmapFont(byte[] data, int cellArrayOffset, int cellSize, int characterCount, int cellHeight, int defaultCellWidth, int defaultCellIncrement, int defaultBSpace)
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
                int width = BitConverter.ToUInt16(data, entry + ((cellSize == 6) ? 4 : 6));

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

        private static DecodedFont DecodeOutlineFont(
            byte[] data, int metricsOffset, int definitionOffset, int definitionRecordSize,
            int cellArrayOffset, int cellSize, int characterCount,
            int firstCharacter, int defaultCharacterOffset, int breakCharacterOffset, int codePage,
            int cellHeightUnits, int emUnits, int ascentUnits, int descentUnits,
            int nominalPointTenths, int definitionFlags,
            int defaultCellWidth, int defaultCellIncrement,
            int defaultASpace, int defaultBSpace, int defaultCSpace)
        {
            if (cellSize != 6 && cellSize != 10)
                throw new InvalidOperationException("Unsupported OS/2 outline cell size: " + cellSize.ToString());

            int baseHeight = nominalPointTenths > 0
                ? Math.Max(12, Math.Min(32, (nominalPointTenths + 5) / 10 + 4))
                : 16;
            int designHeight = cellHeightUnits > 0
                ? cellHeightUnits
                : Math.Max(1, ascentUnits + descentUnits);
            if (designHeight <= 0)
                designHeight = emUnits > 0 ? emUnits : 1000;
            double unitScale = (double)baseHeight / designHeight;

            int lastCharacter = firstCharacter + characterCount - 1;
            DecodedFont font = CreateFontHeader(data, metricsOffset, codePage,
                firstCharacter, lastCharacter,
                firstCharacter + defaultCharacterOffset,
                firstCharacter + breakCharacterOffset);
            font.FormatName = "OS/2 GPI outline";
            font.PixelHeight = baseHeight;
            font.Ascent = Math.Max(1, ScaleUnit(ascentUnits, unitScale));
            font.Descent = Math.Max(0, ScaleUnit(descentUnits, unitScale));
            font.LineHeight = Math.Max(baseHeight, font.Ascent + font.Descent);
            font.IsVector = true;
            font.DeclaredGlyphCount = characterCount;

            int definitionEnd = definitionOffset + definitionRecordSize;
            if (definitionEnd < definitionOffset || definitionEnd > data.Length)
                definitionEnd = data.Length;

            int[] glyphOffsets = new int[characterCount];
            int[] advances = new int[characterCount];
            List<int> sortedOffsets = new List<int>();

            for (int i = 0; i < characterCount; i++)
            {
                int cellOffset = cellArrayOffset + i * cellSize;
                uint rawOffset = BitConverter.ToUInt32(data, cellOffset);
                int glyphOffset = rawOffset <= int.MaxValue ? (int)rawOffset : 0;
                glyphOffsets[i] = glyphOffset;
                if (glyphOffset > 0 && glyphOffset < definitionEnd && !sortedOffsets.Contains(glyphOffset))
                    sortedOffsets.Add(glyphOffset);

                int aSpace = 0;
                int bSpace;
                int cSpace = 0;
                if (cellSize == 6)
                {
                    bSpace = BitConverter.ToUInt16(data, cellOffset + 4);
                    if (bSpace <= 0 || bSpace == 0x8000)
                        bSpace = defaultCellWidth > 0 ? defaultCellWidth : defaultCellIncrement;
                }
                else
                {
                    aSpace = BitConverter.ToInt16(data, cellOffset + 4);
                    bSpace = BitConverter.ToUInt16(data, cellOffset + 6);
                    cSpace = BitConverter.ToInt16(data, cellOffset + 8);
                    if (aSpace == short.MinValue)
                        aSpace = defaultASpace == short.MinValue ? 0 : defaultASpace;
                    if (bSpace == 0 || bSpace == 0x8000)
                    {
                        bSpace = defaultBSpace;
                        if (bSpace == 0 || bSpace == 0x8000)
                            bSpace = defaultCellWidth > 0 ? defaultCellWidth : defaultCellIncrement;
                    }
                    if (cSpace == short.MinValue)
                        cSpace = defaultCSpace == short.MinValue ? 0 : defaultCSpace;
                }

                int advanceUnits = aSpace + bSpace + cSpace;
                if (advanceUnits <= 0)
                    advanceUnits = defaultCellIncrement > 0 ? defaultCellIncrement : Math.Max(1, bSpace);
                advances[i] = Math.Max(1, ScaleUnit(advanceUnits, unitScale));
            }
            sortedOffsets.Sort();

            int failedGlyphs = 0;
            for (int i = 0; i < characterCount; i++)
            {
                int glyphStart = glyphOffsets[i];
                int glyphEnd = definitionEnd;
                if (glyphStart > 0)
                {
                    for (int offsetIndex = 0; offsetIndex < sortedOffsets.Count; offsetIndex++)
                    {
                        if (sortedOffsets[offsetIndex] > glyphStart)
                        {
                            glyphEnd = sortedOffsets[offsetIndex];
                            break;
                        }
                    }
                }

                List<FontOutlineContour> contours = new List<FontOutlineContour>();
                if (glyphStart > 0 && glyphStart < glyphEnd && glyphEnd <= data.Length)
                {
                    try
                    {
                        contours = OS2OutlineDecoder.DecodeGlyph(data, glyphStart, glyphEnd - glyphStart, unitScale, ascentUnits);
                    }
                    catch (InvalidOperationException)
                    {
                        failedGlyphs++;
                    }
                }

                font.Glyphs.Add(new FontGlyph
                {
                    CharacterCode = firstCharacter + i,
                    Width = advances[i],
                    Height = font.LineHeight,
                    AdvanceX = advances[i],
                    OffsetX = 0,
                    OffsetY = 0,
                    OutlineContours = contours
                });
            }

            if (failedGlyphs > 0)
            {
                font.PreviewMessage = failedGlyphs.ToString() +
                    " outline glyph(s) contained unsupported or malformed commands.";
            }
            else if ((definitionFlags & 0x0001) != 0)
            {
                font.PreviewMessage =
                    "OS/2 FOCA outline decoded with line and rational conic commands.";
            }
            return font;
        }

        private static DecodedFont DecodeBitmapFont(
            byte[] data, int metricsOffset, int cellArrayOffset, int cellSize, int firstCharacter, int lastCharacter, int defaultCharacter, int breakCharacter,
            int codePage, int cellHeight, int ascent, int descent, int defaultCellWidth, int defaultCellIncrement, int defaultASpace, int defaultBSpace, int defaultCSpace)
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

        private static DecodedFont CreateFontHeader(byte[] data, int metricsOffset, int codePage, int firstCharacter, int lastCharacter, int defaultCharacter, int breakCharacter)
        {
            DecodedFont font = new DecodedFont
            {
                FaceName = ReadFixedString(data, metricsOffset + 40, 32, codePage)
            };
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

                List<Bitmap> rendered = new List<Bitmap>();
                List<Point> offsets = new List<Point>();
                int minimumX = 0;
                int maximumX = 1;
                int rowHeight = Math.Max(1, font.LineHeight);

                try
                {
                    for (int i = 0; i < font.Glyphs.Count; i++)
                    {
                        FontGlyph glyph = font.Glyphs[i];
                        int offsetX;
                        int offsetY;
                        Bitmap bitmap = RT_FONT.RenderGlyph(glyph, 1, out offsetX, out offsetY);
                        rendered.Add(bitmap);
                        offsets.Add(new Point(offsetX, offsetY));
                        minimumX = Math.Min(minimumX, offsetX);
                        maximumX = Math.Max(maximumX, offsetX + bitmap.Width);
                        rowHeight = Math.Max(rowHeight, offsetY + bitmap.Height);
                    }

                    Bitmap strip = new Bitmap(
                        Math.Max(1, maximumX - minimumX),
                        Math.Max(1, rowHeight * font.Glyphs.Count),
                        PixelFormat.Format32bppArgb);
                    using (Graphics graphics = Graphics.FromImage(strip))
                    {
                        graphics.Clear(Color.Transparent);
                        for (int i = 0; i < rendered.Count; i++)
                        {
                            Point offset = offsets[i];
                            graphics.DrawImageUnscaled(
                                rendered[i],
                                offset.X - minimumX,
                                i * rowHeight + offset.Y);
                        }
                    }
                    return strip;
                }
                finally
                {
                    for (int i = 0; i < rendered.Count; i++)
                        rendered[i].Dispose();
                }
            }
        }

    }
}
