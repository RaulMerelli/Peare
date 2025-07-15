using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace Peare
{
    public static class RT_BITMAP
    {
        // This is an entrypoint also for RT_POINTER. A bitmap array can be the base for both the types.
        // This return as list of Bitmap for compatibility with OS/2 Bitmap Array
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
                        bool imageDecoded = false;
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

                        var bmp = Decode_BITMAP(bmpData);
                        if (bmp != null)
                        {
                            imageDecoded = true;
                            result.Add(bmp);
                        }

                        bmp = Decode_RT_POINTER(bmpData, resData);
                        if (bmp != null)
                        {
                            imageDecoded = true;
                            result.Add(bmp);
                        }

                        string status = imageDecoded ? "loaded successfully" : "failed to load";
                        Console.WriteLine($"Bitmap or Pointer {status}");

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
                    bool imageDecoded = false;
                    Bitmap bmp = Decode_BITMAP(resData);
                    if (bmp != null)
                    {
                        imageDecoded = true;
                        result.Add(bmp);
                    }

                    bmp = Decode_RT_POINTER(resData, resData);
                    if (bmp != null)
                    {
                        imageDecoded = true;
                        result.Add(bmp);
                    }

                    string status = imageDecoded ? "loaded successfully" : "failed to load";
                    Console.WriteLine($"Bitmap or Pointer {status}");

                    return result;
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show("Failed to convert bitmap data: " + ex.Message);
                return result;
            }
        }

        private static Bitmap Decode_BITMAP(byte[] resData)
        {
            try
            {
                Console.Write("\r\n\r\nData found:\r\n");
                Program.DumpRaw(resData);

                var bmpOS2 = Decode_BITMAP_OS2_V1(resData);
                if (bmpOS2 != null)
                    return bmpOS2;

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
                bmpOS2 = Decode_BITMAP_OS2_V1(resData);
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
                return null;
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseBMP failed: " + ex.Message);
                return null;
            }
        }

        private static Bitmap Decode_RT_POINTER(byte[] CIresData, byte[] bitmapArray)
        {
            using (var ms = new MemoryStream(CIresData))
            using (var reader = new BinaryReader(ms))
            {
                // === First block (mask) ===
                ushort header = reader.ReadUInt16();
                // "CI", "IC", "CP", "PT" are basically the same format, for a different use.
                // The main difference is that the bitmap data block might be missing and
                // there is only the first block (mask) that contains also the bitmap data.
                // IC icon (OS/2 1.x)
                // CI icon (OS/2 2.x+)
                // PT pointer
                if (header != 0x4943 && header != 0x4349 && header != 0x5043 && header != 0x5043 && header != 0x5450) 
                    return null; 
                _ = reader.ReadUInt32(); // fileSize
                ushort xHotspotMask = reader.ReadUInt16(); // xHotspot
                ushort yHotspotMask = reader.ReadUInt16(); // yHotspot
                uint bitmapOffsetMask = reader.ReadUInt32();

                if (reader.ReadUInt32() != 12) return null; // BitmapCoreHeader size
                ushort widthMask = reader.ReadUInt16();
                ushort heightMask = reader.ReadUInt16(); // This is double the height of the bitmap!
                ushort planesMask = reader.ReadUInt16(); // planes
                ushort bppMask = reader.ReadUInt16(); // bpp (should be 1)

                if (bppMask < 1)
                {
                    // Invalid bpp detected.
                    Console.WriteLine("Invalid bpp detected. bpp has manually set to 1.");
                    bppMask = 1;
                }

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

                // === Second block (color image) ===
                if (reader.BaseStream.Position + 2 <= reader.BaseStream.Length && reader.ReadUInt16() == header)
                {
                    // Second block found!
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

                    // === Calculate size ===
                    int stride = ((width * bpp + 31) / 32) * 4;
                    int strideMask = ((widthMask * bppMask + 31) / 32) * 4;

                    // We use the real weight, already given by the bitmap, not from the mask.
                    // From the mask it would be heightMask / 2
                    int bitmapSize = stride * height;
                    int maskSize = strideMask * height;

                    // === Read data from bitmapArray ===
                    if (bitmapOffsetMask + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];
                    if (bitmapOffset + bitmapSize > bitmapArray.Length)
                    {
                        // Out of offset. Must fallback to the mask as bitmap data.
                        Console.WriteLine("Invalid bitmapOffset detected. Using bitmapOffsetMask.");
                        Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    }
                    else
                    {
                        Array.Copy(bitmapArray, bitmapOffset, colorData, 0, bitmapSize);
                    }
                    Array.Copy(bitmapArray, bitmapOffsetMask + maskSize, maskData, 0, maskSize); // We skip the first half that we don't care about

                    return GenerateBitmapFromData(colorData, maskData, width, height, bpp, palette);
                }
                else
                {
                    // Second block not found.
                    // We use the bitmap data from the first half of mask data.

                    int stride = ((widthMask * bppMask + 31) / 32) * 4;
                    int maskStride = ((widthMask + 31) / 32) * 4; // 1bpp mask

                    int realHeight = heightMask / 2;
                    int bitmapSize = stride * realHeight;
                    int maskSize = maskStride * realHeight;

                    if (bitmapOffsetMask + bitmapSize + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];

                    Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    Array.Copy(bitmapArray, bitmapOffsetMask + bitmapSize, maskData, 0, maskSize);

                    return GenerateBitmapFromData(colorData, maskData, widthMask, realHeight, bppMask, paletteMask);
                }
            }
        }

        private static Bitmap Decode_BITMAP_OS2_V1(byte[] data)
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

                return GenerateBitmapFromData(bitmapData, null, width, height, bitCount, palette);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseOS2V1 failed: " + ex.Message);
                return null;
            }
        }

        public static Bitmap GenerateBitmapFromData(byte[] pixelData, byte[] maskData, int width, int height, int bitCount, Color[] palette)
        {
            // Unified function to decode the bitmap given the data.
            // It is made to work with everything, Windows Bitmaps, Icons, Cursors and OS/2 Bitmaps, Icons, Pointers etc.
            int colorStride = ((width * bitCount + 31) / 32) * 4;
            int maskStride = ((width + 31) / 32) * 4;

            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            BitmapData bmpData = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);

            bool applyMask = maskData != null && maskData.Length > 0;

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

                        // AND mask (optional)
                        if (applyMask)
                        {
                            int maskByteIndex = invY * maskStride + (x / 8);
                            int maskBit = 7 - (x % 8);
                            if (maskByteIndex < maskData.Length)
                            {
                                bool isTransparent = (maskData[maskByteIndex] & (1 << maskBit)) != 0;
                                if (isTransparent)
                                {
                                    color = Color.FromArgb(0, color.R, color.G, color.B);
                                }
                            }
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
    }
}
