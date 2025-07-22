using System;
using System.Drawing;
using System.IO;

namespace PeareModule
{
    public static class RT_CURSOR
    {
        public static Bitmap Get(byte[] resData)
        {
            if (resData.Length > 4 &&
                resData[0] == 0x89 && resData[1] == 0x50 &&
                resData[2] == 0x4E && resData[3] == 0x47)
            {
                using (var ms = new MemoryStream(resData))
                {
                    return new Bitmap(ms); // PNG cursor, does it exists?
                }
            }

            // Skip first 4 bytes (hotspotX + hotspotY)
            const int hotspotOffset = 4;

            int biSize = BitConverter.ToInt32(resData, hotspotOffset + 0);
            int width = BitConverter.ToInt32(resData, hotspotOffset + 4);
            int fullHeight = BitConverter.ToInt32(resData, hotspotOffset + 8);
            int height = fullHeight / 2;
            ushort bitCount = BitConverter.ToUInt16(resData, hotspotOffset + 14);

            int paletteEntries = 0;
            if (bitCount <= 8)
            {
                paletteEntries = BitConverter.ToInt32(resData, hotspotOffset + 32);
                if (paletteEntries == 0) paletteEntries = 1 << bitCount;
            }

            if (width <= 0 || height <= 0 || bitCount == 0)
            {
                Console.WriteLine("Invalid bitmap dimensions or bit count.");
                return RT_ICON.Get_ICON_Win1_Win2(resData);
                ModuleResources.DumpRaw(resData);
                return new Bitmap(1, 1);
            }

            long pixelDataOffset = hotspotOffset + biSize + paletteEntries * 4;
            long colorStride = ((width * bitCount + 31) / 32) * 4;
            long maskStride = ((width + 31) / 32) * 4;
            long maskDataOffset = pixelDataOffset + colorStride * height;

            long pixelDataLength = colorStride * height;
            long maskDataLength = maskStride * height;

            // Palette
            Color[] palette = null;
            if (bitCount <= 8)
            {
                palette = new Color[paletteEntries];
                int paletteStart = hotspotOffset + biSize;
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

            // Bitmap data
            byte[] pixelData = new byte[colorStride * height];
            byte[] maskData = new byte[maskStride * height];
            RT_BITMAP.CopyLarge(resData, pixelDataOffset, pixelData, 0, pixelDataLength);
            RT_BITMAP.CopyLarge(resData, maskDataOffset, maskData, 0, maskDataLength);

            return RT_BITMAP.GenerateBitmapFromData(pixelData, maskData, width, height, bitCount, palette);
        }
    }
}
