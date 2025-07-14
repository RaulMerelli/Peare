using System;
using System.Text;

namespace Peare
{
    public class RT_GROUP_ICON
    {
        public static string Get(byte[] data)
        {
            if (data == null || data.Length < 6)
                return "Invalid data";

            StringBuilder sb = new StringBuilder();

            // ICONDIR
            ushort idReserved = BitConverter.ToUInt16(data, 0); // 0x00
            ushort idType = BitConverter.ToUInt16(data, 2);     // 0x01
            ushort idCount = BitConverter.ToUInt16(data, 4);    // 0x02

            sb.AppendLine($"RT_GROUP_ICON");
            sb.AppendLine("{");
            sb.AppendLine($"\tReserved: {idReserved}");
            sb.AppendLine($"\tType: {idType} (1 = Icon)");
            sb.AppendLine($"\tCount: {idCount}");

            int offset = 6;

            for (int i = 0; i < idCount; i++)
            {
                if (offset + 14 > data.Length)
                {
                    sb.AppendLine($"  [!] GRPICONDIRENTRY {i} incomplete.");
                    break;
                }

                byte bWidth = data[offset];
                byte bHeight = data[offset + 1];
                byte bColorCount = data[offset + 2];
                byte bReserved = data[offset + 3];
                ushort wPlanes = BitConverter.ToUInt16(data, offset + 4);
                ushort wBitCount = BitConverter.ToUInt16(data, offset + 6);
                uint dwBytesInRes = BitConverter.ToUInt32(data, offset + 8);
                ushort nID = BitConverter.ToUInt16(data, offset + 12);

                sb.AppendLine($"\tRT_ICON #{nID}");
                sb.AppendLine("\t{");
                sb.AppendLine($"\t\tSize: {bWidth}x{bHeight} px");
                sb.AppendLine($"\t\tColors: {(bColorCount == 0 ? ">8bpp" : bColorCount.ToString())}");
                sb.AppendLine($"\t\tPlanes: {wPlanes}");
                sb.AppendLine($"\t\tBitCount: {wBitCount}");
                sb.AppendLine($"\t\tBytes in Resource: {dwBytesInRes}");
                sb.AppendLine("\t}");

                offset += 14;
            }

            sb.AppendLine("}");

            return sb.ToString();
        }
    }
}
