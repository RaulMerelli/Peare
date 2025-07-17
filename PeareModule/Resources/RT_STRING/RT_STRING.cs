using System;
using System.Drawing;
using System.IO;
using System.Text;

namespace PeareModule
{
    public static class RT_STRING
    {
        public static byte ReadByte(byte[] data, ref int offset)
        {
            if (offset + 1 > data.Length)
            {
                throw new EndOfStreamException("Attempted to read past end of data.");
            }
            return data[offset++];
        }

        public static ushort ReadUInt16(byte[] data, ref int offset)
        {
            if (offset + 2 > data.Length)
            {
                throw new EndOfStreamException("Attempted to read past end of data for ushort.");
            }
            ushort value = BitConverter.ToUInt16(data, offset);
            offset += 2;
            return value;
        }

        private static string ReadLenString(byte[] data, ref int offset, int codepage, int len)
        {
            Encoding encoding = Encoding.GetEncoding(codepage);
            Decoder decoder = encoding.GetDecoder();

            char[] chars = new char[len];
            int bytesAvailable = data.Length - offset;

            int bytesUsed, charsUsed;
            bool completed;

            decoder.Convert(
                data, offset, bytesAvailable,
                chars, 0, len,
                flush: false,
                out bytesUsed, out charsUsed, out completed);

            if (charsUsed != len)
            {
                throw new EndOfStreamException("Not enough data to decode the requested number of characters.");
            }

            string result = new string(chars);
            offset += bytesUsed;
            return result;
        }

        public static string ReadNullTerminatedString(byte[] data, ref int offset, int codepage)
        {
            Encoding encoding = Encoding.GetEncoding(codepage);
            // Determine the null terminator for the given encoding.
            // For most single-byte encodings, this is 0x00.
            // For UTF-16, it's 0x00 0x00.
            // This is a simplification; a robust solution might need a more complex way to get the null terminator.
            byte[] nullTerminatorBytes = encoding.GetBytes(new char[] { '\0' });

            using (MemoryStream ms = new MemoryStream())
            {
                int startIndex = offset;
                bool foundNull = false;

                while (offset < data.Length)
                {
                    // Check if the current byte(s) match the null terminator sequence
                    bool possibleNull = true;
                    if (offset + nullTerminatorBytes.Length <= data.Length)
                    {
                        for (int i = 0; i < nullTerminatorBytes.Length; i++)
                        {
                            if (data[offset + i] != nullTerminatorBytes[i])
                            {
                                possibleNull = false;
                                break;
                            }
                        }
                    }
                    else
                    {
                        possibleNull = false; // Not enough bytes left to be the null terminator
                    }

                    if (possibleNull)
                    {
                        foundNull = true;
                        offset += nullTerminatorBytes.Length; // Consume the null terminator
                        break;
                    }

                    // If not null, write the current byte to the MemoryStream
                    ms.WriteByte(data[offset]);
                    offset++;
                }

                if (!foundNull)
                {
                    // If we reached the end of the data without finding a null terminator,
                    // you might want to throw an exception or handle it differently
                    // depending on your expected behavior (e.g., return the string read so far).
                    throw new EndOfStreamException("Null-terminated string not found in the provided data.");
                }

                // Convert the bytes read (excluding the null terminator) into a string
                byte[] stringBytes = ms.ToArray();
                return encoding.GetString(stringBytes);
            }
        }

        public static string Get(byte[] data, ModuleResources.ModuleProperties properties, int baseId = 100)
        {
            ushort cp = 20127; // Default ASCII

            if (properties.headerType == ModuleResources.HeaderType.PE)
            {
                cp = 1200;
            }

            if (data == null || data.Length == 0)
                return string.Empty;

            var sb = new StringBuilder();
            sb.AppendLine("STRINGTABLE");
            sb.AppendLine("{");

            int offset = 0;

            if ((properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                properties.headerType == ModuleResources.HeaderType.LX)
            {
                // First two bytes in OS/2 are the codepage
                cp = ReadUInt16(data, ref offset);
            }

            int currentId = -1;  // -1 means "first one not found yet"

            while (offset < data.Length)
            {
                int length;
                if (properties.headerType == ModuleResources.HeaderType.PE)
                {
                    length = ReadUInt16(data, ref offset);
                }
                else
                {
                    length = ReadByte(data, ref offset);
                }
                if (length == 0)
                    continue;

                if (offset + length > data.Length)
                {
                    sb.AppendLine($"  // ERROR: incomplete string data");
                    break;
                }

                string value = ReadLenString(data, ref offset, cp, length).TrimEnd('\0');

                if (currentId == -1)
                    currentId = baseId;  // first non-empty ID

                if (!string.IsNullOrEmpty(value))
                {
                    sb.AppendLine($"\t{currentId}, \"{Escape(value)}\"");

                    currentId++;
                }
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
