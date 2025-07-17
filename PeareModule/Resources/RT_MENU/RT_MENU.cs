using System;
using System.Text;
using System.Collections.Generic;
using System.Linq;

namespace PeareModule
{
    public static class RT_MENU
    {
        [Flags]
        public enum MenuFlags : ushort
        {
            MF_ENABLED = 0x0000,
            MF_STRING = 0x0000,
            MF_DISABLED = 0x0002,
            MF_GRAYED = 0x0001,
            MF_BITMAP = 0x0004,
            MF_CHECKED = 0x0008,
            MF_POPUP = 0x0010,
            MF_MENUBREAK = 0x0040,
            MF_MENUBARBREAK = 0x0020,
            MF_UNCHECKED = 0x0000,
            MF_SEPARATOR = 0x0800,
            MF_BYCOMMAND = 0x0000,
            MF_BYPOSITION = 0x0400,
            MF_HELP = 0x4000,
            MF_RIGHTJUSTIFY = 0x4000,
            MF_MOUSESELECT = 0x8000,
            MF_END = 0x0080
        }

        public static string Get(byte[] data, ModuleResources.ModuleProperties properties)
        {
            if (data == null || data.Length < 2)
            {
                return "Insufficient data for a valid menu header.";
            }

            StringBuilder menuOutput = new StringBuilder();
            int offset = 0;
            bool isUnicode = properties.headerType == ModuleResources.HeaderType.PE;

            if ((properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) || 
                properties.headerType == ModuleResources.HeaderType.LX)
            {
                // Structure is different for OS/2
                return OS2_RTMENU.Get(data, properties);
            }

            if (data.Length >= 4 && BitConverter.ToUInt32(data, 0) == 0)
            {
                offset += 4;
            }

            Stack<int> indentLevels = new Stack<int>();

            string GetIndentString()
            {
                return new string(' ', indentLevels.Count * 2);
            }

            menuOutput.AppendLine("MENU");
            menuOutput.AppendLine("{");
            indentLevels.Push(1); // Start with the main MENU block at level 1

            while (offset < data.Length)
            {
                ushort usFlags;
                string menuText = "";
                ushort wID = 0;

                if (offset + 2 > data.Length)
                {
                    menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read usFlags.");
                    break;
                }
                usFlags = BitConverter.ToUInt16(data, offset);
                offset += 2;

                bool isPopup = (usFlags & (ushort)MenuFlags.MF_POPUP) != 0;
                bool isEnd = (usFlags & (ushort)MenuFlags.MF_END) != 0;
                bool isSeparatorFlag = (usFlags & (ushort)MenuFlags.MF_SEPARATOR) != 0;

                if (isPopup)
                {
                    int textEndOffset;
                    if (isUnicode)
                    {
                        textEndOffset = FindNullTerminatedUnicodeStringEnd(data, offset);
                    }
                    else
                    {
                        textEndOffset = FindNullTerminatedAnsiStringEnd(data, offset);
                    }

                    if (textEndOffset == offset && offset != data.Length) // Empty string at current offset
                    {
                        // Handle case where textEndOffset is returned as startIndex due to invalid input
                        if (textEndOffset == offset && ((isUnicode && offset >= data.Length - 1) || (!isUnicode && offset >= data.Length)))
                        {
                            menuOutput.AppendLine(GetIndentString() + "Insufficient data to read the text string for POPUP.");
                            break;
                        }
                    }
                    else if (textEndOffset == -1 || textEndOffset > data.Length)
                    {
                        menuOutput.AppendLine(GetIndentString() + "Insufficient data to read the text string for POPUP.");
                        break;
                    }


                    int stringLengthInBytes = textEndOffset - offset;
                    if (stringLengthInBytes < 0) stringLengthInBytes = 0; // Should not be <0 with proper find methods

                    if (isUnicode)
                    {
                        if (stringLengthInBytes % 2 != 0) stringLengthInBytes--;
                        menuText = Encoding.Unicode.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 2;
                    }
                    else // ANSI
                    {
                        menuText = Encoding.Default.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 1;
                    }

                    string flagString = GetMenuFlagsString((ushort)(usFlags & ~(int)MenuFlags.MF_POPUP));
                    if (!string.IsNullOrEmpty(flagString))
                    {
                        menuOutput.AppendLine(GetIndentString() + $"POPUP \"{menuText}\", {flagString}");
                    }
                    else
                    {
                        menuOutput.AppendLine(GetIndentString() + $"POPUP \"{menuText}\"");
                    }
                    menuOutput.AppendLine(GetIndentString() + "{");
                    indentLevels.Push(1);
                }
                else // It's a normal ITEM or a SEPARATOR
                {
                    if (offset + 2 > data.Length)
                    {
                        menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read wID for NORMAL MENU ITEM.");
                        break;
                    }
                    wID = BitConverter.ToUInt16(data, offset);
                    offset += 2;

                    int textEndOffset;
                    if (isUnicode)
                    {
                        textEndOffset = FindNullTerminatedUnicodeStringEnd(data, offset);
                    }
                    else
                    {
                        textEndOffset = FindNullTerminatedAnsiStringEnd(data, offset);
                    }

                    if (textEndOffset == offset && offset != data.Length) // Empty string at current offset
                    {
                        // Handle case where textEndOffset is returned as startIndex due to invalid input
                        if (textEndOffset == offset && (isUnicode ? (offset >= data.Length - 1) : (offset >= data.Length)))
                        {
                            menuOutput.AppendLine(GetIndentString() + "Insufficient data to read the text string for NORMAL MENU ITEM.");
                            break;
                        }
                    }
                    else if (textEndOffset == -1 || textEndOffset > data.Length)
                    {
                        menuOutput.AppendLine(GetIndentString() + "Insufficient data to read the text string for NORMAL MENU ITEM.");
                        break;
                    }

                    int stringLengthInBytes = textEndOffset - offset;
                    if (stringLengthInBytes < 0) stringLengthInBytes = 0;

                    if (isUnicode)
                    {
                        if (stringLengthInBytes % 2 != 0) stringLengthInBytes--;
                        menuText = Encoding.Unicode.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 2;
                    }
                    else // ANSI
                    {
                        menuText = Encoding.Default.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 1;
                    }

                    // Corrected separator logic
                    if (isSeparatorFlag || (wID == 0 && string.IsNullOrEmpty(menuText)))
                    {
                        menuOutput.AppendLine(GetIndentString() + "MENUITEM SEPARATOR");
                    }
                    else
                    {
                        string flagString = GetMenuFlagsString(usFlags);
                        if (!string.IsNullOrEmpty(flagString))
                        {
                            menuOutput.AppendLine(GetIndentString() + $"MENUITEM \"{menuText}\", {wID}, {flagString}");
                        }
                        else
                        {
                            menuOutput.AppendLine(GetIndentString() + $"MENUITEM \"{menuText}\", {wID}");
                        }
                    }

                    if (isEnd && indentLevels.Count > 1)
                    {
                        indentLevels.Pop();
                        menuOutput.AppendLine(GetIndentString() + "}");
                        if (IsRemainingDataNull(data, offset))
                            break;
                    }
                }
            }

            while (indentLevels.Count > 0)
            {
                indentLevels.Pop();
                menuOutput.AppendLine(GetIndentString() + "}");
            }

            return menuOutput.ToString();
        }

