using System;
using System.Drawing;
using System.Drawing.Imaging;

namespace Peare
{
    public static class IconNE
    {
        public static Bitmap Get(byte[] resData)
        {
            int biSize = BitConverter.ToInt32(resData, 0);
            int width = BitConverter.ToInt32(resData, 4);
            int fullHeight = BitConverter.ToInt32(resData, 8);
            int height = fullHeight / 2;  // half for pixel data, half for transparency AND mask
            ushort planes = BitConverter.ToUInt16(resData, 12);
            ushort bitCount = BitConverter.ToUInt16(resData, 14);

            // Palette count (if bitCount <= 8), calculate how many entries (4 bytes each)
            int paletteEntries = 0;
            if (bitCount <= 8)
            {
                // In BITMAPINFOHEADER, the color count field is at offset 32 (int)
                paletteEntries = BitConverter.ToInt32(resData, 32);
                if (paletteEntries == 0) paletteEntries = 1 << bitCount;
            }

            // Offset where pixel color data starts
            int pixelDataOffset = biSize + paletteEntries * 4;

            // Calculate stride (aligned to 4 bytes)
            int colorStride = ((width * bitCount + 31) / 32) * 4;
            int maskStride = ((width + 31) / 32) * 4;

            // Offset of the AND mask
            int maskDataOffset = pixelDataOffset + colorStride * height;

            // Create the final bitmap in 32-bit ARGB format
            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);

            // If using palette, extract it
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
                    // fourth byte is reserved?
                    palette[j] = Color.FromArgb(255, r, g, b);
                }
            }

            // LockBits for fast memory access
            BitmapData bmpData = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
            unsafe
            {
                byte* ptr = (byte*)bmpData.Scan0;

                for (int y = 0; y < height; y++)
                {
                    int invY = height - 1 - y; // DIB bitmap is bottom-up

                    for (int x = 0; x < width; x++)
                    {
                        Color color = Color.Magenta; // default error color

                        // Read color pixel
                        int pixelOffset = pixelDataOffset + invY * colorStride;
                        if (bitCount == 32)
                        {
                            int off = pixelOffset + x * 4;
                            if (off + 3 < resData.Length)
                            {
                                byte b = resData[off];
                                byte g = resData[off + 1];
                                byte r = resData[off + 2];
                                byte a = resData[off + 3];
                                color = Color.FromArgb(a, r, g, b);
                            }
                        }
                        else if (bitCount == 24)
                        {
                            int off = pixelOffset + x * 3;
                            if (off + 2 < resData.Length)
                            {
                                byte b = resData[off];
                                byte g = resData[off + 1];
                                byte r = resData[off + 2];
                                color = Color.FromArgb(255, r, g, b);
                            }
                        }
                        else if (bitCount == 8)
                        {
                            int off = pixelOffset + x;
                            if (off < resData.Length && palette != null && palette.Length > 0)
                            {
                                int idx = resData[off];
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 4)
                        {
                            int off = pixelOffset + (x / 2);
                            if (off < resData.Length && palette != null && palette.Length > 0)
                            {
                                byte value = resData[off];
                                int idx = (x % 2 == 0) ? (value >> 4) : (value & 0x0F);
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 1)
                        {
                            int off = pixelOffset + (x / 8);
                            if (off < resData.Length && palette != null && palette.Length > 0)
                            {
                                byte value = resData[off];
                                int bitIndex = 7 - (x % 8);
                                int idx = (value >> bitIndex) & 1;
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else
                        {
                            // bitCount not supported
                            color = Color.Magenta;
                        }

                        // Read AND mask bit (1 bit per pixel, bit set = transparent)
                        int maskByteIndex = maskDataOffset + invY * maskStride + (x / 8);
                        int maskBit = 7 - (x % 8);
                        bool isTransparent = false;
                        if (maskByteIndex < resData.Length)
                        {
                            isTransparent = (resData[maskByteIndex] & (1 << maskBit)) != 0;
                        }

                        if (isTransparent)
                        {
                            color = Color.FromArgb(0, color.R, color.G, color.B);
                        }

                        // Write pixel to bitmap
                        byte* pixelPtr = ptr + y * bmpData.Stride + x * 4;
                        pixelPtr[0] = color.B;
                        pixelPtr[1] = color.G;
                        pixelPtr[2] = color.R;
                        pixelPtr[3] = color.A;
                    }
                }
            }

            bmp.UnlockBits(bmpData);
            return bmp;
        }
    }
}
