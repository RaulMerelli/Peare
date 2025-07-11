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

                        var bmp = TryParseSingle(bmpData);
                        if (bmp != null)
                        {
                            result.Add(bmp);
                        }
                        else
                        {
                            Console.WriteLine($"[DEBUG] Failed to parse bitmap at offset {offset}");
                        }

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
                    Bitmap bmp = TryParseSingle(resData);
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
        private static Bitmap TryParseSingle(byte[] resData)
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

                Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);

                for (int y = 0; y < height; y++)
                {
                    int rowOffset = pixelOffset + (height - 1 - y) * stride;

                    for (int x = 0; x < width; x++)
                    {
                        int paletteIndex = 0;

                        switch (bitCount)
                        {
                            case 8:
                                paletteIndex = data[rowOffset + x];
                                break;

                            case 4:
                                {
                                    int byteIndex = rowOffset + (x / 2);
                                    byte b = data[byteIndex];
                                    paletteIndex = (x % 2 == 0) ? (b >> 4) : (b & 0x0F);
                                }
                                break;

                            case 1:
                                {
                                    int byteIndex = rowOffset + (x / 8);
                                    byte b = data[byteIndex];
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
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseOS2V1 failed: " + ex.Message);
                return null;
            }
        }

    }
}
