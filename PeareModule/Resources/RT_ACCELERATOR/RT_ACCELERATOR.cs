using System;
using System.IO;
using System.Text;
using System.Collections.Generic;

namespace PeareModule
{
    public class RT_ACCELERATOR
    {
        // ACCELTABLEENTRY flags as defined in Windows SDK (winuser.h)
        private const ushort FVIRTKEY = 0x0001;   // The wAnsi member specifies a virtual-key code.
        private const ushort FNOINVERT = 0x0002;  // Prevents highlighting of the menu item when the accelerator is used.
        private const ushort FSHIFT = 0x0004;     // The SHIFT key must be held down.
        private const ushort FCONTROL = 0x0008;   // The CTRL key must be held down.
        private const ushort FALT = 0x0010;       // The ALT key must be held down.
        private const ushort FLAST = 0x0080;      // Indicates the last entry in the accelerator table.

        // A mapping of common virtual key codes to their string representations for RC files.
        // This helps in generating human-readable key names like VK_RETURN, VK_F5.
        public static readonly Dictionary<ushort, string> VirtualKeyCodeMap = new Dictionary<ushort, string>
        {
            { 0x01, "VK_LBUTTON" },    { 0x02, "VK_RBUTTON" },    { 0x03, "VK_CANCEL" },
            { 0x04, "VK_MBUTTON" },    { 0x05, "VK_XBUTTON1" },   { 0x06, "VK_XBUTTON2" },
            { 0x08, "VK_BACK" },       { 0x09, "VK_TAB" },
            { 0x0C, "VK_CLEAR" },      { 0x0D, "VK_RETURN" },
            { 0x10, "VK_SHIFT" },      { 0x11, "VK_CONTROL" },    { 0x12, "VK_MENU" },
            { 0x13, "VK_PAUSE" },      { 0x14, "VK_CAPITAL" },
            { 0x1B, "VK_ESCAPE" },
            { 0x20, "VK_SPACE" },      { 0x21, "VK_PRIOR" },      { 0x22, "VK_NEXT" },
            { 0x23, "VK_END" },        { 0x24, "VK_HOME" },       { 0x25, "VK_LEFT" },
            { 0x26, "VK_UP" },         { 0x27, "VK_RIGHT" },      { 0x28, "VK_DOWN" },
            { 0x29, "VK_SELECT" },     { 0x2A, "VK_PRINT" },      { 0x2B, "VK_EXECUTE" },
            { 0x2C, "VK_SNAPSHOT" },   { 0x2D, "VK_INSERT" },     { 0x2E, "VK_DELETE" },
            { 0x2F, "VK_HELP" },
            { 0x30, "VK_0" }, { 0x31, "VK_1" }, { 0x32, "VK_2" }, { 0x33, "VK_3" }, { 0x34, "VK_4" },
            { 0x35, "VK_5" }, { 0x36, "VK_6" }, { 0x37, "VK_7" }, { 0x38, "VK_8" }, { 0x39, "VK_9" },
            { 0x41, "VK_A" }, { 0x42, "VK_B" }, { 0x43, "VK_C" }, { 0x44, "VK_D" }, { 0x45, "VK_E" },
            { 0x46, "VK_F" }, { 0x47, "VK_G" }, { 0x48, "VK_H" }, { 0x49, "VK_I" }, { 0x4A, "VK_J" },
            { 0x4B, "VK_K" }, { 0x4C, "VK_L" }, { 0x4D, "VK_M" }, { 0x4E, "VK_N" }, { 0x4F, "VK_O" },
            { 0x50, "VK_P" }, { 0x51, "VK_Q" }, { 0x52, "VK_R" }, { 0x53, "VK_S" }, { 0x54, "VK_T" },
            { 0x55, "VK_U" }, { 0x56, "VK_V" }, { 0x57, "VK_W" }, { 0x58, "VK_X" }, { 0x59, "VK_Y" },
            { 0x5A, "VK_Z" },
            { 0x5B, "VK_LWIN" }, { 0x5C, "VK_RWIN" }, { 0x5D, "VK_APPS" },
            { 0x60, "VK_NUMPAD0" }, { 0x61, "VK_NUMPAD1" }, { 0x62, "VK_NUMPAD2" },
            { 0x63, "VK_NUMPAD3" }, { 0x64, "VK_NUMPAD4" }, { 0x65, "VK_NUMPAD5" },
            { 0x66, "VK_NUMPAD6" }, { 0x67, "VK_NUMPAD7" }, { 0x68, "VK_NUMPAD8" },
            { 0x69, "VK_NUMPAD9" }, { 0x6A, "VK_MULTIPLY" }, { 0x6B, "VK_ADD" },
            { 0x6C, "VK_SEPARATOR" }, { 0x6D, "VK_SUBTRACT" }, { 0x6E, "VK_DECIMAL" },
            { 0x6F, "VK_DIVIDE" },
            { 0x70, "VK_F1" }, { 0x71, "VK_F2" }, { 0x72, "VK_F3" }, { 0x73, "VK_F4" },
            { 0x74, "VK_F5" }, { 0x75, "VK_F6" }, { 0x76, "VK_F7" }, { 0x77, "VK_F8" },
            { 0x78, "VK_F9" }, { 0x79, "VK_F10" }, { 0x7A, "VK_F11" }, { 0x7B, "VK_F12" },
            { 0x7C, "VK_F13" }, { 0x7D, "VK_F14" }, { 0x7E, "VK_F15" }, { 0x7F, "VK_F16" },
            { 0x80, "VK_F17" }, { 0x81, "VK_F18" }, { 0x82, "VK_F19" }, { 0x83, "VK_F20" },
            { 0x84, "VK_F21" }, { 0x85, "VK_F22" }, { 0x86, "VK_F23" }, { 0x87, "VK_F24" },
            { 0x90, "VK_NUMLOCK" }, { 0x91, "VK_SCROLL" },
            { 0xA0, "VK_LSHIFT" }, { 0xA1, "VK_RSHIFT" }, { 0xA2, "VK_LCONTROL" },
            { 0xA3, "VK_RCONTROL" }, { 0xA4, "VK_LMENU" }, { 0xA5, "VK_RMENU" },
            { 0xF6, "VK_ATTN" }, { 0xF7, "VK_CRSEL" }, { 0xF8, "VK_EXSEL" },
            { 0xF9, "VK_EREOF" }, { 0xFA, "VK_PLAY" }, { 0xFB, "VK_ZOOM" },
            { 0xFC, "VK_NONAME" }, { 0xFD, "VK_PA1" }, { 0xFE, "VK_OEM_CLEAR" },
            // Extra IME keys if needed
            { 0x18, "VK_FINAL" }, { 0x19, "VK_KANJI" }
        };