        // Helper method to check if remaining data is all nulls
        private static bool IsRemainingDataNull(byte[] data, int startIndex)
        {
            for (int i = startIndex; i < data.Length; i++)
            {
                if (data[i] != 0x00)
                {
                    return false;
                }
            }
            return true;
        }

        private static string GetMenuFlagsString(ushort flags)
        {
            List<string> flagNames = new List<string>();

            if ((flags & (ushort)MenuFlags.MF_DISABLED) != 0) flagNames.Add("DISABLED");
            else if ((flags & (ushort)MenuFlags.MF_GRAYED) != 0) flagNames.Add("GRAYED");

            if ((flags & (ushort)MenuFlags.MF_BITMAP) != 0) flagNames.Add("BITMAP");

            if ((flags & (ushort)MenuFlags.MF_CHECKED) != 0) flagNames.Add("CHECKED");

            if ((flags & (ushort)MenuFlags.MF_MENUBREAK) != 0) flagNames.Add("MENUBREAK");
            if ((flags & (ushort)MenuFlags.MF_MENUBARBREAK) != 0) flagNames.Add("MENUBARBREAK");

            if (((flags & (ushort)MenuFlags.MF_HELP) != 0) && ((flags & (ushort)MenuFlags.MF_RIGHTJUSTIFY) != 0))
            {
                flagNames.Add("HELP | RIGHTJUSTIFY");
            }
            else if ((flags & (ushort)MenuFlags.MF_HELP) != 0)
            {
                flagNames.Add("HELP");
            }
            else if ((flags & (ushort)MenuFlags.MF_RIGHTJUSTIFY) != 0)
            {
                flagNames.Add("RIGHTJUSTIFY");
            }

            if ((flags & (ushort)MenuFlags.MF_MOUSESELECT) != 0) flagNames.Add("MOUSESELECT");

            return string.Join(", ", flagNames.Where(s => !string.IsNullOrEmpty(s)));
        }

        private static int FindNullTerminatedUnicodeStringEnd(byte[] data, int startIndex)
        {
            if (startIndex < 0 || startIndex >= data.Length - 1) return startIndex;

            for (int i = startIndex; i < data.Length - 1; i += 2)
            {
                if (data[i] == 0x00 && data[i + 1] == 0x00)
                {
                    return i;
                }
            }
            return data.Length - (data.Length % 2); // Ensure returned end makes for an even string length
        }

        private static int FindNullTerminatedAnsiStringEnd(byte[] data, int startIndex)
        {
            if (startIndex < 0 || startIndex >= data.Length) return startIndex;

            for (int i = startIndex; i < data.Length; i++)
            {
                if (data[i] == 0x00)
                {
                    return i;
                }
            }
            return data.Length;
        }
    }
}