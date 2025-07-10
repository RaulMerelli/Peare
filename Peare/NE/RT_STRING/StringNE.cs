using System;
using System.Text;

namespace Peare
{
    public static class StringNE
    {
        public static string Get(byte[] data, int baseId = 100)
        {
            if (data == null || data.Length == 0)
                return string.Empty;

            var sb = new StringBuilder();
            sb.AppendLine("STRINGTABLE");
            sb.AppendLine("{");

            int offset = 0;
            if (data.Length >= 2 && data[0] == 0xB5 && data[1] == 0x01)
            {
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

                sb.AppendLine($" {currentId}, \"{Escape(value)}\"");

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
