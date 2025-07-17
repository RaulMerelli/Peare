using System;
using System.Collections.Generic;
using System.Drawing;
using System.Text;

namespace PeareModule
{
    public class RT_GROUP_CURSOR
    {
        public static string Get(byte[] data)
        {
            // This function only reads the single RT_GROUP_CURSOR resource without accessing the single RT_CURSOR resources.
            return Get(data, null, out _);
        }

        public static string Get(byte[] data, ModuleResources.ModuleProperties properties, out List<Bitmap> bitmaps)
        {
            bitmaps = new List<Bitmap>();

            if (data == null || data.Length < 6)
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

            int offset = 6;

            for (int i = 0; i < idCount; i++)
            {
                if (offset + 14 > data.Length)
                {
                    sb.AppendLine("\tInvalid entry (truncated data)");
                    break;
                }
                // This is not what is documented, but it's made to get a result similar to Resource Hacker.
                byte size = data[offset];
                byte reserved1 = data[offset + 1];
                byte colorCount = data[offset + 2];
                byte reserved2 = data[offset + 3];
                ushort hotspotX = BitConverter.ToUInt16(data, offset + 4);
                ushort hotspotY = BitConverter.ToUInt16(data, offset + 6);
                uint bytesInRes = BitConverter.ToUInt32(data, offset + 8);
                ushort nID = BitConverter.ToUInt16(data, offset + 12);

                sb.AppendLine($"\tRT_CURSOR #{nID}");
                sb.AppendLine("\t{");
                sb.AppendLine($"\t\tWidth: {size}");
                sb.AppendLine($"\t\tHeight: {size}");
                sb.AppendLine($"\t\tReserved1: {reserved1}");
                sb.AppendLine($"\t\tColorCount: {colorCount / size}");
                sb.AppendLine($"\t\tReserved2: {reserved2}");
                sb.AppendLine($"\t\tHotspotX: {hotspotX}");
                sb.AppendLine($"\t\tHotspotY: {hotspotY}");
                sb.AppendLine($"\t\tBytesInRes: {bytesInRes}");
                sb.AppendLine("\t}");
                try
                {
                    if (properties != null)
                    {
                        bitmaps.Add(RT_CURSOR.Get(
                            ModuleResources.OpenResource(properties,
                            "RT_CURSOR",
                            nID.ToString(),
                            out _,
                            out _)));
                    }
                }
                catch (Exception err)
                {
                    Console.WriteLine(err.ToString());
                }
                offset += 14;
            }

            sb.AppendLine("}");

            return sb.ToString();
        }
    }
}
