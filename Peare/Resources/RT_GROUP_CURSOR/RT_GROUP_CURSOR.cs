using System;
using System.Text;

namespace Peare
{
    public class RT_GROUP_CURSOR
    {
        // This structure is binary compatible with RT_GROUP_ICON but is not the same.
        // I tried to apply the structure CURSORICONINFO as described in the book Undocumented Windows, 1992, Andrew Schulman, David Maxey and Matt Pietrek
        // It seems like it is the one that makes more sense, but still the final result data looks like it's made up, could be also I'm using it wrong or it is the wrong struct... 
        public static string Get(byte[] data)
        {
            if (data == null || data.Length < 14)
                return "Invalid data";

            StringBuilder sb = new StringBuilder();

            // CURSORDIR header
            ushort idReserved = BitConverter.ToUInt16(data, 0);
            ushort idType = BitConverter.ToUInt16(data, 2);
            ushort idCount = BitConverter.ToUInt16(data, 4);

            sb.AppendLine("RT_GROUP_CURSOR");
            sb.AppendLine("{");
            sb.AppendLine($"\tReserved: {idReserved}");
            sb.AppendLine($"\tType: {idType} (2 = Cursor)");
            sb.AppendLine($"\tCount: {idCount}");

            int offset = 2;

            for (int i = 0; i < idCount; i++)
            {
                if (offset + 14 > data.Length)
                {
                    sb.AppendLine($"  [!] GRPCURSORDIRENTRY {i} incomplete.");
                    break;
                }

                // Interpret CURSORICONINFO from offset
                ushort hotspotX = BitConverter.ToUInt16(data, offset);
                ushort hotspotY = BitConverter.ToUInt16(data, offset + 2);
                ushort width = BitConverter.ToUInt16(data, offset + 4);
                ushort height = BitConverter.ToUInt16(data, offset + 6);
                ushort widthBytes = BitConverter.ToUInt16(data, offset + 8);
                byte planes = data[offset + 10];
                byte bitsPerPixel = data[offset + 11];
                uint dwBytesInRes = BitConverter.ToUInt32(data, offset + 12);
                ushort nID = BitConverter.ToUInt16(data, offset + 16);

                sb.AppendLine($"\tRT_CURSOR #{nID}");
                sb.AppendLine("\t{");
                sb.AppendLine($"\t\tSize: {width}x{height} px");
                sb.AppendLine($"\t\tHotspot: ({hotspotX}, {hotspotY})");
                sb.AppendLine($"\t\tWidthBytes: {widthBytes}");
                sb.AppendLine($"\t\tPlanes: {planes}");
                sb.AppendLine($"\t\tBitsPerPixel: {bitsPerPixel}");
                sb.AppendLine($"\t\tBytes in Resource: {dwBytesInRes}");
                sb.AppendLine("\t}");

                offset += 18; // total size read per entry
            }

            sb.AppendLine("}");

            return sb.ToString();
        }
    }
}
