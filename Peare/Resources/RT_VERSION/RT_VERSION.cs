using System;
using System.Text;
using System.Runtime.InteropServices;

namespace Peare
{
    public static class RT_VERSION
    {
        // The VS_HEADER structure is simplified to include only fields common to all blocks.
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

            bool isUnicode = IsUnicode(data, 6); // Offset 6 is where "VS_VERSION_INFO" should be
            if (isUnicode)
                offset += 2; // Unicode version has additional fields we're not interested in
            string rootKey = isUnicode ? ReadUnicodeZ(data, ref offset) : ReadAsciiZ(data, ref offset);
            Align4(ref offset);

            if (rootHeader.wValueLength > 0)
            {
                offset += rootHeader.wValueLength;
                Align4(ref offset);
            }

            // Start parsing the child blocks of the root
            ParseAndDump(data, ref offset, rootStart + rootHeader.wLength, sb, 1, isUnicode);

            sb.AppendLine("}");
            return sb.ToString();
        }

        private static void ParseAndDump(byte[] data, ref int offset, int parentEndOffset, StringBuilder sb, int indent, bool isUnicode)
        {
            while (offset < parentEndOffset && offset + 4 <= data.Length)
            {
                int currentBlockStart = offset;
                VS_HEADER header = ReadHeader(data, ref offset);

                if (header.wLength == 0 || currentBlockStart + header.wLength > parentEndOffset)
                    break; // Invalid block or one that exceeds the parent's boundary

                if (isUnicode)
                    offset += 2; // Skip two bytes when unicode

                string key = isUnicode ? ReadUnicodeZ(data, ref offset) : ReadAsciiZ(data, ref offset);
                Align4(ref offset); // Align after reading the key

                string indentStr = new string(' ', indent * 2);
                string value = null;

                // Handle the block's value
                if (header.wValueLength > 0)
                {
                    if (isUnicode)
                    {
                        if (indent == 3)
                        {
                            // ReadUnicodeZ reads the full value until the double null terminator and advances the offset
                            value = ReadUnicodeZ(data, ref offset);
                        }
                        else if (key == "Translation" && header.wValueLength >= 4)
                        {
                            // Special case for Translation (binary value)
                            ushort langID = BitConverter.ToUInt16(data, offset);
                            ushort codePage = BitConverter.ToUInt16(data, offset + 2);
                            value = $"{langID} {codePage}";
                            offset += header.wValueLength;
                        }
                    }
                    else // ASCII
                    {
                        if (indent == 3)
                        {
                            // ReadAsciiZ will find the null terminator (0x00) and advance the offset
                            value = ReadAsciiZ(data, ref offset);
                        }
                        else if (key == "Translation" && header.wValueLength >= 4)
                        {
                            // Special case for Translation (binary value)
                            ushort langID = BitConverter.ToUInt16(data, offset);
                            ushort codePage = BitConverter.ToUInt16(data, offset + 2);
                            value = $"{langID} {codePage}";
                            offset += header.wValueLength;
                        }
                    }

                    Align4(ref offset); // Align offset after reading (or skipping) the value
                }

                // Print the key and the value if present
                if (value != null)
                {
                    sb.AppendLine($"{indentStr}{key} = \"{Escape(value)}\"");
                }
                else
                {
                    sb.AppendLine($"{indentStr}{key}");
                }

                if (header.wValueLength == 0 && offset < currentBlockStart + header.wLength)
                {
                    sb.AppendLine($"{indentStr}{{");
                    // Recursively parse child blocks until the end of the current parent block
                    ParseAndDump(data, ref offset, currentBlockStart + header.wLength, sb, indent + 1, isUnicode);
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
            // Advance offset until a null terminator (0x00) is found or the end of data is reached.
            while (offset < data.Length && data[offset] != 0)
                offset++;

            int length = offset - start;
            string s = "";
            if (length > 0)
            {
                // Decode the ASCII string from the byte array.
                s = Encoding.ASCII.GetString(data, start, length);
            }

            // Skip the null terminator if it exists.
            if (offset < data.Length && data[offset] == 0)
                offset++;

            return s;
        }

        private static string ReadUnicodeZ(byte[] data, ref int offset)
        {
            int start = offset;
            // Advance offset by 2 bytes at a time (for UTF-16 characters) until a double null terminator (0x00 0x00) is found or the end of data is reached.
            while (offset + 1 < data.Length && !(data[offset] == 0 && data[offset + 1] == 0))
                offset += 2;

            int length = offset - start;
            string s = "";
            if (length > 0)
            {
                // Use the standard Unicode encoding (UTF-16 Little Endian).
                // If there are unmappable characters (e.g., problematic codepoints),
                // it will use the U+FFFD replacement character (the square box).
                s = Encoding.Unicode.GetString(data, start, length);

                // Now, replace the U+FFFD replacement character with a space.
                s = s.Replace('\uFFFD', ' ');
            }

            // Skip the double null terminator if it exists.
            if (offset + 1 < data.Length && data[offset] == 0 && data[offset + 1] == 0)
                offset += 2;

            return s;
        }

        private static bool IsUnicode(byte[] data, int offset)
        {
            // Unicode representation of "VS_VERSION"
            byte[] unicodeVersion = Encoding.Unicode.GetBytes("VS_VERSION");

            if (offset + unicodeVersion.Length > data.Length)
                return false;

            for (int i = 0; i < unicodeVersion.Length; i++)
            {
                if (data[offset + i] != unicodeVersion[i])
                    return false;
            }

            return true;
        }

        private static void Align4(ref int offset)
        {
            offset = (offset + 3) & ~3;
        }

        private static string Escape(string s)
        {
            return s.Replace("\\", "\\\\")
                     .Replace("\"", "\\\"")
                     .Replace("\t", "\\t")
                     .Replace("\n", "\\n")
                     .Replace("\r", "\\r");
        }
    }
}