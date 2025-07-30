using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
using System.Collections.Generic;

namespace PeareModule
{
    public static class OS2_RT_FONT
    {
        public static Bitmap Get(byte[] resData)
        {
            // Validate minimum data length
            if (resData.Length < 20 + 168 + 28)
                throw new InvalidOperationException("Invalid font data: Insufficient length");

            // Read FONTSIGNATURE (20 bytes) - assert as validated
            // Skip FOCAMETRICS (168 bytes)
            const int focametricsOffset = 20;
            int offset = focametricsOffset + 168; // Move to FONTDEFINITIONHEADER

            // Read FONTDEFINITIONHEADER (28 bytes)
            short yCellHeight = BitConverter.ToInt16(resData, offset + 16); // Character height
            short usCellSize = BitConverter.ToInt16(resData, offset + 12);   // Cell size
            offset += 28; // Move to cell array

            const int numCharacters = 300;
            int cellArraySize = numCharacters * usCellSize;

            // Validate cell array
            if (resData.Length < offset + cellArraySize)
                return null;

            // Read glyph widths and offsets
            int[] glyphWidths = new int[numCharacters];
            uint[] glyphOffsets = new uint[numCharacters]; // Absolute offsets within resource
            int maxWidth = 0;

            for (int i = 0; i < numCharacters; i++)
            {
                int cellOffset = offset + i * usCellSize;

                if (usCellSize == 6) // TYPE1/TYPE2
                {
                    glyphOffsets[i] = BitConverter.ToUInt32(resData, cellOffset);
                    glyphWidths[i] = BitConverter.ToUInt16(resData, cellOffset + 4);
                }
                else if (usCellSize == 10) // TYPE3
                {
                    glyphOffsets[i] = BitConverter.ToUInt32(resData, cellOffset);
                    glyphWidths[i] = BitConverter.ToUInt16(resData, cellOffset + 6);
                }
                else
                {
                    throw new InvalidOperationException($"Unsupported cell size: {usCellSize}");
                }

                if (glyphWidths[i] > maxWidth)
                    maxWidth = glyphWidths[i];
            }

            int totalHeight = numCharacters * yCellHeight;
            int stride = (maxWidth + 31) / 32 * 4; // DWORD-aligned stride
            Bitmap bitmap = new Bitmap(maxWidth, totalHeight, PixelFormat.Format1bppIndexed);

            // Set palette (0=white, 1=black)
            ColorPalette palette = bitmap.Palette;
            palette.Entries[0] = Color.Transparent;
            palette.Entries[1] = Color.Black;
            bitmap.Palette = palette;

            BitmapData bmpData = bitmap.LockBits(
                new Rectangle(0, 0, bitmap.Width, bitmap.Height),
                ImageLockMode.WriteOnly,
                PixelFormat.Format1bppIndexed
            );

            try
            {
                IntPtr scan0 = bmpData.Scan0;
                int bytesPerScanline = Math.Abs(bmpData.Stride);
                byte[] blankLine = new byte[bytesPerScanline]; // Initialized to 0 (transparent)

                // Clear entire bitmap to transparent
                for (int y = 0; y < totalHeight; y++)
                {
                    IntPtr linePtr = scan0 + y * bytesPerScanline;
                    Marshal.Copy(blankLine, 0, linePtr, blankLine.Length);
                }

                for (int charIndex = 0; charIndex < numCharacters; charIndex++)
                {
                    int width = glyphWidths[charIndex];
                    int height = yCellHeight;
                    int columns = (width + 7) / 8;
                    uint glyphDataOffset = glyphOffsets[charIndex];
                    int globalY = charIndex * height;

                    // Skip zero-width glyphs
                    if (width <= 0 || height <= 0)
                        continue;

                    // Calculate required data size for this glyph
                    int requiredDataSize = columns * height;

                    // Skip glyph if data would extend beyond resource bounds
                    if (glyphDataOffset + requiredDataSize > resData.Length)
                        continue;

                    // Process each row
                    for (int row = 0; row < height; row++)
                    {
                        IntPtr targetLine = scan0 + (globalY + row) * bytesPerScanline;

                        // Process each column in this row
                        for (int col = 0; col < columns; col++)
                        {
                            int dataPos = (int)glyphDataOffset + col * height + row;
                            if (dataPos >= resData.Length) break;

                            byte srcByte = resData[dataPos];
                            int targetX = col * 8;

                            // Process each bit in the byte
                            for (int bit = 0; bit < 8; bit++)
                            {
                                int pixelX = targetX + bit;
                                if (pixelX >= width) break;

                                int targetByte = pixelX / 8;
                                int bitPos = 7 - (pixelX % 8);

                                // Skip if beyond bitmap width
                                if (targetByte >= bytesPerScanline) continue;

                                if ((srcByte & (0x80 >> bit)) != 0) // Set pixel if bit is on
                                {
                                    IntPtr targetPtr = targetLine + targetByte;
                                    byte current = Marshal.ReadByte(targetPtr);
                                    current |= (byte)(1 << bitPos);
                                    Marshal.WriteByte(targetPtr, current);
                                }
                            }
                        }
                    }
                }
            }
            finally
            {
                bitmap.UnlockBits(bmpData);
            }

            return bitmap;
        }
    }
}