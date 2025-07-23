using System;
using System.Text;

namespace PeareModule
{
    public static class RT_HELPTABLE
    {
        // Special thanks to EDM2.com for providing how this resource is structured.
        // The format is described at "when rt = 18" (search for it in the page)
        // https://www.edm2.com/index.php/Resources_and_Decompiling_Them
        public static string Get(byte[] data)
        {
            StringBuilder sb = new StringBuilder();

            sb.AppendLine("HELPTABLE");
            sb.AppendLine("{");

            // Check for null or insufficient data.
            // Each help item requires 8 bytes (wnd, sub, separator, ext, each 16 bits).
            if (data == null || data.Length < 8)
            {
                sb.AppendLine("}");
                return sb.ToString();
            }

            // Iterate through the byte array, processing 8 bytes at a time for each help item.
            // The loop condition 'i + 7 < data.Length' ensures that we only attempt to read
            // a full 8-byte block, preventing IndexOutOfRangeException for incomplete items
            // at the end of the data array.
            for (int i = 0; i + 7 < data.Length; i += 8)
            {
                ushort wnd = BitConverter.ToUInt16(data, i);     // application window ID
                ushort sub = BitConverter.ToUInt16(data, i + 2); // help subtable ID
                ushort ext = BitConverter.ToUInt16(data, i + 6); // extended help panel ID
                sb.AppendLine($"    {wnd}, {sub}, {ext}");
            }

            sb.AppendLine("}");
            return sb.ToString();
        }
    }
}
