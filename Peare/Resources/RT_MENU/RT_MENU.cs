using System;
using System.Text;
using System.Collections.Generic;
using System.Linq; // Added for Enumerable.Contains

namespace Peare
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

        public static string Get(byte[] data)
        {
            // Program.DumpRaw(data);
            if (data == null || data.Length < 2)
            {
                return "Insufficient data for a valid menu header.";
            }

            StringBuilder menuOutput = new StringBuilder();
            int offset = 0;
            bool isUnicode = true; 

            // Skip the first 4 bytes if they are zero (assuming an empty MENUHEADER)
            if (data.Length >= 4 && BitConverter.ToUInt32(data, 0) == 0)
            {
                offset = 4;
            }

            Stack<int> indentLevels = new Stack<int>();

            // Helper function for indentation
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
                string menuText;
                ushort wID;

                if (isUnicode)
                {
                    if (offset + 2 > data.Length)
                    {
                        menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read usFlags.");
                        break;
                    }
                    usFlags = BitConverter.ToUInt16(data, offset);
                    offset += 2;
                }
                else // ANSI
                {
                    if (offset + 1 > data.Length) // ANSI usFlags is 1 byte
                    {
                        menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read usFlags (ANSI).");
                        break;
                    }
                    usFlags = data[offset]; // Read as a single byte
                    offset += 1;
                }

                bool isPopup = (usFlags & (ushort)MenuFlags.MF_POPUP) != 0;
                bool isEnd = (usFlags & (ushort)MenuFlags.MF_END) != 0;
                bool isSeparatorFlag = (usFlags & (ushort)MenuFlags.MF_SEPARATOR) != 0;

                if (isPopup && indentLevels.Count > 1)
                {
                    // This logic seems a bit off for handling nested popups and their closure.
                    // If a popup is encountered, it generally starts a *new* block, not closes existing ones
                    // unless it's explicitly an MF_END type for the *previous* block.
                    // The original code was popping while indentLevels.Count > 1, which might close all parent popups.
                    // Re-evaluating the original intent, if a POPUP is encountered, it *starts* a new block,
                    // so the 'while' loop here is likely incorrect. We should only close blocks when MF_END is hit.
                    // For now, I'm keeping the original logic's structure for closing, but it might need review
                    // based on exact RT_MENU structure definition.
                    while (indentLevels.Count > 1)
                    {
                        indentLevels.Pop();
                        menuOutput.AppendLine(GetIndentString() + "}");
                    }
                }

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

                    if (textEndOffset == -1 || textEndOffset > data.Length)
                    {
                        menuOutput.AppendLine(GetIndentString() + "Insufficient data to read the text string for POPUP.");
                        break;
                    }

                    int stringLengthInBytes = textEndOffset - offset;
                    if (stringLengthInBytes < 0) stringLengthInBytes = 0;

                    if (isUnicode)
                    {
                        // Ensure an even length for Unicode strings
                        if (stringLengthInBytes % 2 != 0) stringLengthInBytes--;
                        menuText = Encoding.Unicode.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 2; // +2 for the null terminator
                    }
                    else // ANSI
                    {
                        menuText = Encoding.Default.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 1; // +1 for the null terminator
                    }

                    menuOutput.AppendLine(GetIndentString() + $"POPUP \"{menuText}\"");
                    menuOutput.AppendLine(GetIndentString() + "{");
                    indentLevels.Push(1); // Opens a new POPUP block
                }
                else // It's a normal ITEM or a SEPARATOR
                {
                    if (isUnicode)
                    {
                        if (offset + 2 > data.Length)
                        {
                            menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read wID for NORMAL MENU ITEM.");
                            break;
                        }
                        wID = BitConverter.ToUInt16(data, offset);
                        offset += 2;
                    }
                    else // ANSI
                    {
                        if (offset + 1 > data.Length) // ANSI wID is 1 byte
                        {
                            menuOutput.AppendLine(GetIndentString() + "Truncated data, unable to read wID for NORMAL MENU ITEM (ANSI).");
                            break;
                        }
                        wID = data[offset]; // Read as a single byte
                        offset += 1;
                    }

                    int textEndOffset;
                    if (isUnicode)
                    {
                        textEndOffset = FindNullTerminatedUnicodeStringEnd(data, offset);
                    }
                    else
                    {
                        textEndOffset = FindNullTerminatedAnsiStringEnd(data, offset);
                    }

                    if (textEndOffset == -1 || textEndOffset > data.Length)
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
                        offset = textEndOffset + 2; // +2 for the null terminator
                    }
                    else // ANSI
                    {
                        menuText = Encoding.Default.GetString(data, offset, stringLengthInBytes);
                        offset = textEndOffset + 1; // +1 for the null terminator
                    }

                    if (wID == 0 && string.IsNullOrEmpty(menuText))
                    {
                        menuOutput.AppendLine(GetIndentString() + "MENUITEM SEPARATOR");
                    }
                    else if (isSeparatorFlag)
                    {
                        menuOutput.AppendLine(GetIndentString() + "MENUITEM SEPARATOR");
                    }
                    else
                    {
                        menuOutput.AppendLine(GetIndentString() + $"MENUITEM \"{menuText}\", {wID}");
                    }

                    // If the current ITEM has MF_END, it means this is the last ITEM of its submenu.
                    if (isEnd && indentLevels.Count > 1)
                    {
                        indentLevels.Pop(); // Closes the submenu block
                        menuOutput.AppendLine(GetIndentString() + "}");
                    }
                }
            }

            // Closes all remaining blocks at the end of the file (only the main MENU block if everything went well)
            while (indentLevels.Count > 0)
            {
                indentLevels.Pop();
                menuOutput.AppendLine(GetIndentString() + "}");
            }

            return menuOutput.ToString();
        }

        private static int FindNullTerminatedUnicodeStringEnd(byte[] data, int startIndex)
        {
            if (startIndex < 0 || startIndex >= data.Length)
            {
                return -1; // Indicate error or out of bounds
            }

            for (int i = startIndex; i < data.Length - 1; i += 2)
            {
                if (data[i] == 0x00 && data[i + 1] == 0x00)
                {
                    return i; // Return the start of the null terminator
                }
            }
            return data.Length; // No null terminator found, string goes to end of data (or is truncated)
        }

        private static int FindNullTerminatedAnsiStringEnd(byte[] data, int startIndex)
        {
            if (startIndex < 0 || startIndex >= data.Length)
            {
                return -1; // Indicate error or out of bounds
            }

            for (int i = startIndex; i < data.Length; i++)
            {
                if (data[i] == 0x00)
                {
                    return i; // Return the start of the null terminator
                }
            }
            return data.Length; // No null terminator found, string goes to end of data (or is truncated)
        }
    }
}