        public static string Get(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            // Handle empty or null input data gracefully.
            if (resData == null || resData.Length == 0)
            {
                return "// No accelerator data provided or data is empty.";
            }

            if ((properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                properties.headerType == ModuleResources.HeaderType.LX)
            {
                return RT_ACCELTABLE.Get(resData, properties);
            }

                StringBuilder rcContent = new StringBuilder();
            // Standard RC file header for an accelerator table.
            // We use a generic name 'IDR_ACCELERATOR1' as the actual resource ID/name is not in the byte data itself.
            rcContent.AppendLine("ACCELERATORS");
            rcContent.AppendLine("{");

            // Use MemoryStream and BinaryReader to easily read structured binary data.
            using (MemoryStream ms = new MemoryStream(resData))
            using (BinaryReader reader = new BinaryReader(ms))
            {
                try
                {
                    bool isLastEntry = false;
                    // Loop through the byte array, reading 6 bytes per ACCELTABLEENTRY,
                    // and then skipping 2 bytes of padding, until the FLAST flag is encountered or end of stream is reached.
                    while (!isLastEntry && reader.BaseStream.Position < reader.BaseStream.Length)
                    {
                        // Ensure there are enough bytes for a complete ACCELTABLEENTRY (6 bytes).
                        if (reader.BaseStream.Length - reader.BaseStream.Position < 6)
                        {
                            rcContent.AppendLine("// Warning: Incomplete accelerator entry detected at end of stream.");
                            break; // Not enough bytes for a full entry, break the loop.
                        }

                        // Read the three WORD (ushort) members of the ACCELTABLEENTRY structure.
                        // Based on the provided dump and Resource Hacker's output, the order of wID and wAnsiOrVirtualKey
                        // appears to be swapped relative to the standard definition, and values are in lower byte.
                        ushort fFlags = reader.ReadUInt16();          // Accelerator flags
                        ushort rawVirtualKey = reader.ReadUInt16();   // This is interpreted as the Virtual Key code
                        ushort rawID = reader.ReadUInt16();           // This is interpreted as the Command ID

                        // Check if this is the last entry in the table.
                        isLastEntry = (fFlags & FLAST) != 0;

                        // After reading 6 bytes, check if there are at least 2 more bytes for padding.
                        // If so, consume them. This assumes a consistent 8-byte record length in the dump.
                        // This also handles the case where the very last entry might not have padding.
                        if (reader.BaseStream.Position + 2 <= reader.BaseStream.Length)
                        {
                            reader.ReadUInt16(); // Skip 2 bytes of padding
                        }

                        // Build a list of RC-specific flags based on the fFlags value.
                        List<string> rcFlags = new List<string>();
                        if ((fFlags & FNOINVERT) != 0) rcFlags.Add("NOINVERT");
                        if ((fFlags & FSHIFT) != 0) rcFlags.Add("SHIFT");
                        if ((fFlags & FCONTROL) != 0) rcFlags.Add("CONTROL");
                        if ((fFlags & FALT) != 0) rcFlags.Add("ALT");

                        string keyString;
                        // For the command ID, use the full 16-bit value as seen in Resource Hacker's output.
                        ushort finalID = rawID;

                        // Determine if the accelerator is a virtual key or a character.
                        // For this specific dump, it appears all key entries are intended as VIRTKEY.
                        if ((fFlags & FVIRTKEY) != 0)
                        {
                            rcFlags.Add("VIRTKEY"); // Add VIRTKEY flag to the RC output.

                            // Extract only the lower byte for the virtual key code lookup.
                            ushort actualVirtualKeyCode = (ushort)(rawVirtualKey & 0xFF);

                            // Try to get a human-readable name from the map.
                            if (VirtualKeyCodeMap.TryGetValue(actualVirtualKeyCode, out string vkName))
                            {
                                keyString = vkName;
                            }
                            else
                            {
                                // If not in map, try to convert common alphanumeric virtual keys (0-9, A-Z)
                                // to their VK_ representation (e.g., VK_A, VK_0).
                                if (actualVirtualKeyCode >= 0x30 && actualVirtualKeyCode <= 0x39) // '0'-'9'
                                {
                                    keyString = $"VK_{(char)actualVirtualKeyCode}";
                                }
                                else if (actualVirtualKeyCode >= 0x41 && actualVirtualKeyCode <= 0x5A) // 'A'-'Z'
                                {
                                    keyString = $"VK_{(char)actualVirtualKeyCode}";
                                }
                                else
                                {
                                    // Fallback for unknown or less common virtual keys: use hex representation of the original 16-bit value.
                                    // We use the original 16-bit value here to preserve all information if the lower byte isn't a standard VK.
                                    keyString = $"0x{rawVirtualKey:X4}";
                                }
                            }
                        }
                        else // FVIRTKEY is NOT set, so it's nominally a character accelerator.
                        {
                            // Handle specific known special characters first.
                            if (rawVirtualKey == '\t')
                            {
                                keyString = "\"\\t\""; // Represent tab as "\t" in RC
                            }
                            else if (rawVirtualKey == '\0')
                            {
                                keyString = "\"\\0\""; // Represent null as "\0" in RC
                            }
                            else
                            {
                                // Fallback for unknown characters or those not matching a VK_ code.
                                // Represent as hex, and add ASCII flag if it's a control/whitespace char.
                                if (char.IsControl((char)rawVirtualKey) || (char.IsWhiteSpace((char)rawVirtualKey) && rawVirtualKey != ' '))
                                {
                                    rcFlags.Add("ASCII");
                                }
                                keyString = $"0x{rawVirtualKey:X4}";
                            }
                        }

                        // Format the flags string for the RC file.
                        string flagsString = rcFlags.Count > 0 ? ", " + string.Join(" | ", rcFlags) : "";

                        // Append the formatted accelerator entry to the StringBuilder.
                        rcContent.AppendLine($"    {keyString}, {finalID}{flagsString}");
                    }
                }
                catch (EndOfStreamException)
                {
                    // This exception might occur if the data is malformed and ends abruptly.
                    rcContent.AppendLine("// Error: End of stream reached unexpectedly, input data might be truncated or malformed.");
                }
                catch (Exception ex)
                {
                    // Catch any other unexpected errors during parsing.
                    rcContent.AppendLine($"// An unexpected error occurred during decoding: {ex.Message}");
                }
            }

            rcContent.AppendLine("}");

            return rcContent.ToString();
        }
    }
}