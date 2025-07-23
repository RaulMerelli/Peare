using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

namespace PeareModule
{
    public static class RT_ICON
    {
        public static Img Get(byte[] resData)
        {
            if (resData.Length > 4 &&
                resData[0] == 0x89 && resData[1] == 0x50 &&
                resData[2] == 0x4E && resData[3] == 0x47)
            {
                using (var ms = new MemoryStream(resData))
                {
                    // PNG format
                    Bitmap bmp = new Bitmap(ms);

                    Img img = new Img();
                    img.BitCount = Image.GetPixelFormatSize(bmp.PixelFormat);
                    img.Size = new Size(bmp.Width, bmp.Height);
                    img.Bitmap = bmp;
                    return img;
                }
            }

            int biSize = BitConverter.ToInt32(resData, 0);
            int width = BitConverter.ToInt32(resData, 4);
            int fullHeight = BitConverter.ToInt32(resData, 8);
            int height = fullHeight / 2;
            ushort bitCount = BitConverter.ToUInt16(resData, 14);

            if (width <= 0 || height <= 0 || bitCount == 0)
            {
                Console.WriteLine("Invalid bitmap dimensions or bit count.");
                return Get_ICON_Win1_Win2(resData);
            }

            int paletteEntries = 0;
            if (bitCount <= 8)
            {
                paletteEntries = BitConverter.ToInt32(resData, 32);
                if (paletteEntries == 0) paletteEntries = 1 << bitCount;
            }

            long pixelDataOffset = (long)biSize + (long)paletteEntries * 4;
            long colorStride = (((long)width * bitCount + 31) / 32) * 4;
            long maskStride = (((long)width + 31) / 32) * 4;
            long maskDataOffset = pixelDataOffset + colorStride * height;

            long pixelDataLength = colorStride * height;
            long maskDataLength = maskStride * height;

            if (pixelDataOffset + pixelDataLength > resData.LongLength ||
                maskDataOffset + maskDataLength > resData.LongLength)
            {
                Console.WriteLine("Data does not contain enough bytes for pixel or mask data.");
                return Get_ICON_Win1_Win2(resData);
            }

            // Extract palette
            Color[] palette = null;
            if (bitCount <= 8)
            {
                palette = new Color[paletteEntries];
                int paletteStart = biSize;
                for (int j = 0; j < paletteEntries; j++)
                {
                    int entryOffset = paletteStart + j * 4;
                    if (entryOffset + 3 >= resData.Length) break;

                    byte b = resData[entryOffset];
                    byte g = resData[entryOffset + 1];
                    byte r = resData[entryOffset + 2];
                    palette[j] = Color.FromArgb(255, r, g, b);
                }
            }

            // Allocate and copy large image buffers
            byte[] pixelData = new byte[pixelDataLength];
            byte[] maskData = new byte[maskDataLength];

            RT_BITMAP.CopyLarge(resData, pixelDataOffset, pixelData, 0, pixelDataLength);
            RT_BITMAP.CopyLarge(resData, maskDataOffset, maskData, 0, maskDataLength);

            return RT_BITMAP.GenerateBitmapFromData(pixelData, maskData, width, height, bitCount, palette);
        }

