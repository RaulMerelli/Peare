using System;
using System.Text;
using System.Runtime.InteropServices;

namespace PeareModule
{
    public static class RT_NAMETABLE
    {
        [StructLayout(LayoutKind.Sequential, Pack = 1)]
        private struct RTNameTableHeader
        {
            public ushort lengthEntry;    // WORD (2 bytes)
            public ushort resourceType;   // WORD (2 bytes)
            public ushort resourceId;     // WORD (2 bytes)
            public byte paddingZero;      // BYTE (1 byte)
                                          // CHAR szName[]; follows immediately
        }

        public static string Get(byte[] data)
        {
            if (data == null || data.Length == 0)
            {
                // We'll just return an empty string or a header for context.
                return "RT_NAMETABLE\n{\n}";
            }

            StringBuilder resultBuilder = new StringBuilder();
            resultBuilder.AppendLine("RT_NAMETABLE");
            resultBuilder.AppendLine("{");

            int offset = 0;
            int headerSize = Marshal.SizeOf(typeof(RTNameTableHeader)); // Should be 7 bytes

            while (offset < data.Length)
            {
                if (offset + headerSize > data.Length)
                {
                    break;
                }

                GCHandle handle = GCHandle.Alloc(data, GCHandleType.Pinned);
                IntPtr ptr = handle.AddrOfPinnedObject() + offset;

                RTNameTableHeader header;
                try
                {
                    header = (RTNameTableHeader)Marshal.PtrToStructure(ptr, typeof(RTNameTableHeader));
                }
                finally
                {
                    if (handle.IsAllocated)
                    {
                        handle.Free();
                    }
                }

                ushort lengthEntry = header.lengthEntry;
                ushort resourceType = header.resourceType;
                ushort resourceIdRaw = header.resourceId; // Keep the raw value for comment
                byte paddingZero = header.paddingZero;

                // Validate the padding byte
                if (paddingZero != 0x00)
                {
                    resultBuilder.AppendLine($"// WARNING: Expected padding byte to be 0x00 at offset 0x{offset + 6:X4}, but found 0x{paddingZero:X2}.");
                }

                if (lengthEntry < headerSize)
                {
                    break;
                }

                if (offset + lengthEntry > data.Length)
                {
                    resultBuilder.AppendLine($"// ERROR: Declared length 0x{lengthEntry:X4} ({lengthEntry} bytes) at offset 0x{offset:X4} exceeds available data ({data.Length - offset} bytes remaining). Truncated entry detected. Parsing what's available.");
                    lengthEntry = (ushort)(data.Length - offset); // Adjust lengthEntry to parse what's available
                }

                int stringStartIndex = offset + headerSize;
                int stringEndIndex = -1;

                for (int i = stringStartIndex; i < offset + lengthEntry; i++)
                {
                    if (data[i] == 0x00)
                    {
                        stringEndIndex = i;
                        break;
                    }
                }

                string decodedName = string.Empty;
                if (stringEndIndex != -1)
                {
                    int stringLength = stringEndIndex - stringStartIndex;
                    if (stringLength >= 0)
                    {
                        decodedName = Encoding.ASCII.GetString(data, stringStartIndex, stringLength);
                    }
                }
                else
                {
                    if (stringStartIndex < offset + lengthEntry)
                    {
                        int stringLength = (offset + lengthEntry) - stringStartIndex;
                        if (stringLength > 0)
                        {
                            decodedName = Encoding.ASCII.GetString(data, stringStartIndex, stringLength).TrimEnd('\0');
                        }
                    }
                }

                // The resourceId contains both the ordinal and the 0x8000 flag.
                // For naming, the ordinal likely refers to its position in the NAMETABLE itself,
                // not necessarily the final resource ID.
                ushort ordinalId = (ushort)(resourceIdRaw & ~0x8000); // Clear the 0x8000 bit

                // Determine a human-readable resource type from the constant
                string typeDescription;

                if (NeResources.WindowsNeResourceTypes.TryGetValue(resourceType, out string name))
                    typeDescription = name;
                else
                    typeDescription = $"#{resourceType}";

                resultBuilder.AppendLine($"  {typeDescription} #{ordinalId} = \"{decodedName}\"");

                offset += lengthEntry;
            }

            resultBuilder.AppendLine("}");
            return resultBuilder.ToString();
        }
    }
}