using System;
using System.Text;
using System.Collections.Generic;

namespace PeareModule
{
    public static class RT_ACCELTABLE
    {
        // Define AccelTypeFlags as const ushort directly within the class
        // These flags come from pmwin.h
        public const ushort KC_NONE                   = 0x0000;     /* Reserved */
        public const ushort KC_CHAR                   = 0x0001;
        public const ushort KC_VIRTUALKEY             = 0x0002;
        public const ushort KC_SCANCODE               = 0x0004;

        public const ushort KC_SHIFT                  = 0x0008;
        public const ushort KC_CTRL                   = 0x0010;
        public const ushort KC_ALT                    = 0x0020;
        public const ushort KC_KEYUP                  = 0x0040;
        public const ushort KC_PREVDOWN               = 0x0080;
        public const ushort KC_LONEKEY                = 0x0100;
        public const ushort KC_DEADKEY                = 0x0200;
        public const ushort KC_COMPOSITE              = 0x0400;
        public const ushort KC_INVALIDCOMP            = 0x0800;

        public static string Get(byte[] data, ModuleResources.ModuleProperties properties)
        {
            StringBuilder result = new StringBuilder();
            int offset = 0;

            if (data == null || data.Length < 4)
            {
                return "Error: Invalid data or data too short. Expected at least 4 bytes.";
            }

            try
            {
                // Read count and codepage (these are typically outside the ACCELERATORS block in RC)
                ushort count = RT_STRING.ReadUInt16(data, ref offset);
                ushort cp = RT_STRING.ReadUInt16(data, ref offset);

                // Add RC file header for accelerators
                //result.AppendLine("// Generated from RT_ACCELTABLE resource");
                //result.AppendLine($"// Number of entries: {count}, Codepage: {cp}");
                result.AppendLine("ACCELERATORS");
                result.AppendLine("{");

                if (data.Length < 4 + (count * 6))
                {
                    return $"Error: Data too short for {count} entries. Expected at least {4 + (count * 6)} bytes, but got {data.Length}. Aborting RC generation.";
                }

                for (int i = 0; i < count; i++)
                {
                    if (offset + 6 > data.Length)
                    {
                        result.AppendLine($"    // WARNING: Not enough data for entry {i + 1}. Remaining bytes: {data.Length - offset}. Truncated RC output.");
                        break;
                    }

                    ushort type = RT_STRING.ReadUInt16(data, ref offset);
                    ushort key = RT_STRING.ReadUInt16(data, ref offset);
                    ushort cmd = RT_STRING.ReadUInt16(data, ref offset);

                    StringBuilder rcFlags = new StringBuilder();
                    string keyLiteral;

                    // Determine if it's a CHAR or VIRTKEY based on AF_CHAR flag and key value
                    bool isCharKey = (type & KC_CHAR) != 0;

                    if (isCharKey)
                    {
                        rcFlags.Append("ASCII");
                        keyLiteral = $"'{(char)key}'"; // e.g., 'A'
                    }
                    else // Assume VIRTKEY if AF_CHAR is not set
                    {
                        rcFlags.Append("VIRTKEY");
                        if (RT_ACCELERATOR.VirtualKeyCodeMap.TryGetValue(key, out string vkName))
                        {
                            keyLiteral = vkName; // Use VK_NAME
                        }
                        else
                        {
                            keyLiteral = $"0x{key:X4}"; // Fallback to hex for unknown VKs
                        }
                    }

                    // Append modifier flags
                    if ((type & KC_SHIFT) != 0) rcFlags.Append(", SHIFT");
                    if ((type & KC_CTRL) != 0) rcFlags.Append(", CONTROL");
                    if ((type & KC_ALT) != 0) rcFlags.Append(", ALT");

                    if ((type & KC_KEYUP) != 0) rcFlags.Append(", KEYUP");
                    if ((type & KC_PREVDOWN) != 0) rcFlags.Append(", PREVDOWN");
                    if ((type & KC_LONEKEY) != 0) rcFlags.Append(", LONEKEY");
                    if ((type & KC_DEADKEY) != 0) rcFlags.Append(", DEADKEY");
                    if ((type & KC_COMPOSITE) != 0) rcFlags.Append(", COMPOSITE");
                    if ((type & KC_INVALIDCOMP) != 0) rcFlags.Append(", INVALIDCOMP");

                    // RC compilers only understand specific flags. Unrecognized bits are ignored for RC syntax.
                    // For debugging, you could uncomment the following to see "unknown" bits in comments:
                    ushort knownFlagsMask = (ushort)(KC_CHAR | KC_VIRTUALKEY | KC_SCANCODE | 
                        KC_SHIFT | KC_CTRL | KC_ALT |
                        KC_KEYUP | KC_PREVDOWN | KC_LONEKEY | KC_DEADKEY | KC_COMPOSITE | KC_INVALIDCOMP);
                    ushort unrecognizedFlags = (ushort)(type & ~knownFlagsMask);
                    if (unrecognizedFlags != 0)
                    {
                        result.AppendLine($"    // WARNING: Original type 0x{type:X4} had unrecognized bits: 0x{unrecognizedFlags:X4}");
                    }

                    result.AppendLine($"    {keyLiteral}, 0x{cmd:X4}, {rcFlags}");
                }

                result.AppendLine("}");
            }
            catch (Exception ex)
            {
                return $"An unexpected error occurred during parsing: {ex.Message}";
            }

            return result.ToString();
        }
    }
}
