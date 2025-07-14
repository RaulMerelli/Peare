using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using static System.Windows.Forms.VisualStyles.VisualStyleElement.ProgressBar;

namespace Peare
{
    public static class RT_BITMAP
    {
        // This is an entrypoint also for RT_POINTER. A bitmap array can be the base for both the types.
        public static List<Bitmap> Get(byte[] resData)
        {
            var result = new List<Bitmap>();

            try
            {
                // Check for OS/2 BITMAPARRAYHEADER (starts with 'BA')
                if (resData.Length >= 2 && resData[0] == 0x42 && resData[1] == 0x41) // 'BA'
                {
                    int offset = 0;

                    while (offset < resData.Length)
                    {
                        if (offset + 14 > resData.Length) break;

                        if (resData[offset] != 0x42 || resData[offset + 1] != 0x41)
                        {
                            Console.WriteLine($"[DEBUG] Invalid 'BA' at offset {offset}. Aborting.");
                            break;
                        }

                        int headerSize = BitConverter.ToInt32(resData, offset + 2);
                        int nextOffset = BitConverter.ToInt32(resData, offset + 6);
                        int bmpOffset = offset + 14; // + headerSize;

                        if (bmpOffset >= resData.Length)
                        {
                            Console.WriteLine($"[DEBUG] BMP data offset {bmpOffset} beyond end of data.");
                            break;
                        }

                        int nextBmpSize = (nextOffset > 0 && nextOffset > offset)
                            ? nextOffset - bmpOffset
                            : resData.Length - bmpOffset;

                        byte[] bmpData = resData.Skip(bmpOffset).Take(nextBmpSize).ToArray();

                        var bmp = TryParseBMP(bmpData);
                        if (bmp != null)
                            result.Add(bmp);

                        bmp = TryParseCI(bmpData, resData);
                        if (bmp != null)
                            result.Add(bmp);

                        if (nextOffset <= offset || nextOffset >= resData.Length)
                        {
                            break;
                        }

                        offset = nextOffset;
                    }

                    return result;
                }
                else
                {
                    Bitmap bmp = TryParseBMP(resData);
                    if (bmp != null)
                        result.Add(bmp);

                    bmp = TryParseCI(resData, resData);
                    if (bmp != null)
                        result.Add(bmp);
                    return result;
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Failed to convert bitmap data: " + ex.Message);
                return result;
            }
        }

        private static Bitmap TryParseBMP(byte[] resData)
        {
            try
            {
                Program.DumpRaw(resData);

                var bmpOS2 = TryParseOS2V1(resData);
                if (bmpOS2 != null)
                    return bmpOS2;

                Console.WriteLine("[DEBUG] TryParseOS2V1 failed.");

                // Check if it's a full BMP file (starts with 'BM')
                if (resData.Length >= 14 && resData[0] == 0x42 && resData[1] == 0x4D)
                {
                    uint bfOffBits = BitConverter.ToUInt32(resData, 10);

                    if (bfOffBits <= resData.Length)
                    {
                        // Valid bfOffBits, try loading with GDI+
                        try
                        {
                            using (MemoryStream ms = new MemoryStream(resData))
                                return new Bitmap(ms);
                        }
                        catch
                        {
                            Console.WriteLine("[DEBUG] BMP header present but GDI+ failed to load. Will fallback.");
                            // Remove file header and proceed with DIB
                            resData = resData.Skip(14).ToArray();
                        }
                    }
                    else
                    {
                        Console.WriteLine("[DEBUG] BMP signature found but bfOffBits invalid. Treating as DIB.");
                        // Invalid bfOffBits, remove file header and proceed
                        resData = resData.Skip(14).ToArray();
                    }
                }

                // Try again after removing the BMP signature
                bmpOS2 = TryParseOS2V1(resData);
                if (bmpOS2 != null)
                    return bmpOS2;

                // Windows-style DIB (BITMAPINFOHEADER)
                try
                {
                    int biSize = BitConverter.ToInt32(resData, 0);
                    int bfOffBits = 14 + biSize;
                    int bfSize = bfOffBits + resData.Length - biSize;

                    using (MemoryStream ms = new MemoryStream())
                    using (BinaryWriter bw = new BinaryWriter(ms))
                    {
                        bw.Write((ushort)0x4D42); // 'BM'
                        bw.Write((uint)bfSize);
                        bw.Write((ushort)0);
                        bw.Write((ushort)0);
                        bw.Write((uint)bfOffBits);
                        bw.Write(resData);
                        ms.Position = 0;
                        return new Bitmap(ms);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Reading as Windows-style DIB (BITMAPINFOHEADER) failed");
                }

                throw new NotSupportedException("Bitmap format not recognized.");
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseSingle failed: " + ex.Message);
                return null;
            }
        }

        private static Bitmap TryParseCI(byte[] CIresData, byte[] bitmapArray)
        {
            using (var ms = new MemoryStream(CIresData))
            using (var reader = new BinaryReader(ms))
            {
                // === First CI block (monochrome mask) ===
                ushort header = reader.ReadUInt16();
                if (header != 0x4943 && header != 0x4349) return null; // "CI" or "IC"
                _ = reader.ReadUInt32(); // fileSize
                ushort xHotspotMask = reader.ReadUInt16(); // xHotspot
                ushort yHotspotMask = reader.ReadUInt16(); // yHotspot
                uint bitmapOffsetMask = reader.ReadUInt32();

                if (reader.ReadUInt32() != 12) return null; // BitmapCoreHeader size
                ushort widthMask = reader.ReadUInt16();
                ushort heightMask = reader.ReadUInt16(); // This is double the height of the bitmap!
                ushort planesMask = reader.ReadUInt16(); // planes
                ushort bppMask = reader.ReadUInt16(); // bpp (should be 1)

                if (planesMask != 1 || (bppMask != 1 && bppMask != 4 && bppMask != 8))
                    return null;

                int numColorsMask = 1 << bppMask;

                // --- COLOR PALETTE ---
                var paletteMask = new Color[numColorsMask];
                for (int i = 0; i < numColorsMask; i++)
                {
                    byte blue = reader.ReadByte();
                    byte green = reader.ReadByte();
                    byte red = reader.ReadByte();
                    paletteMask[i] = Color.FromArgb(red, green, blue);
                }

                // === Second CI block (color image) ===
                if (reader.BaseStream.Position + 2 <= reader.BaseStream.Length && reader.ReadUInt16() == 0x4943)
                {
                    // Second "CI" block found!
                    _ = reader.ReadUInt32(); // fileSize
                    ushort xHotspot = reader.ReadUInt16();
                    ushort yHotspot = reader.ReadUInt16();
                    uint bitmapOffset = reader.ReadUInt32();

                    if (reader.ReadUInt32() != 12) return null;
                    ushort width = reader.ReadUInt16();
                    ushort height = reader.ReadUInt16();
                    ushort planes = reader.ReadUInt16();
                    ushort bpp = reader.ReadUInt16();

                    if (planes != 1 || (bpp != 1 && bpp != 4 && bpp != 8))
                        return null;

                    int numColors = 1 << bpp;
                    var palette = new Color[numColors];
                    for (int i = 0; i < numColors; i++)
                    {
                        byte blue = reader.ReadByte();
                        byte green = reader.ReadByte();
                        byte red = reader.ReadByte();
                        palette[i] = Color.FromArgb(red, green, blue);
                    }

                    // === Calcola dimensioni ===
                    int stride = ((width * bpp + 31) / 32) * 4;
                    int strideMask = ((widthMask * bppMask + 31) / 32) * 4;

                    // We use the real weight, already given by the bitmap, not from the mask.
                    // From the mask it would be heightMask / 2
                    int bitmapSize = stride * height;
                    int maskSize = strideMask * height;

                    // === Legge i dati dal bitmapArray ===
                    if (bitmapOffset + bitmapSize > bitmapArray.Length ||
                        bitmapOffsetMask + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];

                    Array.Copy(bitmapArray, bitmapOffset, colorData, 0, bitmapSize);
                    Array.Copy(bitmapArray, bitmapOffsetMask + maskSize, maskData, 0, maskSize); // We skip the first half that we don't care about

                    return GenerateBitmapFromData(colorData, maskData, width, height, bpp, palette);
                }
                else
                {
                    // Second "CI" block not found.
                    // Fallback
                    // I think we might find a case where we have a single CI block, handled as monochrome only, where the first half of the mask is also the bitmap
                    // This fallback is also compatible with IC file, that apparently is the same to the CI with a single block.
                    // I found the IC format being used in OS/2 1.1 for example.

                    int stride = ((widthMask * bppMask + 31) / 32) * 4;
                    int maskStride = ((widthMask + 31) / 32) * 4; // 1bpp mask

                    int realHeight = heightMask / 2;
                    int bitmapSize = stride * realHeight;
                    int maskSize = maskStride * realHeight;

                    if (bitmapOffsetMask + bitmapSize + maskSize > bitmapArray.Length)
                        return null;

                    byte[] raw = new byte[bitmapSize];
                    byte[] rawmask = new byte[maskSize];

                    Array.Copy(bitmapArray, bitmapOffsetMask, raw, 0, bitmapSize);
                    Array.Copy(bitmapArray, bitmapOffsetMask + bitmapSize, rawmask, 0, maskSize);

                    return GenerateBitmapFromData(raw, rawmask, widthMask, realHeight, bppMask, paletteMask);
                }
            }
        }

        private static Bitmap TryParseOS2V1(byte[] data)
        {
            try
            {
                // OS/2 V1 header is 12 bytes long
                if (data.Length < 12)
                    return null;

                ushort bcSize = BitConverter.ToUInt16(data, 0);
                if (bcSize != 12)
                    return null;

                ushort width = BitConverter.ToUInt16(data, 2);
                ushort height = BitConverter.ToUInt16(data, 4);
                ushort planes = BitConverter.ToUInt16(data, 6);
                ushort bitCount = BitConverter.ToUInt16(data, 8);

                if (planes != 1 || (bitCount != 1 && bitCount != 4 && bitCount != 8))
                {
                    Console.WriteLine("[DEBUG] Unsupported OS/2 v1 planes or BPP: planes={0}, bpp={1}", planes, bitCount);
                    return null;
                }

                int numColors = 1 << bitCount;
                int paletteOffset = 12;
                int paletteSize = numColors * 3;

                if (data.Length < paletteOffset + paletteSize)
                {
                    Console.WriteLine("[DEBUG] Data too short for palette");
                    return null;
                }

                Color[] palette = new Color[numColors];
                for (int i = 0; i < numColors; i++)
                {
                    byte b = data[paletteOffset + i * 3 + 0];
                    byte g = data[paletteOffset + i * 3 + 1];
                    byte r = data[paletteOffset + i * 3 + 2];
                    palette[i] = Color.FromArgb(r, g, b);
                }

                int pixelOffset = paletteOffset + paletteSize;

                int bitsPerLine = width * bitCount;
                int stride = ((bitsPerLine + 31) / 32) * 4;

                int totalPixelBytes = stride * height;
                if (data.Length < pixelOffset + totalPixelBytes)
                {
                    Console.WriteLine("[DEBUG] Data too short for pixel rows");
                    return null;
                }

                // Estraggo solo la porzione bitmap "pulita" da passare
                byte[] bitmapData = new byte[totalPixelBytes];
                Array.Copy(data, pixelOffset, bitmapData, 0, totalPixelBytes);

                return CreateBitmapFromData(bitmapData, width, height, bitCount, palette);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseOS2V1 failed: " + ex.Message);
                return null;
            }
        }

        public static Bitmap GenerateBitmapFromData(byte[] pixelData, byte[] maskData, int width, int height, int bitCount, Color[] palette)
        {
            int colorStride = ((width * bitCount + 31) / 32) * 4;
            int maskStride = ((width + 31) / 32) * 4;

            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            BitmapData bmpData = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);

            unsafe
            {
                byte* ptr = (byte*)bmpData.Scan0;

                for (int y = 0; y < height; y++)
                {
                    int invY = height - 1 - y;

                    for (int x = 0; x < width; x++)
                    {
                        Color color = Color.Magenta;

                        int pixelOffset = invY * colorStride;
                        if (bitCount == 32)
                        {
                            int off = pixelOffset + x * 4;
                            if (off + 3 < pixelData.Length)
                            {
                                byte b = pixelData[off];
                                byte g = pixelData[off + 1];
                                byte r = pixelData[off + 2];
                                byte a = pixelData[off + 3];
                                color = Color.FromArgb(a, r, g, b);
                            }
                        }
                        else if (bitCount == 24)
                        {
                            int off = pixelOffset + x * 3;
                            if (off + 2 < pixelData.Length)
                            {
                                byte b = pixelData[off];
                                byte g = pixelData[off + 1];
                                byte r = pixelData[off + 2];
                                color = Color.FromArgb(255, r, g, b);
                            }
                        }
                        else if (bitCount == 8 && palette != null)
                        {
                            int off = pixelOffset + x;
                            if (off < pixelData.Length)
                            {
                                int idx = pixelData[off];
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 4 && palette != null)
                        {
                            int off = pixelOffset + (x / 2);
                            if (off < pixelData.Length)
                            {
                                byte val = pixelData[off];
                                int idx = (x % 2 == 0) ? (val >> 4) : (val & 0x0F);
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 1 && palette != null)
                        {
                            int off = pixelOffset + (x / 8);
                            if (off < pixelData.Length)
                            {
                                byte val = pixelData[off];
                                int idx = (val >> (7 - (x % 8))) & 1;
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }

                        // AND mask
                        int maskByteIndex = invY * maskStride + (x / 8);
                        int maskBit = 7 - (x % 8);
                        bool isTransparent = false;
                        if (maskByteIndex < maskData.Length)
                        {
                            isTransparent = (maskData[maskByteIndex] & (1 << maskBit)) != 0;
                        }

                        if (isTransparent)
                        {
                            color = Color.FromArgb(0, color.R, color.G, color.B);
                        }

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

        private static Bitmap CreateBitmapFromData(byte[] bitmapData, int width, int height, ushort bitCount, Color[] palette)
        {
            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);

            int bitsPerLine = width * bitCount;
            int stride = ((bitsPerLine + 31) / 32) * 4;

            for (int y = 0; y < height; y++)
            {
                int rowOffset = (height - 1 - y) * stride;

                for (int x = 0; x < width; x++)
                {
                    int paletteIndex = 0;

                    switch (bitCount)
                    {
                        case 8:
                            paletteIndex = bitmapData[rowOffset + x];
                            break;

                        case 4:
                            {
                                int byteIndex = rowOffset + (x / 2);
                                byte b = bitmapData[byteIndex];
                                paletteIndex = (x % 2 == 0) ? (b >> 4) : (b & 0x0F);
                            }
                            break;

                        case 1:
                            {
                                int byteIndex = rowOffset + (x / 8);
                                byte b = bitmapData[byteIndex];
                                int bit = 7 - (x % 8);
                                paletteIndex = (b >> bit) & 0x01;
                            }
                            break;
                    }

                    if (paletteIndex >= 0 && paletteIndex < palette.Length)
                        bmp.SetPixel(x, y, palette[paletteIndex]);
                }
            }

            return bmp;
        }


    }
}
