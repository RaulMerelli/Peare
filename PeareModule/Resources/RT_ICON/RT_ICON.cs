using System;
using System.Drawing;
using System.IO;

namespace PeareModule
{
    public static class RT_ICON
    {
        public static Bitmap Get(byte[] resData)
        {
            if (resData.Length > 4 &&
                resData[0] == 0x89 && resData[1] == 0x50 &&
                resData[2] == 0x4E && resData[3] == 0x47)
            {
                using (var ms = new MemoryStream(resData))
                {
                    return new Bitmap(ms); // PNG format
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
                return new Bitmap(1, 1);
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
                return new Bitmap(1, 1);
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
    }
}
