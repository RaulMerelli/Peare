using System;
using System.Collections;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace PeareModule
{
    public static class RT_FONT
    {
        public static Bitmap Get(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            if ((properties.headerType == ModuleResources.HeaderType.LE && properties.versionType == ModuleResources.VersionType.OS2) ||
                (properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                properties.headerType == ModuleResources.HeaderType.LX)
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

            uint bitmapDataLength = totalSize - dfBitsOffset;
            if (dfBitsOffset + bitmapDataLength > resData.Length)
                bitmapDataLength = (uint)resData.Length - dfBitsOffset;

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

            if (isMonospace)
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
            Bitmap bmp = new Bitmap(maxWidth, dfPixHeight * charCount);
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
                    int glyphWidth = glyphWidths[i];          // glyph width in bit
                    int glyphOffset = glyphOffsets[i];        // absolute offset in bit

                    Bitmap bmpChar = new Bitmap(glyphWidth, dfPixHeight);
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