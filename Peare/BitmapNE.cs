using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Peare
{
    public static class BitmapNE
    {
        public static Bitmap Get(byte[] resData)
        {
            Bitmap bmp;
            int biSize = BitConverter.ToInt32(resData, 0);
            int bfOffBits = 14 + biSize; // offset to pixel data = file header + info header
            int bfSize = bfOffBits + resData.Length - biSize;

            using (MemoryStream ms = new MemoryStream())
            using (BinaryWriter bw = new BinaryWriter(ms))
            {
                // Write BITMAPFILEHEADER (14 bytes)
                bw.Write((ushort)0x4D42);         // 'BM'
                bw.Write((uint)bfSize);           // Total file size
                bw.Write((ushort)0);              // reserved1
                bw.Write((ushort)0);              // reserved2
                bw.Write((uint)bfOffBits);        // offset to pixel data

                // Write the rest of the data (already includes BITMAPINFOHEADER + pixels)
                bw.Write(resData);

                ms.Position = 0;
                bmp = new Bitmap(ms);
            }
            return bmp;
        }
    }
}
