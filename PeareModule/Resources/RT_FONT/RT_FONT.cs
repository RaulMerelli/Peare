using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Text;

namespace PeareModule
{
    public static class RT_FONT
    {
        public static DecodedFont Decode(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            bool moduleIsOs2 = properties != null &&
                ((properties.headerType == ModuleResources.HeaderType.LE && properties.versionType == ModuleResources.VersionType.OS2) ||
                 (properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                 properties.headerType == ModuleResources.HeaderType.LX);

            if (moduleIsOs2)
                return OS2_RT_FONT.Decode(resData);

            FONTINFO16 header = ModuleResources.Deserialize<FONTINFO16>(resData);
            int firstCharacter = header.dfFirstChar;
            int lastCharacter = header.dfLastChar;
            int characterCount = lastCharacter - firstCharacter + 1;
            if (characterCount <= 0 || characterCount > 512)
                throw new InvalidOperationException("Invalid Windows FNT character range.");

            bool isVectorFont = (header.dfType & 0x0001) != 0;
            int glyphHeight = isVectorFont ? Math.Max(1, header.dfPixHeight + header.dfAscent) : Math.Max(1, (int)header.dfPixHeight);

            DecodedFont font = new DecodedFont
            {
                FaceName = ReadNullTerminatedAnsi(resData, (int)header.dfFace)
            };
            if (string.IsNullOrEmpty(font.FaceName))
                font.FaceName = "Windows FNT";
            font.FormatName = "Windows FNT " + (header.dfVersion / 256).ToString() + "." + (header.dfVersion & 0xFF).ToString("D2");
            font.FirstCharacter = firstCharacter;
            font.LastCharacter = lastCharacter;
            font.DefaultCharacter = firstCharacter + header.dfDefaultChar;
            font.BreakCharacter = firstCharacter + header.dfBreakChar;
            font.PixelHeight = Math.Max(1, (int)header.dfPixHeight);
            font.Ascent = header.dfAscent;
            font.Descent = Math.Max(0, font.PixelHeight - font.Ascent);
            font.LineHeight = Math.Max(glyphHeight, font.PixelHeight + header.dfExternalLeading);
            font.CharacterSet = header.dfCharSet;
            font.IsVector = isVectorFont;

            int[] glyphWidths = ReadGlyphWidths(resData, header, characterCount, isVectorFont);
            if (isVectorFont)
            {
                DecodeWindowsVectorGlyphs(resData, header, font, glyphWidths, characterCount);
                return font;
            }

            Bitmap strip = Get(resData, properties);
            if (strip == null)
                throw new InvalidOperationException("The Windows FNT decoder did not produce an image.");

            try
            {
                for (int i = 0; i < characterCount; i++)
                {
                    int width = glyphWidths[i];
                    if (width < 0)
                        width = header.dfPixWidth > 0 ? header.dfPixWidth : header.dfAvgWidth;
                    if (width < 0)
                        width = 8;
                    int bitmapWidth = Math.Max(1, Math.Min(width, strip.Width));

                    int sourceY = i * glyphHeight;
                    if (sourceY >= strip.Height)
                        break;
                    int sourceHeight = Math.Min(glyphHeight, strip.Height - sourceY);
                    Bitmap glyphBitmap = CopyGlyph(strip, bitmapWidth, sourceY, sourceHeight);

                    font.Glyphs.Add(new FontGlyph
                    {
                        CharacterCode = firstCharacter + i,
                        Width = width,
                        Height = sourceHeight,
                        AdvanceX = width,
                        OffsetX = 0,
                        OffsetY = 0,
                        Bitmap = glyphBitmap
                    });
                }
            }
            finally
            {
                strip.Dispose();
            }

            return font;
        }

        private static void DecodeWindowsVectorGlyphs(byte[] resData, FONTINFO16 header, DecodedFont font, int[] glyphWidths, int characterCount)
        {
            int[] glyphOffsets = ReadVectorGlyphOffsets(resData, header, characterCount);
            bool coords2Byte = header.dfPixHeight > 128 || header.dfMaxWidth > 128;
            int yOffset = header.dfPixHeight - header.dfAscent;
            int dataLength = header.dfBitsOffset < resData.Length ? resData.Length - (int)header.dfBitsOffset : 0;

            for (int i = 0; i < characterCount; i++)
            {
                int advance = glyphWidths[i];
                if (advance <= 0)
                    advance = header.dfPixWidth > 0 ? header.dfPixWidth : header.dfAvgWidth;
                if (advance <= 0)
                    advance = 8;

                int currentOffset = glyphOffsets[i];
                int nextOffset = i + 1 < characterCount ? glyphOffsets[i + 1] : dataLength;
                int strokeDataStart = (int)header.dfBitsOffset + currentOffset;
                int strokeLength = nextOffset - currentOffset;

                List<FontVectorSegment> segments = ParseVectorSegments(resData, strokeDataStart, strokeLength, coords2Byte, yOffset);

                FontGlyph glyph = new FontGlyph
                {
                    CharacterCode = font.FirstCharacter + i,
                    AdvanceX = advance,
                    VectorSegments = segments
                };

                int offsetX;
                int offsetY;
                glyph.Bitmap = RenderGlyph(glyph, 1, out offsetX, out offsetY);
                glyph.OffsetX = offsetX;
                glyph.OffsetY = offsetY;
                glyph.Width = glyph.Bitmap.Width;
                glyph.Height = glyph.Bitmap.Height;
                font.Glyphs.Add(glyph);
            }
        }

        private static int[] ReadVectorGlyphOffsets(byte[] resData, FONTINFO16 header, int characterCount)
        {
            int[] offsets = new int[characterCount];
            int version = header.dfVersion;
            int headerSize = version < 0x0200 ? 117 : (version < 0x0300 ? 118 : 148);

            for (int i = 0; i < characterCount; i++)
            {
                if (version >= 0x0300)
                {
                    int entryOffset = headerSize + i * 6;
                    if (entryOffset + 6 > resData.Length)
                        break;
                    uint value = BitConverter.ToUInt32(resData, entryOffset + 2);
                    offsets[i] = value <= int.MaxValue ? (int)value : 0;
                }
                else
                {
                    int entryOffset = headerSize + i * 4;
                    if (entryOffset + 4 > resData.Length)
                        break;
                    offsets[i] = BitConverter.ToUInt16(resData, entryOffset);
                }
            }

            return offsets;
        }

        private static List<FontVectorSegment> ParseVectorSegments(byte[] data, int start, int length, bool coords2Byte, int yOffset)
        {
            List<FontVectorSegment> segments = new List<FontVectorSegment>();
            if (data == null || length <= 0 || start < 0 || start >= data.Length || start + length > data.Length)
                return segments;

            int x = 0;
            int y = 0;
            int position = 0;
            bool penDown = false;
            int lastX = 0;
            int lastY = 0;

            while (position < length)
            {
                if (coords2Byte)
                {
                    if (position + 2 > length)
                        break;
                    short marker = BitConverter.ToInt16(data, start + position);
                    if (marker == short.MinValue)
                    {
                        penDown = false;
                        position += 2;
                        continue;
                    }
                }
                else
                {
                    sbyte marker = (sbyte)data[start + position];
                    if (marker == sbyte.MinValue)
                    {
                        penDown = false;
                        position += 1;
                        continue;
                    }
                }

                int dx;
                int dy;
                if (coords2Byte)
                {
                    if (position + 4 > length)
                        break;
                    dx = BitConverter.ToInt16(data, start + position);
                    dy = BitConverter.ToInt16(data, start + position + 2);
                    position += 4;
                }
                else
                {
                    if (position + 2 > length)
                        break;
                    dx = (sbyte)data[start + position];
                    dy = (sbyte)data[start + position + 1];
                    position += 2;
                }

                x += dx;
                y += dy;
                int currentY = y + yOffset;

                if (penDown)
                {
                    segments.Add(new FontVectorSegment
                    {
                        X1 = lastX,
                        Y1 = lastY,
                        X2 = x,
                        Y2 = currentY
                    });
                }

                lastX = x;
                lastY = currentY;
                penDown = true;
            }

            return segments;
        }

        public static Bitmap RenderGlyph(FontGlyph glyph, int scale, out int offsetX, out int offsetY)
        {
            if (glyph == null)
                throw new ArgumentNullException("glyph");
            if (scale < 1)
                scale = 1;

            if (glyph.HasFilledOutline)
                return RenderFilledOutlineGlyph(glyph, scale, out offsetX, out offsetY);
            if (glyph.HasVectorOutline)
                return RenderVectorGlyph(glyph, scale, out offsetX, out offsetY);

            offsetX = glyph.OffsetX * scale;
            offsetY = glyph.OffsetY * scale;
            if (glyph.Bitmap == null)
                return new Bitmap(Math.Max(1, glyph.AdvanceX * scale), Math.Max(1, glyph.Height * scale), System.Drawing.Imaging.PixelFormat.Format32bppArgb);

            Bitmap result = new Bitmap(
                Math.Max(1, glyph.Bitmap.Width * scale),
                Math.Max(1, glyph.Bitmap.Height * scale),
                System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            using (Graphics graphics = Graphics.FromImage(result))
            {
                graphics.Clear(Color.Transparent);
                graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
                graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.Half;
                graphics.DrawImage(glyph.Bitmap,
                    new Rectangle(0, 0, result.Width, result.Height),
                    0, 0, glyph.Bitmap.Width, glyph.Bitmap.Height,
                    GraphicsUnit.Pixel);
            }
            return result;
        }

        private static Bitmap RenderFilledOutlineGlyph(FontGlyph glyph, int scale, out int offsetX, out int offsetY)
        {
            List<FontOutlineContour> contours = glyph.OutlineContours;
            if (contours == null || contours.Count == 0)
            {
                offsetX = 0;
                offsetY = 0;
                return new Bitmap(
                    Math.Max(1, glyph.AdvanceX * scale),
                    Math.Max(1, glyph.Height * scale),
                    System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            }

            float minimumX = float.MaxValue;
            float minimumY = float.MaxValue;
            float maximumX = float.MinValue;
            float maximumY = float.MinValue;
            int drawableContours = 0;

            for (int contourIndex = 0; contourIndex < contours.Count; contourIndex++)
            {
                FontOutlineContour contour = contours[contourIndex];
                if (contour == null || contour.Points == null || contour.Points.Count < 3)
                    continue;

                drawableContours++;
                for (int pointIndex = 0; pointIndex < contour.Points.Count; pointIndex++)
                {
                    PointF point = contour.Points[pointIndex];
                    minimumX = Math.Min(minimumX, point.X);
                    minimumY = Math.Min(minimumY, point.Y);
                    maximumX = Math.Max(maximumX, point.X);
                    maximumY = Math.Max(maximumY, point.Y);
                }
            }

            if (drawableContours == 0)
            {
                offsetX = 0;
                offsetY = 0;
                return new Bitmap(
                    Math.Max(1, glyph.AdvanceX * scale),
                    Math.Max(1, glyph.Height * scale),
                    System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            }

            const int margin = 1;
            int left = (int)Math.Floor(minimumX * scale) - margin;
            int top = (int)Math.Floor(minimumY * scale) - margin;
            int right = (int)Math.Ceiling(maximumX * scale) + margin;
            int bottom = (int)Math.Ceiling(maximumY * scale) + margin;
            int width = Math.Max(1, right - left + 1);
            int height = Math.Max(1, bottom - top + 1);

            Bitmap bitmap = new Bitmap(
                width, height,
                System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            using (Graphics graphics = Graphics.FromImage(bitmap))
            using (System.Drawing.Drawing2D.GraphicsPath path =
                new System.Drawing.Drawing2D.GraphicsPath(
                    System.Drawing.Drawing2D.FillMode.Alternate))
            {
                graphics.Clear(Color.Transparent);
                graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
                graphics.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;

                for (int contourIndex = 0; contourIndex < contours.Count; contourIndex++)
                {
                    FontOutlineContour contour = contours[contourIndex];
                    if (contour == null || contour.Points == null || contour.Points.Count < 3)
                        continue;

                    PointF[] points = new PointF[contour.Points.Count];
                    for (int pointIndex = 0; pointIndex < contour.Points.Count; pointIndex++)
                    {
                        PointF source = contour.Points[pointIndex];
                        points[pointIndex] = new PointF(
                            source.X * scale - left,
                            source.Y * scale - top);
                    }
                    path.AddPolygon(points);
                }

                graphics.FillPath(Brushes.Black, path);
            }

            offsetX = left;
            offsetY = top;
            return bitmap;
        }

        private static Bitmap RenderVectorGlyph(FontGlyph glyph, int scale, out int offsetX, out int offsetY)
        {
            List<FontVectorSegment> segments = glyph.VectorSegments;
            if (segments == null || segments.Count == 0)
            {
                offsetX = 0;
                offsetY = 0;
                return new Bitmap(Math.Max(1, glyph.AdvanceX * scale), Math.Max(1, glyph.Height * scale), System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            }

            int minX = int.MaxValue;
            int minY = int.MaxValue;
            int maxX = int.MinValue;
            int maxY = int.MinValue;
            for (int i = 0; i < segments.Count; i++)
            {
                FontVectorSegment segment = segments[i];
                minX = Math.Min(minX, Math.Min(segment.X1, segment.X2));
                minY = Math.Min(minY, Math.Min(segment.Y1, segment.Y2));
                maxX = Math.Max(maxX, Math.Max(segment.X1, segment.X2));
                maxY = Math.Max(maxY, Math.Max(segment.Y1, segment.Y2));
            }

            float penWidth = Math.Max(1.0f, (float)scale);
            int margin = Math.Max(1, (int)Math.Ceiling(penWidth / 2.0f));
            int left = minX * scale - margin;
            int top = minY * scale - margin;
            int right = maxX * scale + margin;
            int bottom = maxY * scale + margin;
            int width = Math.Max(1, right - left + 1);
            int height = Math.Max(1, bottom - top + 1);

            Bitmap bitmap = new Bitmap(width, height, System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            using (Graphics graphics = Graphics.FromImage(bitmap))
            using (Pen pen = new Pen(Color.Black, penWidth))
            {
                graphics.Clear(Color.Transparent);
                graphics.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;
                graphics.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighQuality;
                graphics.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;
                pen.StartCap = System.Drawing.Drawing2D.LineCap.Round;
                pen.EndCap = System.Drawing.Drawing2D.LineCap.Round;
                pen.LineJoin = System.Drawing.Drawing2D.LineJoin.Round;

                for (int i = 0; i < segments.Count; i++)
                {
                    FontVectorSegment segment = segments[i];
                    graphics.DrawLine(pen,
                        segment.X1 * scale - left,
                        segment.Y1 * scale - top,
                        segment.X2 * scale - left,
                        segment.Y2 * scale - top);
                }
            }

            offsetX = left;
            offsetY = top;
            return bitmap;
        }

        private static int[] ReadGlyphWidths(byte[] resData, FONTINFO16 header, int characterCount, bool isVectorFont)
        {
            int[] widths = new int[characterCount];
            bool[] widthWasRead = new bool[characterCount];
            int version = header.dfVersion;
            int headerSize = version < 0x0200 ? 117 : (version < 0x0300 ? 118 : 148);
            bool isMonospace = (header.dfPitchAndFamily & 1) == 0;
            bool preserveExplicitZeroWidth = version == 0x0100 && !isVectorFont && !isMonospace;

            if (version >= 0x0300)
            {
                for (int i = 0; i < characterCount; i++)
                {
                    int entryOffset = headerSize + i * 6;
                    if (entryOffset + 6 > resData.Length)
                        break;
                    widths[i] = BitConverter.ToUInt16(resData, entryOffset);
                    widthWasRead[i] = true;
                }
            }
            else if (isMonospace)
            {
                int width = header.dfPixWidth;
                if (width <= 0)
                    width = header.dfAvgWidth;
                if (width <= 0)
                    width = 8;
                for (int i = 0; i < characterCount; i++)
                {
                    widths[i] = width;
                    widthWasRead[i] = true;
                }
                return widths;
            }
            else if (version == 0x0100)
            {
                if (isVectorFont)
                {
                    for (int i = 0; i < characterCount; i++)
                    {
                        int entryOffset = headerSize + i * 4;
                        if (entryOffset + 4 > resData.Length)
                            break;
                        widths[i] = BitConverter.ToUInt16(resData, entryOffset + 2);
                        widthWasRead[i] = true;
                    }
                }
                else
                {
                    int totalEntries = characterCount + 1;
                    ushort[] offsets = new ushort[totalEntries];
                    for (int i = 0; i < totalEntries; i++)
                    {
                        int entryOffset = headerSize + i * 2;
                        if (entryOffset + 2 > resData.Length)
                            break;
                        offsets[i] = BitConverter.ToUInt16(resData, entryOffset);
                    }
                    for (int i = 0; i < characterCount; i++)
                    {
                        widths[i] = Math.Max(0, offsets[i + 1] - offsets[i]);
                        widthWasRead[i] = true;
                    }
                }
            }
            else
            {
                for (int i = 0; i < characterCount; i++)
                {
                    int entryOffset = headerSize + i * 4;
                    if (entryOffset + 4 > resData.Length)
                        break;
                    widths[i] = BitConverter.ToUInt16(resData, entryOffset);
                    widthWasRead[i] = true;
                }
            }

            int fallback = header.dfPixWidth;
            if (fallback <= 0 && version >= 0x0300)
                fallback = header.dfBspace;
            if (fallback <= 0)
                fallback = header.dfAvgWidth;
            if (fallback <= 0)
                fallback = 8;
            for (int i = 0; i < widths.Length; i++)
            {
                if (!widthWasRead[i] || (!preserveExplicitZeroWidth && widths[i] <= 0))
                    widths[i] = fallback;
            }

            return widths;
        }

        private static Bitmap CopyGlyph(Bitmap source, int width, int sourceY, int height)
        {
            Bitmap result = new Bitmap(Math.Max(1, width), Math.Max(1, height), System.Drawing.Imaging.PixelFormat.Format32bppArgb);
            using (Graphics graphics = Graphics.FromImage(result))
            {
                graphics.Clear(Color.Transparent);
                Rectangle destination = new Rectangle(0, 0, result.Width, result.Height);
                Rectangle sourceRectangle = new Rectangle(0, sourceY, result.Width, result.Height);
                graphics.DrawImage(source, destination, sourceRectangle, GraphicsUnit.Pixel);
            }
            return result;
        }

        private static string ReadNullTerminatedAnsi(byte[] data, int offset)
        {
            if (data == null || offset <= 0 || offset >= data.Length)
                return string.Empty;
            int end = offset;
            while (end < data.Length && data[end] != 0)
                end++;
            try
            {
                return Encoding.Default.GetString(data, offset, end - offset).Trim();
            }
            catch
            {
                return Encoding.ASCII.GetString(data, offset, end - offset).Trim();
            }
        }

        public static Bitmap Get(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            if (properties != null &&
                ((properties.headerType == ModuleResources.HeaderType.LE && properties.versionType == ModuleResources.VersionType.OS2) ||
                 (properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                 properties.headerType == ModuleResources.HeaderType.LX))
            {
                // Structure is different for OS/2
                return OS2_RT_FONT.Get(resData);
            }
            FONTINFO16 header = ModuleResources.Deserialize<FONTINFO16>(resData);
            int dfVersion = header.dfVersion;
            int headerSize;

            // Special thanks to RubyTuesday from BetaArchive for those magic numbers:
            // https://www.betaarchive.com/forum/viewtopic.php?t=33486
            if (dfVersion < 0x0200)
            {
                headerSize = 117;  // version 1.x
            }
            else if (dfVersion < 0x0300)
            {
                headerSize = 118;  // version 2.x
            }
            else
            {
                headerSize = 148;  // version 3.x and later
            }

            Debug.WriteLine($"headerSize: {headerSize}");
            Debug.WriteLine($"dfVersion: {dfVersion}");
            uint dfBitsOffset = header.dfBitsOffset;

            uint totalSize = header.dfSize;

            ushort dfPixHeight = header.dfPixHeight;
            ushort dfPixWidth = header.dfPixWidth;
            ushort dfMaxWidth = header.dfMaxWidth;
            int dfFirstChar = header.dfFirstChar;
            int dfLastChar = header.dfLastChar;
            int charCount = dfLastChar - dfFirstChar + 1;

            bool isMonospace = (header.dfPitchAndFamily & 1) == 0;

            int[] glyphWidths = new int[charCount];
            int[] glyphOffsets = new int[charCount];

            uint bitmapDataLength = 0;
            if (dfBitsOffset < resData.Length)
            {
                bitmapDataLength = totalSize > dfBitsOffset ? totalSize - dfBitsOffset : (uint)resData.Length - dfBitsOffset;
                if (dfBitsOffset + bitmapDataLength > resData.Length)
                    bitmapDataLength = (uint)resData.Length - dfBitsOffset;
            }

            ushort dfType = header.dfType;
            uint dfFlags = header.dfFlags;

            bool isBitmapFont = (dfType & 0x0001) == 0;
            bool isDataInFile = (dfType & 0x0004) == 0;
            bool isVectorFont = (dfType & 0x0001) == 1;

            if (!isDataInFile)
            {
                Debug.WriteLine("Bitmap data is not contained in the file, handling not supported.");
            }
            Debug.WriteLine($"dfType: 0x{dfType:X4}");

            if (dfVersion >= 0x0300)
            {
                // Version 3 always uses GLYPHENTRY30: WORD width + DWORD absolute offset.
                // The table is present for fixed-width fonts as well as proportional fonts.
                const int entrySize = 6;
                int fallbackWidth = dfPixWidth > 0 ? dfPixWidth : header.dfBspace;
                if (fallbackWidth <= 0)
                    fallbackWidth = header.dfAvgWidth;
                if (fallbackWidth <= 0)
                    fallbackWidth = 8;

                for (int i = 0; i < charCount; i++)
                {
                    int entryOffset = headerSize + i * entrySize;
                    if (entryOffset + entrySize > resData.Length)
                        break;

                    int width = BitConverter.ToUInt16(resData, entryOffset);
                    uint offset = BitConverter.ToUInt32(resData, entryOffset + 2);
                    glyphWidths[i] = width > 0 ? width : fallbackWidth;
                    glyphOffsets[i] = offset <= int.MaxValue ? (int)offset : 0;
                }
            }
            else if (isMonospace)
            {
                // raster monospace
                int offset = 0;
                int monoWidth = dfPixWidth;
                if (monoWidth == 0) 
                    monoWidth = 8;
                int bytesPerGlyph = ((monoWidth + 7) / 8) * dfPixHeight;
                for (int i = 0; i < charCount; i++)
                {
                    glyphWidths[i] = monoWidth;
                    if (dfVersion > 0x0100)
                    {
                        glyphOffsets[i] = (int)header.dfBitsOffset + i * bytesPerGlyph;
                    }
                    else 
                    {
                        // ver 1.x raster monospace
                        glyphOffsets[i] = (int)header.dfBitsOffset * 8 + offset; // bit
                        offset += monoWidth;
                    }
                }
            }
            else
            {
                uint charTableLength = dfBitsOffset - (uint)headerSize;

                int entrySize;
                string glyphEntryFormat;

                // Special thanks to File Format Encyclopedia for helping me with ver2 and ver3:
                // https://ffenc.blogspot.com/2008/04/fnt-font-file-format.html
                if (dfVersion == 0x0100)  // Windows 1.x font version
                {
                    // Special thanks to OS2Museum (https://www.os2museum.com/files/docs/win10sdk/windows-1.03-sdk-prgref-1986.pdf)
                    // for providing the reference SDK manual describing how to read the ver. 1 vectorial and raster FNT format
                    if (isVectorFont)
                    {
                        glyphEntryFormat = "Windows 1.x dfCharTable[] (offset + width)";
                        entrySize = 4;

                        for (int i = 0; i < charCount; i++)
                        {
                            int entryOffset = headerSize + i * entrySize;
                            if (entryOffset + entrySize > resData.Length)
                            {
                                Console.WriteLine($"Truncated glyph at {i}");
                                break;
                            }

                            int offset = BitConverter.ToUInt16(resData, entryOffset);
                            ushort width = BitConverter.ToUInt16(resData, entryOffset + 2);

                            glyphOffsets[i] = offset;
                            glyphWidths[i] = width;

                            Console.WriteLine($"Char {(char)(i + dfFirstChar)} (code {i + dfFirstChar}) → Offset: {offset}, Width: {width}");
                        }
                    }
                    else
                    {
                        // We expect to have only the offset and no width in raster fonts ver. 1
                        glyphEntryFormat = "Windows 1.x dfCharOffset[] (offset only)";
                        entrySize = 2;

                        int totalEntries = charCount + 1;

                        ushort[] offsets = new ushort[totalEntries];

                        for (int i = 0; i < totalEntries; i++)
                        {
                            int entryOffset = headerSize + i * entrySize;
                            if (entryOffset + entrySize > resData.Length)
                            {
                                Console.WriteLine($"Truncated offset table at {i}");
                                break;
                            }
                            offsets[i] = BitConverter.ToUInt16(resData, entryOffset);
                        }

                        for (int i = 0; i < charCount; i++)
                        {
                            glyphWidths[i] = offsets[i + 1] - offsets[i];
                            glyphOffsets[i] = (int)(dfBitsOffset * 8) + offsets[i];
                            Console.WriteLine($"Char {(char)(i + dfFirstChar)} (code {i + dfFirstChar}): Offset = {glyphOffsets[i]}, Width (bit) = {glyphWidths[i]}");
                        }
                    }
                }
                else if (dfVersion <= 0x0200) // Windows 2.x font version
                {
                    glyphEntryFormat = "Windows 2.x dfCharTable";
                    entrySize = 4;

                    for (int i = 0; i < charCount; i++)
                    {
                        int pos = headerSize + i * entrySize;
                        ushort width = BitConverter.ToUInt16(resData, pos);
                        ushort offset = BitConverter.ToUInt16(resData, pos + 2);
                        glyphWidths[i] = width;
                        glyphOffsets[i] = offset;
                    }
                }
                else // Windows 3.x and later font version
                {
                    if ((dfFlags & 0x0004) != 0) // DFF_ABCFIXED
                    {
                        glyphEntryFormat = "DFF_ABCFIXED";
                        entrySize = 16; // WORD + DWORD*3 + WORD
                    }
                    else if ((dfFlags & 0x0008) != 0) // DFF_ABCPROPORTIONAL
                    {
                        glyphEntryFormat = "DFF_ABCPROPORTIONAL";
                        entrySize = 16;
                    }
                    else if ((dfFlags & 0x0001) != 0) // DFF_FIXED
                    {
                        glyphEntryFormat = "DFF_FIXED";
                        entrySize = 6;  // WORD + DWORD
                    }
                    else if ((dfFlags & 0x0002) != 0) // DFF_PROPORTIONAL
                    {
                        glyphEntryFormat = "DFF_PROPORTIONAL";
                        entrySize = 6;
                    }
                    else if ((dfFlags & 0x0010) != 0) // DFF_COLOR
                    {
                        glyphEntryFormat = "DFF_COLOR";
                        entrySize = 20;
                    }
                    else
                    {
                        glyphEntryFormat = "Format not recognized";
                        entrySize = 0;
                    }

                    // WARNING: PLACEHOLDER
                    // NO FNT AVAILABLE TO TEST V3 FNTs
                    for (int i = 0; i < charCount; i++)
                        glyphWidths[i] = 8;
                }

                Console.WriteLine($"GlyphEntry Format: {glyphEntryFormat} (entry size: {entrySize} bytes)");
            }

            int maxWidth = 0;
            for (int i = 0; i < charCount; i++)
            {
                if (glyphWidths[i] > maxWidth)
                    maxWidth = glyphWidths[i];
            }

            // Create the final bitmap
            // maxWidth to the left, all the glyphs vertically stacked
            if (maxWidth <= 0)
                maxWidth = dfPixWidth > 0 ? dfPixWidth : 8;
            Bitmap bmp = new Bitmap(Math.Max(1, maxWidth), Math.Max(1, dfPixHeight * charCount));
            if (isVectorFont)
            {
                // dfAscent here might be wrong used like this
                bmp = new Bitmap(maxWidth, (dfPixHeight + header.dfAscent) * charCount);
            }
            using (Graphics g = Graphics.FromImage(bmp))
            {
                g.Clear(Color.Transparent);
            }

            // Special thanks to OS2Museum (https://www.os2museum.com/files/docs/win10sdk/windows-1.03-sdk-prgref-1986.pdf)
            // for providing the reference SDK manual describing how to read the ver. 1 vectorial and raster FNT format
            if (dfVersion == 0x0100 && isVectorFont)
            {
                // The manual was transformed to code with the help of ChatGPT and DeepSeek, that were able to correct each other mistakes, after I provided them my base code.
                // some fonts in this format are roman.fon, script.fon, modern.fon
                bool coords2Byte = (dfPixHeight > 128 || dfMaxWidth > 128);
                Bitmap[] glyphBitmaps = new Bitmap[charCount];

                // dfAscent here might be wrong used like this
                int yOffset = dfPixHeight - header.dfAscent;

                for (int i = 0; i < charCount; i++)
                {
                    // apparently sometimes the width is zero. This code is a defense in order to not crash, we don't expect any drawning.
                    // an example is with script.fon
                    int glyphWidth = glyphWidths[i] <= 0 ? dfPixWidth : glyphWidths[i];
                    glyphWidth = glyphWidth <= 0 ? 8 : glyphWidth;

                    // dfAscent here might be wrong used like this
                    Bitmap bmpChar = new Bitmap(glyphWidth, dfPixHeight + header.dfAscent);
                    using (Graphics gChar = Graphics.FromImage(bmpChar))
                    {
                        // with this only option the render is really better than the native Windows Font Viewer, it looks like a TrueType font!
                        gChar.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.HighQuality;

                        gChar.Clear(Color.Transparent);
                        Pen pen = new Pen(Color.Black, 1);

                        int offsetStroke = glyphOffsets[i];
                        int offsetStrokeNext = (i == charCount - 1)
                            ? (int)bitmapDataLength  // use the correct bitmap width
                            : glyphOffsets[i + 1];

                        int strokeDataStart = (int)dfBitsOffset + offsetStroke;
                        int strokeLength = offsetStrokeNext - offsetStroke;

                        Debug.WriteLine($"Char {i + dfFirstChar}: offset={offsetStroke}, len={strokeLength}, width={(ushort)glyphWidth}");

                        if (strokeLength <= 0 || strokeDataStart < 0 || strokeDataStart + strokeLength > resData.Length)
                        {
                            glyphBitmaps[i] = bmpChar;
                            continue;
                        }

                        int x = 0, y = 0;
                        int pos = 0;
                        bool penDown = false;
                        Point? lastPoint = null;

                        while (pos < strokeLength)
                        {
                            // 1. first check if it is a pen-up marker
                            if (coords2Byte)
                            {
                                if (pos + 2 > strokeLength) break;
                                short marker = BitConverter.ToInt16(resData, strokeDataStart + pos);

                                if (marker == -32768)  // pen-up for 2-bytes
                                {
                                    penDown = false;
                                    lastPoint = null;
                                    pos += 2;
                                    continue;
                                }
                            }
                            else
                            {
                                if (pos + 1 > strokeLength) break;
                                sbyte marker = (sbyte)resData[strokeDataStart + pos];

                                if (marker == -128)  // pen-up for 1-byte
                                {
                                    penDown = false;
                                    lastPoint = null;
                                    pos += 1;
                                    continue;
                                }
                            }

                            // 2. read the coordinates
                            int dx, dy;

                            if (coords2Byte)
                            {
                                if (pos + 4 > strokeLength) break;
                                dx = BitConverter.ToInt16(resData, strokeDataStart + pos);
                                dy = BitConverter.ToInt16(resData, strokeDataStart + pos + 2);
                                pos += 4;
                            }
                            else
                            {
                                if (pos + 2 > strokeLength) break;
                                dx = (sbyte)resData[strokeDataStart + pos];
                                dy = (sbyte)resData[strokeDataStart + pos + 1];
                                pos += 2;
                            }

                            x += dx;
                            y += dy;

                            // apply yOffset for the vertial alignment
                            Point currentPoint = new Point(x, y + yOffset);

                            if (penDown && lastPoint.HasValue)
                            {
                                // draw only if the pen is down
                                gChar.DrawLine(pen, lastPoint.Value, currentPoint);
                                lastPoint = currentPoint;
                            }
                            else
                            {
                                // first point after pen-up
                                lastPoint = currentPoint;
                                penDown = true;
                            }
                        }
                    }
                    glyphBitmaps[i] = bmpChar;
                }

                // vertical merge all the bitmaps
                using (Graphics g = Graphics.FromImage(bmp))
                {
                    g.Clear(Color.Transparent);

                    for (int i = 0; i < charCount; i++)
                    {
                        g.DrawImageUnscaled(glyphBitmaps[i], 0, i * (dfPixHeight + header.dfAscent));
                        glyphBitmaps[i].Dispose();
                    }
                }
                return bmp;
            }
            else if (dfVersion == 0x0100 && !isVectorFont)
            {
                // Special thanks again to RubyTuesday from BetaArchive for better explaining than the docs how the pixels are stored in v1 raster:
                // https://www.betaarchive.com/forum/viewtopic.php?t=33486
                int totalWidthInBits = header.dfWidthBytes * 8; // The sum of all the widths is not the same to this number and leads to errors, we must use this one. 
                Bitmap[] glyphBitmaps = new Bitmap[charCount];
                for (int i = 0; i < charCount; i++)
                {
                    int glyphWidth = glyphWidths[i];          // glyph width in bit; zero is valid for an empty glyph
                    int glyphOffset = glyphOffsets[i];        // absolute offset in bit

                    Bitmap bmpChar = new Bitmap(Math.Max(1, glyphWidth), Math.Max(1, (int)dfPixHeight));
                    for (int y = 0; y < dfPixHeight; y++)
                    {
                        for (int x = 0; x < glyphWidth; x++)
                        {
                            // calculate absolute bit offset of current bit in buffer:
                            int bitPos = glyphOffset + y * totalWidthInBits + x;

                            // calculate internal byte and bit
                            int bytePos = bitPos / 8;
                            int bitInByte = 7 - (bitPos % 8);

                            if (bytePos >= 0 && bytePos < resData.Length)
                            {
                                bool bitSet = (resData[bytePos] & (1 << bitInByte)) != 0;
                                bmpChar.SetPixel(x, y, bitSet ? Color.Black : Color.Transparent);
                            }
                            else
                            {
                                bmpChar.SetPixel(x, y, Color.Transparent);
                            }
                        }
                    }
                    glyphBitmaps[i] = bmpChar;
                }

                // vertical merge all the bitmaps
                using (Graphics g = Graphics.FromImage(bmp))
                {
                    g.Clear(Color.Transparent);

                    for (int i = 0; i < charCount; i++)
                    {
                        g.DrawImageUnscaled(glyphBitmaps[i], 0, i * dfPixHeight);
                        glyphBitmaps[i].Dispose();
                    }
                }
                return bmp;
            }
            else
            {
                for (int i = 0; i < charCount; i++)
                {
                    int glyphWidth = glyphWidths[i];          // use real glyph width
                    int glyphOffset = glyphOffsets[i];        // absolute offset of the first pseudo-glyph

                    for (int y = 0; y < dfPixHeight; y++)                // vertical row in the glyph
                    {
                        for (int x = 0; x < glyphWidth; x++)             // horizontal pixel in the glyph
                        {
                            int stripeIdx = x / 8;                       // which 8px stripe
                            int bitIdx = 7 - (x % 8);                    // bit (MSB-first) inside the byte

                            // address of the byte that contains the bit of this pixel
                            int rowOffset = glyphOffset                  // start of the first stripe
                                            + stripeIdx * dfPixHeight    // skip to next stripes
                                            + y;                         // current row within the stripe

                            if (rowOffset >= resData.Length) continue;   // safety check

                            byte b = resData[rowOffset];
                            bool pixelOn = ((b >> bitIdx) & 1) != 0;

                            bmp.SetPixel(x, y + i * dfPixHeight, pixelOn ? Color.Black : Color.Transparent);
                        }
                    }
                }
            }
            return bmp;
        }
    }
}