using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace Peare
{
    public static class RT_MESSAGE
    {
        // --- Structures for Windows Message Table ---
        // These are essential and MUST accurately reflect the WinAPI definitions.
        // LayoutKind.Sequential is crucial for correct memory alignment.

        // MESSAGE_RESOURCE_HEADER is part of MESSAGE_RESOURCE_DATA
        [StructLayout(LayoutKind.Sequential)]
        public struct MESSAGE_RESOURCE_HEADER
        {
            public uint NumberOfBlocks; // Number of MESSAGE_RESOURCE_BLOCK structures
                                        // Followed by MESSAGE_RESOURCE_BLOCKs
        }

        // MESSAGE_RESOURCE_BLOCK defines a range of message IDs and their offset
        [StructLayout(LayoutKind.Sequential)]
        public struct MESSAGE_RESOURCE_BLOCK
        {
            public uint LowId;          // Lowest message ID in this block
            public uint HighId;         // Highest message ID in this block
            public uint OffsetToEntries; // Offset from the start of MESSAGE_RESOURCE_DATA to the first MESSAGE_RESOURCE_ENTRY for this block
        }

        // MESSAGE_RESOURCE_ENTRY is not explicitly a separate struct in the file,
        // but its components are always a short Length, short Flags, and then the text.
        // The 'length' field in MESSAGE_RESOURCE_ENTRY includes the 4-byte header (Length + Flags)
        // AND the string content AND its 2-byte null terminator.

        public static string Get(byte[] data)
        {
            ushort cp = 20127; // Default ASCII

            int offset = 0; // Current read position in the 'data' array
            if (Program.isOS2)
            {
                // First two bytes in OS/2 are the codepage
                cp = RT_STRING.ReadUInt16(data, ref offset);
            }

            StringBuilder output = new StringBuilder();
            output.AppendLine("MESSAGETABLE");
            output.AppendLine("{");

            // We need to manage a memory pointer for Marshal operations,
            // as data is a managed byte array.
            IntPtr unmanagedDataPtr = IntPtr.Zero;

            try
            {
                // Allocate unmanaged memory and copy the entire byte array to it.
                // This allows Marshal.PtrToStructure to work directly.
                unmanagedDataPtr = Marshal.AllocHGlobal(data.Length);
                Marshal.Copy(data, 0, unmanagedDataPtr, data.Length);

                // Current pointer for reading through the unmanaged memory block
                IntPtr currentUnmanagedPtr = unmanagedDataPtr;

                while (offset + 1 < data.Length) // This loop condition might need adjustment based on full resource parsing
                {
                    int msgId; // Will be the actual message ID from MESSAGE_RESOURCE_BLOCK/ENTRY
                    if (Program.currentHeaderType.StartsWith("PE"))
                    {
                        // --- Windows PE (RT_MESSAGETABLE) Parsing Logic ---

                        // The first thing in the MESSAGE_RESOURCE_DATA is MESSAGE_RESOURCE_HEADER.NumberOfBlocks
                        // Read NumberOfBlocks (assuming offset is at the beginning of the MESSAGE_RESOURCE_DATA)
                        uint numberOfBlocks = (uint)Marshal.ReadInt32(currentUnmanagedPtr, offset);
                        offset += Marshal.SizeOf<uint>(); // Advance offset past NumberOfBlocks

                        // The blocks array starts immediately after NumberOfBlocks
                        IntPtr blockPtr = IntPtr.Add(currentUnmanagedPtr, offset);
                        int blockSize = Marshal.SizeOf<MESSAGE_RESOURCE_BLOCK>();

                        for (int i = 0; i < numberOfBlocks; i++)
                        {
                            // Read the current MESSAGE_RESOURCE_BLOCK
                            MESSAGE_RESOURCE_BLOCK block = Marshal.PtrToStructure<MESSAGE_RESOURCE_BLOCK>(blockPtr);

                            // The OffsetToEntries is relative to the start of the MESSAGE_RESOURCE_DATA
                            // So, entryPtr points to the beginning of the MESSAGE_RESOURCE_ENTRY for this block
                            IntPtr entryPtr = IntPtr.Add(unmanagedDataPtr, (int)block.OffsetToEntries);

                            for (uint id = block.LowId; id <= block.HighId; id++)
                            {
                                // Read MESSAGE_RESOURCE_ENTRY fields
                                short entryLength = Marshal.ReadInt16(entryPtr); // Total length of this entry (including its header and string with null terminator)
                                short flags = Marshal.ReadInt16(entryPtr, 2);   // Flags (0 = ANSI, 1 = Unicode)

                                // The actual string data starts 4 bytes after the beginning of the entry
                                IntPtr textPtr = IntPtr.Add(entryPtr, 4);

                                string message = "ERROR: Could not decode message."; // Default error message

                                // Determine encoding based on flags
                                if (flags == 0) // ANSI (single-byte characters, double-byte null terminator)
                                {
                                    // Marshal.PtrToStringAnsi will read until the first 0x00.
                                    // The string content is single-byte, followed by 0x00 0x00.
                                    // PtrToStringAnsi correctly handles the 0x00 as a terminator.
                                    message = Marshal.PtrToStringAnsi(textPtr);
                                }
                                else if (flags == 1) // Unicode (UTF-16LE characters, double-byte null terminator)
                                {
                                    // Marshal.PtrToStringUni will read until the first 0x00 0x00.
                                    // This is the standard for Unicode strings in Windows resources.
                                    message = Marshal.PtrToStringUni(textPtr);
                                }
                                else
                                {
                                    // Handle unknown flags if necessary, this might indicate corrupted data
                                    message = $"UNKNOWN_FLAGS_{flags}_FOR_ID_{id}";
                                }

                                // Clean up the message text: remove carriage returns and line feeds
                                message = message.Replace("\r\n", "");

                                output.AppendLine($"\t0x{id:X4}, \"{message}\"");

                                // Advance to the next MESSAGE_RESOURCE_ENTRY using its total length
                                entryPtr = IntPtr.Add(entryPtr, entryLength);
                            }
                            // Advance to the next MESSAGE_RESOURCE_BLOCK
                            blockPtr = IntPtr.Add(blockPtr, blockSize);
                        }
                        // Once all blocks are processed, we should have read the entire Message Table.
                        // Break out of the outer while loop as we've finished processing this PE resource.
                        offset = data.Length; // Force exit from outer while loop
                        break; // Exit the loop for PE processing
                    }
                    else // Existing non-PE logic
                    {
                        msgId = RT_STRING.ReadByte(data, ref offset);
                        string message = RT_STRING.ReadNullTerminatedString(data, ref offset, cp);
                        output.AppendLine($"\t0x{msgId:X4}, \"{message}\"");
                    }
                }
            }
            finally
            {
                // Crucially, free the unmanaged memory when done.
                if (unmanagedDataPtr != IntPtr.Zero)
                {
                    Marshal.FreeHGlobal(unmanagedDataPtr);
                }
            }

            output.AppendLine("}");
            return output.ToString();
        }
    }
}
