using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace Peare
{
    public static class RT_FONTDIR
    {
        public static unsafe string Get(byte[] data)
        {
            if (data == null || data.Length < 2)
                return string.Empty;

            var sb = new StringBuilder();

            int offset = 0;

            ushort count = BitConverter.ToUInt16(data, offset);
            offset += 2;

            sb.AppendLine($"Font count: {count}");

            for (int i = 0; i < count; i++)
            {
                if (offset + 2 > data.Length)
                {
                    sb.AppendLine("Unexpected end of data reading ordinal.");
                    break;
                }

                ushort ordinal = BitConverter.ToUInt16(data, offset);
                offset += 2;

                int structSize = 0x71; // fixed size of FONTDIRENTRY
                if (offset + structSize > data.Length)
                {
                    sb.AppendLine("Unexpected end of data reading FONTDIRENTRY struct.");
                    break;
                }

                byte[] structBytes = new byte[structSize];
                Array.Copy(data, offset, structBytes, 0, structSize);
                offset += structSize;

                FONTDIRENTRY entry = Program.Deserialize<FONTDIRENTRY>(structBytes);

                // Convert copyright from byte[] to ASCII string
                string copyright = Encoding.ASCII.GetString(entry.dfCopyright)
                    .TrimEnd('\0')
                    .Replace('\0', '\n');

                // Now read szDeviceName (null-terminated string)
                string deviceName = ReadNullTerminatedString(data, ref offset)
                    .TrimEnd('\0')
                    .Replace('\0', '\n');

                // Then szFaceName (null-terminated string), right after szDeviceName
                string faceName = ReadNullTerminatedString(data, ref offset)
                    .TrimEnd('\0')
                    .Replace('\0', '\n');

                sb.AppendLine($"RT_FONT #{ordinal}:");
                sb.AppendLine("{");
                sb.AppendLine($"\tVersion: {entry.dfVersion}");
                sb.AppendLine($"\tSize: {entry.dfSize}");
                sb.AppendLine($"\tCopyright: {copyright}");
                sb.AppendLine($"\tType: {entry.dfType}");
                sb.AppendLine($"\tPoints: {entry.dfPoints}");
                sb.AppendLine($"\tVertRes: {entry.dfVertRes}");
                sb.AppendLine($"\tHorizRes: {entry.dfHorizRes}");
                sb.AppendLine($"\tAscent: {entry.dfAscent}");
                sb.AppendLine($"\tInternalLeading: {entry.dfInternalLeading}");
                sb.AppendLine($"\tExternalLeading: {entry.dfExternalLeading}");
                sb.AppendLine($"\tItalic: {entry.dfItalic}");
                sb.AppendLine($"\tUnderline: {entry.dfUnderline}");
                sb.AppendLine($"\tStrikeOut: {entry.dfStrikeOut}");
                sb.AppendLine($"\tWeight: {entry.dfWeight}");
                sb.AppendLine($"\tCharSet: {entry.dfCharSet}");
                sb.AppendLine($"\tPixWidth: {entry.dfPixWidth}");
                sb.AppendLine($"\tPixHeight: {entry.dfPixHeight}");
                sb.AppendLine($"\tPitchAndFamily: {entry.dfPitchAndFamily}");
                sb.AppendLine($"\tAvgWidth: {entry.dfAvgWidth}");
                sb.AppendLine($"\tMaxWidth: {entry.dfMaxWidth}");
                sb.AppendLine($"\tFirstChar: {entry.dfFirstChar}");
                sb.AppendLine($"\tLastChar: {entry.dfLastChar}");
                sb.AppendLine($"\tDefaultChar: {entry.dfDefaultChar}");
                sb.AppendLine($"\tBreakChar: {entry.dfBreakChar}");
                sb.AppendLine($"\tWidthBytes: {entry.dfWidthBytes}");
                sb.AppendLine($"\tDevice: 0x{entry.dfDevice:X8}");
                sb.AppendLine($"\tFace: 0x{entry.dfFace:X8}");
                sb.AppendLine($"\tReserved: 0x{entry.dfReserved:X8}");
                sb.AppendLine($"\tDeviceName: {deviceName}");
                sb.AppendLine($"\tFaceName: {faceName}");
                sb.AppendLine("}");
                sb.AppendLine();
            }

            return sb.ToString();
        }

        private static string ReadNullTerminatedString(byte[] data, ref int offset)
        {
            int start = offset;
            while (offset < data.Length && data[offset] != 0)
                offset++;
            string result = Encoding.ASCII.GetString(data, start, offset - start);
            offset++; // skip the null terminator
            return result;
        }
    }
}
