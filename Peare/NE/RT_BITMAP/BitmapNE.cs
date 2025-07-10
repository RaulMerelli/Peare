using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Peare
{
    public static class BitmapNE
    {
        public static Bitmap Get(byte[] resData)
        {
            try
            {
                Console.WriteLine("[DEBUG] HEX DUMP:");
                for (int i = 0; i < Math.Min(64, resData.Length); i += 16)
                {
                    var chunk = resData.Skip(i).Take(16).ToArray();
                    string hex = string.Join(" ", chunk.Select(b => b.ToString("X2")));
                    string ascii = string.Concat(chunk.Select(b => b >= 32 && b <= 126 ? (char)b : '.'));
                    Console.WriteLine($"{i:X4}  {hex.PadRight(47)}  {ascii}");
                }

                // complete BMP (starts with 'BM')
                if (resData.Length >= 14 && resData[0] == 0x42 && resData[1] == 0x4D)
                {
                    uint bfOffBits = BitConverter.ToUInt32(resData, 10);
                    if (bfOffBits <= resData.Length)
                    {
                        try
                        {
                            using (MemoryStream ms = new MemoryStream(resData))
                                return new Bitmap(ms);
                        }
                        catch
                        {
                            Console.WriteLine("[DEBUG] BMP header present but GDI+ failed to load. Will fallback.");
                            // Fallthrough to DIB parsing
                            resData = resData.Skip(14).ToArray(); // Rimuovi BITMAPFILEHEADER
                        }
                    }
                    else
                    {
                        Console.WriteLine("[DEBUG] BMP signature found but bfOffBits invalid. Treating as DIB.");
                        resData = resData.Skip(14).ToArray(); // Rimuovi header corrotto
                    }
                }

                // Windows-style DIB (BITMAPINFOHEADER)
                if (resData.Length >= 40)
                {
                    int biSize = BitConverter.ToInt32(resData, 0);
                    if (biSize >= 40 && biSize <= resData.Length - 4)
                    {
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
                }

                // OS/2 1.x BITMAPCOREHEADER
                if (resData.Length >= 12)
                {
                    ushort bcSize = BitConverter.ToUInt16(resData, 0);
                    if (bcSize == 12)
                    {
                        ushort width = BitConverter.ToUInt16(resData, 2);
                        ushort height = BitConverter.ToUInt16(resData, 4);
                        ushort planes = BitConverter.ToUInt16(resData, 6);
                        ushort bitCount = BitConverter.ToUInt16(resData, 8);

                        int numColors = (bitCount <= 8) ? (1 << bitCount) : 0;
                        int paletteSize = numColors * 3;
                        int pixelOffset = 12 + paletteSize;

                        if (resData.Length < pixelOffset)
                            throw new Exception("Bitmap data too short.");

                        int biSize = 40;
                        int bfOffBits = 14 + biSize + numColors * 4;
                        int bfSize = bfOffBits + resData.Length - pixelOffset;

                        using (MemoryStream ms = new MemoryStream())
                        using (BinaryWriter bw = new BinaryWriter(ms))
                        {
                            bw.Write((ushort)0x4D42);     // 'BM'
                            bw.Write((uint)bfSize);       // Total size
                            bw.Write((ushort)0);          // Reserved1
                            bw.Write((ushort)0);          // Reserved2
                            bw.Write((uint)bfOffBits);    // Offset to pixel data

                            // BITMAPINFOHEADER (40 bytes)
                            bw.Write((uint)biSize);
                            bw.Write((int)width);
                            bw.Write((int)height);
                            bw.Write((ushort)planes);
                            bw.Write((ushort)bitCount);
                            bw.Write((uint)0);            // Compression = BI_RGB
                            bw.Write((uint)(resData.Length - pixelOffset));
                            bw.Write((int)0);
                            bw.Write((int)0);
                            bw.Write((uint)numColors);
                            bw.Write((uint)numColors);

                            // Palette: RGBTRIPLE → RGBQUAD
                            for (int i = 0; i < numColors; i++)
                            {
                                byte blue = resData[12 + i * 3];
                                byte green = resData[12 + i * 3 + 1];
                                byte red = resData[12 + i * 3 + 2];
                                bw.Write(red);
                                bw.Write(green);
                                bw.Write(blue);
                                bw.Write((byte)0);
                            }

                            // Pixel data
                            bw.Write(resData, pixelOffset, resData.Length - pixelOffset);

                            ms.Position = 0;
                            return new Bitmap(ms);
                        }
                    }
                }

                throw new NotSupportedException("Bitmap format not recognized.");
            }
            catch (Exception ex)
            {
                MessageBox.Show("Failed to convert bitmap data: " + ex.Message);
                return null;
            }
        }

    }
}