        public static Img Get_ICON_Win1_Win2(byte[] resData)
        {
            Img img = new Img();
            if (resData == null || resData.Length < 14) // Minimum for the common header
            {
                Console.WriteLine("Error: Windows 1.0 resource data too short for header.");
                Bitmap bmp = new Bitmap(1, 1);

                img.BitCount = 0;
                img.Size = new Size(0, 0);
                img.Bitmap = bmp;
                return img;
            }

            // 1. Read the common header (14 bytes)
            UInt32 resourceId = BitConverter.ToUInt32(resData, 0);

            // Fields that are HotspotX/Y for cursors, or something else for icons
            ushort fieldA = BitConverter.ToUInt16(resData, 2);
            ushort fieldB = BitConverter.ToUInt16(resData, 4);

            ushort width = BitConverter.ToUInt16(resData, 6);
            ushort height = BitConverter.ToUInt16(resData, 8);
            ushort bytesPerLine = BitConverter.ToUInt16(resData, 10); // Stride

            Console.WriteLine($"Attempting to decode Win1.0 Resource: ID=0x{resourceId:X}, FieldA={fieldA}, FieldB={fieldB}, Width={width}, Height={height}, BytesPerLine={bytesPerLine}");

            if (width == 0 || height == 0 || bytesPerLine == 0)
            {
                Console.WriteLine("Error: Invalid dimensions or bytesPerLine in Windows 1.0 resource.");
                Bitmap bmp = new Bitmap(1, 1);

                img.BitCount = 0;
                img.Size = new Size(0, 0);
                img.Bitmap = bmp;
                return img;
            }

            int headerSize = 14; // Size of the header we've identified
            long maskDataSize = (long)bytesPerLine * height;

            // Verify there’s enough data for both masks
            if (headerSize + maskDataSize * 2 > resData.Length)
            {
                Console.WriteLine($"Error: Truncated Windows 1.0 resource data. Expected {maskDataSize * 2} bytes for masks, got {resData.Length - headerSize}.");
                Bitmap bmp = new Bitmap(1, 1);

                img.BitCount = 0;
                img.Size = new Size(0, 0);
                img.Bitmap = bmp;
                return img;
            }

            byte[] mask1Data = new byte[maskDataSize];
            byte[] mask2Data = new byte[maskDataSize];

            Buffer.BlockCopy(resData, headerSize, mask1Data, 0, (int)maskDataSize); // XOR Mask
            Buffer.BlockCopy(resData, headerSize + (int)maskDataSize, mask2Data, 0, (int)maskDataSize); // AND Mask
            byte[] xorMaskData = mask1Data;
            byte[] andMaskData = mask2Data;

            // 3. Generate the Bitmap
            Bitmap resultBitmap = null;
            try
            {
                resultBitmap = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            }
            catch (ArgumentException ex)
            {
                Console.WriteLine($"Error creating bitmap {width}x{height}: {ex.Message}");
                Bitmap bmp = new Bitmap(1, 1);

                img.BitCount = 0;
                img.Size = new Size(0, 0);
                img.Bitmap = bmp;
                return img;
            }

            // Colors for monochrome cursor with transparency
            Color black = Color.Black;
            Color white = Color.White;
            Color transparent = Color.FromArgb(0, 0, 0, 0); // Fully transparent

            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int byteIndex = x / 8;
                    int bitIndex = 7 - (x % 8); // Bits are stored MSB to LSB per byte in this format

                    // Safety check to avoid IndexOutOfRangeException
                    if (y * bytesPerLine + byteIndex >= xorMaskData.Length ||
                        y * bytesPerLine + byteIndex >= andMaskData.Length)
                    {
                        Console.WriteLine($"Warning: Data out of bounds at Y={y}, X={x}. Skipping pixel.");
                        continue;
                    }

                    byte andByte = andMaskData[y * bytesPerLine + byteIndex];
                    byte xorByte = xorMaskData[y * bytesPerLine + byteIndex];

                    bool andBit = ((andByte >> bitIndex) & 1) == 1; // 1 = opaque, 0 = transparent
                    bool xorBit = ((xorByte >> bitIndex) & 1) == 1; // 1 = white, 0 = black (if opaque)

                    Color pixelColor;

                    // Logic for combining AND/XOR for Windows 1.0/2.0 monochrome bitmaps
                    // This is the formula: (P_screen AND (NOT A)) XOR X
                    // For displaying on an RGB bitmap, we simplify:
                    // If the AND bit is 0, the pixel is transparent (regardless of XOR)
                    if (!andBit) // AND bit is 0 -> transparent
                    {
                        pixelColor = transparent;
                    }
                    else // AND bit is 1 -> opaque (color is determined by XOR)
                    {
                        if (!xorBit) // XOR bit is 0 -> black
                        {
                            pixelColor = black;
                        }
                        else // XOR bit is 1 -> white
                        {
                            pixelColor = white;
                        }
                    }
                    resultBitmap.SetPixel(x, y, pixelColor);
                }
            }

            img.BitCount = Image.GetPixelFormatSize(resultBitmap.PixelFormat);
            img.Size = new Size(resultBitmap.Width, resultBitmap.Height);
            img.Bitmap = resultBitmap;
            return img;
        }
    }
}
