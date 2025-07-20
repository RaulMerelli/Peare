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

        // Mapping for control characters to their ^X representation
        public static readonly Dictionary<ushort, string> ControlCharMap = new Dictionary<ushort, string>
        {
            { 0x01, "^A" }, { 0x02, "^B" }, { 0x03, "^C" }, { 0x04, "^D" },
            { 0x05, "^E" }, { 0x06, "^F" }, { 0x07, "^G" }, { 0x08, "^H" },
            { 0x09, "^I" }, { 0x0A, "^J" }, { 0x0B, "^K" }, { 0x0C, "^L" },
            { 0x0D, "^M" }, { 0x0E, "^N" }, { 0x0F, "^O" }, { 0x10, "^P" },
            { 0x11, "^Q" }, { 0x12, "^R" }, { 0x13, "^S" }, { 0x14, "^T" },
            { 0x15, "^U" }, { 0x16, "^V" }, { 0x17, "^W" }, { 0x18, "^X" },
            { 0x19, "^Y" }, { 0x1A, "^Z" }
        };


        public static string Get(byte[] resData, ModuleResources.ModuleProperties properties)
        {
            if (resData == null || resData.Length == 0)
            {
                return "// No accelerator data provided or data is empty.";
            }

            // Early exit for specific NE/LX types handled by another class.
            if ((properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                properties.headerType == ModuleResources.HeaderType.LX)
            {
                return RT_ACCELTABLE.Get(resData, properties);
            }

            StringBuilder rcContent = new StringBuilder();
            rcContent.AppendLine("ACCELERATORS");
            rcContent.AppendLine("{");

            using (MemoryStream ms = new MemoryStream(resData))
            using (BinaryReader reader = new BinaryReader(ms))
            {
                try
                {
                    bool isNEFormat = (properties.headerType == ModuleResources.HeaderType.NE);
                    bool isLastEntry = false;

                    while (!isLastEntry && reader.BaseStream.Position < reader.BaseStream.Length)
                    {
                        ushort flags; // Use ushort for flags to be consistent for both PE and NE for bitwise operations.
                                      // The actual bytes read will differ.
                        ushort rawKeyOrCharCode;
                        ushort commandID;

                        if (isNEFormat)
                        {
                            // For NE (Windows 3.x), entries are 5 bytes: byte flags, ushort key, ushort command.
                            if (reader.BaseStream.Length - reader.BaseStream.Position < 5)
                            {
                                rcContent.AppendLine("// Warning: Incomplete accelerator entry detected at end of stream for NE format.");
                                break;
                            }
                            flags = reader.ReadByte(); // Read flags as a single byte, implicitly cast to ushort
                            rawKeyOrCharCode = reader.ReadUInt16(); // Read key as ushort
                            commandID = reader.ReadUInt16(); // Read command as ushort

                            // The FLAST flag for NE is 0x80 within the single byte.
                            isLastEntry = (flags & FLAST) != 0;

                            // No explicit internal padding to skip for NE entries.
                        }
                        else // Assume PE format
                        {
                            // For PE, entries are 8 bytes: ushort flags, ushort key, ushort command, ushort padding.
                            if (reader.BaseStream.Length - reader.BaseStream.Position < 8) // Expect 8 bytes per entry for PE
                            {
                                rcContent.AppendLine("// Warning: Incomplete accelerator entry detected at end of stream for PE format.");
                                break;
                            }
                            flags = reader.ReadUInt16(); // Read flags as ushort
                            rawKeyOrCharCode = reader.ReadUInt16(); // Read key as ushort
                            commandID = reader.ReadUInt16(); // Read command as ushort

                            // For PE, FLAST is 0x0080 in the ushort flags.
                            isLastEntry = (flags & FLAST) != 0;

                            reader.ReadUInt16(); // Skip 2 bytes of padding after each PE entry
                        }

                        List<string> rcFlags = new List<string>();
                        if ((flags & FCONTROL) != 0) rcFlags.Add("CONTROL");
                        if ((flags & FSHIFT) != 0) rcFlags.Add("SHIFT");
                        if ((flags & FALT) != 0) rcFlags.Add("ALT");
                        if ((flags & FNOINVERT) != 0) rcFlags.Add("NOINVERT");

                        string keyString;

                        if ((flags & FVIRTKEY) != 0)
                        {
                            rcFlags.Add("VIRTKEY");
                            if (VirtualKeyCodeMap.TryGetValue(rawKeyOrCharCode, out string vkName))
                            {
                                keyString = vkName;
                            }
                            else
                            {
                                // If a virtual key is not mapped, represent it as its hex value
                                // Use X4 for PE as VK codes can be > 0xFF, X2 for NE if it's strictly byte range.
                                // For consistency, and since rawKeyOrCharCode is ushort, X4 is safer.
                                keyString = $"0x{rawKeyOrCharCode:X4}";
                            }
                        }
                        else // Not a VIRTKEY, so it's a character or control character (ASCII/ANSI)
                        {
                            char charCode = (char)rawKeyOrCharCode;
                            if (ControlCharMap.TryGetValue(rawKeyOrCharCode, out string controlCharName))
                            {
                                keyString = $"\"{controlCharName}\"";
                            }
                            else if (char.IsLetterOrDigit(charCode) || char.IsPunctuation(charCode) || char.IsSymbol(charCode) || char.IsWhiteSpace(charCode))
                            {
                                if (charCode == '"')
                                {
                                    keyString = "\"\\\"\"";
                                }
                                else if (charCode == '\\')
                                {
                                    keyString = "\"\\\\\"";
                                }
                                else
                                {
                                    keyString = $"\"{charCode}\"";
                                }
                            }
                            else
                            {
                                // If it's an unprintable character, or something outside common ASCII, represent as hex.
                                // PE accelerators can use Unicode chars, so X4 is more appropriate here.
                                // For NE, it's typically just byte-sized ASCII.
                                rcFlags.Add("ASCII"); // Indicate it's an ASCII/ANSI character if not printable
                                keyString = $"0x{rawKeyOrCharCode:X4}";
                            }
                        }

                        // Order flags for RC output: VIRTKEY, then modifiers, then others.
                        List<string> orderedRcFlags = new List<string>();
                        if (rcFlags.Contains("VIRTKEY")) orderedRcFlags.Add("VIRTKEY");
                        if (rcFlags.Contains("CONTROL")) orderedRcFlags.Add("CONTROL");
                        if (rcFlags.Contains("SHIFT")) orderedRcFlags.Add("SHIFT");
                        if (rcFlags.Contains("ALT")) orderedRcFlags.Add("ALT");
                        if (rcFlags.Contains("NOINVERT")) orderedRcFlags.Add("NOINVERT");
                        if (rcFlags.Contains("ASCII")) orderedRcFlags.Add("ASCII");

                        string flagsString = orderedRcFlags.Count > 0 ? ", " + string.Join(", ", orderedRcFlags) : "";

                        rcContent.AppendLine($"\t{keyString}, {commandID}{flagsString}");

                        if (isLastEntry)
                        {
                            // For NE, there might be residual padding after the FLAST entry
                            // to align the entire resource. Consume it.
                            if (isNEFormat)
                            {
                                int remainingBytes = (int)(reader.BaseStream.Length - reader.BaseStream.Position);
                                if (remainingBytes > 0)
                                {
                                    reader.ReadBytes(remainingBytes); // Consume remaining bytes
                                }
                            }
                            break; // Stop processing after the last entry
                        }
                    }
                }
                catch (EndOfStreamException)
                {
                    rcContent.AppendLine("// Error: End of stream reached unexpectedly, input data might be truncated or malformed.");
                }
                catch (Exception ex)
                {
                    rcContent.AppendLine($"// An unexpected error occurred during decoding: {ex.Message}");
                }
            }

            rcContent.AppendLine("}");

            return rcContent.ToString();
        }
    }
}