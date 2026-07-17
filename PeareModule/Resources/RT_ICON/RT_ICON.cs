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
            if (resData == null || resData.Length < 14)
                return EmptyImage();

            Img legacyImage;
            if (Win12MonochromeResource.TryDecode(
                resData,
                0,
                out legacyImage))
            {
                return legacyImage;
            }

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

            if (resData.Length < 16)
                return EmptyImage();

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
            Img image;
            if (Win12MonochromeResource.TryDecode(
                resData,
                0,
                out image))
            {
                return image;
            }

            Console.WriteLine("Error: data is not a valid Windows 1.x/2.x icon resource.");
            return EmptyImage();
        }

        private static Img EmptyImage()
        {
            return new Img
            {
                BitCount = 0,
                Size = new Size(0, 0),
                Bitmap = new Bitmap(1, 1, PixelFormat.Format32bppArgb)
            };
        }
    }
}
