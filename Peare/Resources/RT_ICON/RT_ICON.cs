using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;

namespace Peare
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
                    return new Bitmap(ms); // load directly the PNG and skip the old format
                }
            }

            int biSize = BitConverter.ToInt32(resData, 0);
            int width = BitConverter.ToInt32(resData, 4);
            int fullHeight = BitConverter.ToInt32(resData, 8);
            int height = fullHeight / 2;
            ushort bitCount = BitConverter.ToUInt16(resData, 14);

            int paletteEntries = 0;
            if (bitCount <= 8)
            {
                paletteEntries = BitConverter.ToInt32(resData, 32);
                if (paletteEntries == 0) paletteEntries = 1 << bitCount;
            }

            int pixelDataOffset = biSize + paletteEntries * 4;
            int colorStride = ((width * bitCount + 31) / 32) * 4;
            int maskStride = ((width + 31) / 32) * 4;
            int maskDataOffset = pixelDataOffset + colorStride * height;

            // Extract palette
            Color[] palette = null;
            if (bitCount <= 8)
            {
                palette = new Color[paletteEntries];
                int paletteStart = biSize;
                for (int j = 0; j < paletteEntries; j++)
                {
                    if (paletteStart + j * 4 + 3 >= resData.Length)
                        break;
                    byte b = resData[paletteStart + j * 4];
                    byte g = resData[paletteStart + j * 4 + 1];
                    byte r = resData[paletteStart + j * 4 + 2];
                    palette[j] = Color.FromArgb(255, r, g, b);
                }
            }

            // Only pass the sections that are already "offsetted"
            byte[] pixelData = new byte[colorStride * height];
            byte[] maskData = new byte[maskStride * height];
            Buffer.BlockCopy(resData, pixelDataOffset, pixelData, 0, pixelData.Length);
            Buffer.BlockCopy(resData, maskDataOffset, maskData, 0, maskData.Length);

            return RT_BITMAP.GenerateBitmapFromData(pixelData, maskData, width, height, bitCount, palette);
        }
    }
}
