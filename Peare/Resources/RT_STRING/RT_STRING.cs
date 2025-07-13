using System;
using System.Text;

namespace Peare
{
    public static class RT_STRING
    {
        public static string Get(byte[] data, int baseId = 100)
        {
            if (data == null || data.Length == 0)
                return string.Empty;

            var sb = new StringBuilder();
            sb.AppendLine("STRINGTABLE");
            sb.AppendLine("{");

            int offset = 0;
            if (Program.isOS2)
            {
                // I found out the for NE OS/2 and LX there are two bytes unknown for me. Just skipping them seems to be fine.
                // So far I found B501h for NE OS/2 and 5203h for LX
                offset = 2;  // skip the first two bytes for OS/2
            }

            int currentId = -1;  // -1 means "first one not found yet"

            for (int i = 0; i < 16; i++)
            {
                if (offset >= data.Length)
                    break;

                byte length = data[offset++];
                if (length == 0)
                    continue;

                if (offset + length > data.Length)
                {
                    sb.AppendLine($"  // ERROR: incomplete string data at index {i}");
                    break;
                }

                string value = Encoding.ASCII.GetString(data, offset, length).TrimEnd('\0');
                offset += length;

                if (currentId == -1)
                    currentId = baseId;  // first non-empty ID

                sb.AppendLine($"\t{currentId}, \"{Escape(value)}\"");

                currentId++;
            }

            sb.AppendLine("}");
            return sb.ToString();
        }

        private static string Escape(string s)
        {
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
