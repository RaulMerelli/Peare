using System;
using System.Text;

namespace Peare
{
    public static class RT_VERSION
    {
        private struct VS_HEADER
        {
            public ushort wLength;
            public ushort wValueLength;
        }

        public static string Get(byte[] data)
        {
            if (data == null || data.Length < 4)
                return "VERSIONINFO\n{\n}";

            int offset = 0;
            var sb = new StringBuilder();
            sb.AppendLine("VERSIONINFO");
            sb.AppendLine("{");

            int rootStart = offset;
            VS_HEADER rootHeader = ReadHeader(data, ref offset);

            string rootKey = ReadAsciiZ(data, ref offset);
            Align4(ref offset);

            if (rootHeader.wValueLength > 0)
            {
                offset += rootHeader.wValueLength;
                Align4(ref offset);
            }

            ParseAndDump(data, ref offset, rootStart + rootHeader.wLength, sb, 1);

            sb.AppendLine("}");
            return sb.ToString();
        }

        private static void ParseAndDump(byte[] data, ref int offset, int parentEndOffset, StringBuilder sb, int indent)
        {
            while (offset < parentEndOffset && offset + 4 <= data.Length)
            {
                int currentBlockStart = offset;
                VS_HEADER header = ReadHeader(data, ref offset);

                if (header.wLength == 0 || currentBlockStart + header.wLength > parentEndOffset)
                    break;

                string key = ReadAsciiZ(data, ref offset);
                Align4(ref offset);

                string indentStr = new string(' ', indent * 2);
                string value = null;
                bool isStringEntry = (indent == 3);

                if (header.wValueLength > 0)
                {
                    if (key == "Translation" && header.wValueLength >= 4)
                    {
                        ushort langID = BitConverter.ToUInt16(data, offset);
                        ushort codePage = BitConverter.ToUInt16(data, offset + 2);
                        value = $"{langID} {codePage}";
                        offset += header.wValueLength;
                    }
                    else if (isStringEntry)
                    {
                        value = Encoding.ASCII.GetString(data, offset, header.wValueLength);
                        value = value.TrimEnd('\0');
                        offset += header.wValueLength;
                    }
                    else
                    {
                        offset += header.wValueLength;
                    }
                    Align4(ref offset);
                }

                if (value != null)
                {
                    sb.AppendLine($"{indentStr}{key} = \"{Escape(value)}\"");
                }
                else
                {
                    sb.AppendLine($"{indentStr}{key}");
                }

                if (offset < currentBlockStart + header.wLength)
                {
                    sb.AppendLine($"{indentStr}{{");
                    ParseAndDump(data, ref offset, currentBlockStart + header.wLength, sb, indent + 1);
                    sb.AppendLine($"{indentStr}}}");
                }

                offset = currentBlockStart + header.wLength;
                Align4(ref offset);
            }
        }

        private static VS_HEADER ReadHeader(byte[] data, ref int offset)
        {
            VS_HEADER header = new VS_HEADER
            {
                wLength = BitConverter.ToUInt16(data, offset),
                wValueLength = BitConverter.ToUInt16(data, offset + 2),
            };
            offset += 4;
            return header;
        }

        private static string ReadAsciiZ(byte[] data, ref int offset)
        {
            int start = offset;
            while (offset < data.Length && data[offset] != 0)
                offset++;

            int length = offset - start;
            string s = "";
            if (length > 0)
            {
                s = Encoding.ASCII.GetString(data, start, length);
            }

            if (offset < data.Length && data[offset] == 0)
                offset++;

            return s;
        }

        private static void Align4(ref int offset)
        {
            offset = (offset + 3) & ~3;
        }

        private static string Escape(string s)
        {
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"").Replace("\t", "\\t").Replace("\n", "\\n").Replace("\r", "\\r");
        }
    }
}
