using System;
using System.IO;
using System.Text;

namespace PeareModule
{
    // Special thanks to Paul Ratcliffe for the Res2Dlg utility.
    // This code is heavily ispired to it and I have used it to compare and test the results.
    // Many issues are still here and the result is mostly still not the same.
    public static class OS2_RT_DIALOG
    {
        // Static buffers and variables
        private static readonly StringBuilder outputBuffer = new StringBuilder(1024);
        private static readonly StringBuilder outputBuffer2 = new StringBuilder(1024);
        private static readonly StringBuilder idBuffer = new StringBuilder(128);
        private static TextWriter output;
        private static bool nullsDetected = false;

        // Style name tables
        private static readonly string[] par_pp = {
            "0", "PP_FOREGROUNDCOLOR", "PP_FOREGROUNDCOLORINDEX", "PP_BACKGROUNDCOLOR",
            "PP_BACKGROUNDCOLORINDEX", "PP_HILITEFOREGROUNDCOLOR", "PP_HILITEFOREGROUNDCOLORINDEX",
            "PP_HILITEBACKGROUNDCOLOR", "PP_HILITEBACKGROUNDCOLORINDEX", "PP_DISABLEDFOREGROUNDCOLOR",
            "PP_DISABLEDFOREGROUNDCOLORINDEX", "PP_DISABLEDBACKGROUNDCOLOR", "PP_DISABLEDBACKGROUNDCOLORINDEX",
            "PP_BORDERCOLOR", "PP_BORDERCOLORINDEX", "PP_FONTNAMESIZE", "PP_FONTHANDLE", "PP_RESERVED",
            "PP_ACTIVECOLOR", "PP_ACTIVECOLORINDEX", "PP_INACTIVECOLOR", "PP_INACTIVECOLORINDEX",
            "PP_ACTIVETEXTFGNDCOLOR", "PP_ACTIVETEXTFGNDCOLORINDEX", "PP_ACTIVETEXTBGNDCOLOR",
            "PP_ACTIVETEXTBGNDCOLORINDEX", "PP_INACTIVETEXTFGNDCOLOR", "PP_INACTIVETEXTFGNDCOLORINDEX",
            "PP_INACTIVETEXTBGNDCOLOR", "PP_INACTIVETEXTBGNDCOLORINDEX", "PP_SHADOW", "PP_MENUFOREGROUNDCOLOR",
            "PP_MENUFOREGROUNDCOLORINDEX", "PP_MENUBACKGROUNDCOLOR", "PP_MENUBACKGROUNDCOLORINDEX",
            "PP_MENUHILITEFGNDCOLOR", "PP_MENUHILITEFGNDCOLORINDEX", "PP_MENUHILITEBGNDCOLOR",
            "PP_MENUHILITEBGNDCOLORINDEX", "PP_MENUDISABLEDFGNDCOLOR", "PP_MENUDISABLEDFGNDCOLORINDEX",
            "PP_MENUDISABLEDBGNDCOLOR", "PP_MENUDISABLEDBGNDCOLORINDEX", "PP_SHADOWTEXTCOLOR",
            "PP_SHADOWTEXTCOLORINDEX", "PP_SHADOWHILITEFGNDCOLOR", "PP_SHADOWHILITEFGNDCOLORINDEX",
            "PP_SHADOWHILITEBGNDCOLOR", "PP_SHADOWHILITEBGNDCOLORINDEX", "PP_ICONTEXTBACKGROUNDCOLOR",
            "PP_ICONTEXTBACKGROUNDCOLORINDEX", null
        };

        private static readonly string[] par_wc = {
            null, "WC_FRAME", "WC_COMBOBOX", "WC_BUTTON", "WC_MENU", "WC_STATIC", "WC_ENTRYFIELD",
            "WC_LISTBOX", "WC_SCROLLBAR", "WC_TITLEBAR", "WC_MLE", null, null, null, null, null,
            "WC_APPSTAT", "WC_KBDSTAT", "WC_PECIC", "WC_DBE_KKPOPUP", null, null, null, null, null,
            null, null, null, null, null, null, null, null, null, null, "WC_SPINBUTTON", null, null,
            null, null, "WC_CONTAINER", "WC_SLIDER", "WC_VALUESET", "WC_NOTEBOOK", null, null, null,
            null, null, null, null, null, null, null, null, null, null, null, null, null, "WC_CIRCULARSLIDER", null
        };

        private static readonly string[] par_ws = {
            "WS_GROUP", "WS_TABSTOP", "WS_MULTISELECT", "WS_ANIMATE", "WS_MAXIMIZED", "WS_MINIMIZED",
            "WS_SYNCPAINT", "WS_SAVEBITS", "WS_PARENTCLIP", "WS_CLIPSIBLINGS", "WS_CLIPCHILDREN",
            "WS_DISABLED", "WS_VISIBLE", null, null, "WS_DBE_APPSTAT", null
        };

        private static readonly string[] par_fs = {
            "FS_ICON", "FS_ACCELTABLE", "FS_SHELLPOSITION", "FS_TASKLIST", "FS_NOBYTEALIGN",
            "FS_NOMOVEWITHOWNER", "FS_SYSMODAL", "FS_DLGBORDER", "FS_BORDER", "FS_SCREENALIGN",
            "FS_MOUSEALIGN", "FS_SIZEBORDER", "FS_AUTOICON", null, null, "FS_DBE_APPSTAT", null
        };

        private static readonly string[] par_fcf = {
            "FCF_TITLEBAR", "FCF_SYSMENU", "FCF_MENU", "FCF_SIZEBORDER", "FCF_MINBUTTON",
            "FCF_MAXBUTTON", "FCF_VERTSCROLL", "FCF_HORZSCROLL", "FCF_DLGBORDER", "FCF_BORDER",
            "FCF_SHELLPOSITION", "FCF_TASKLIST", "FCF_NOBYTEALIGN", "FCF_NOMOVEWITHOWNER",
            "FCF_ICON", "FCF_ACCELTABLE", "FCF_SYSMODAL", "FCF_SCREENALIGN", "FCF_MOUSEALIGN",
            null, null, null, null, "FCF_HIDEBUTTON", null, "FCF_CLOSEBUTTON", null, null, null,
            "FCF_AUTOICON", "FCF_DBE_APPSTAT", null
        };

        private static readonly string[] par_cbs = {
            "CBS_SIMPLE", "CBS_DROPDOWN", "CBS_DROPDOWNLIST", "LS_HORZSCROLL", "ES_AUTOTAB",
            null, null, null, null, null, null, null, null, null, null, null, null
        };

        private static readonly string[] par_bs1 = {
            null, null, null, null, "BS_TEXT", "BS_MINIICON", "BS_BITMAP", "BS_ICON", "BS_HELP",
            "BS_SYSCOMMAND", "BS_DEFAULT", "BS_NOPOINTERFOCUS", "BS_NOBORDER", "BS_NOCURSORSELECT",
            "BS_AUTOSIZE", null
        };

        private static readonly string[] par_bs2 = {
            null, null, null, null, null, "BS_3STATE", "BS_AUTO3STATE", "BS_USERBUTTON", "BS_NOTEBOOK", null
        };

        private static readonly string[] par_dt = {
            null, null, null, null, null, null, "SS_AUTOSIZE", "DT_EXTERNALLEADING", "DT_CENTER",
            "DT_RIGHT", "DT_VCENTER", "DT_BOTTOM", "DT_HALFTONE", "DT_MNEMONIC", "DT_WORDBREAK", "DT_ERASERECT", null
        };

        private static readonly string[] par_ss = {
            null, null, null, null, "SS_BITMAP", "SS_FGNDRECT", "SS_HALFTONERECT", "SS_BKGNDRECT",
            "SS_FGNDFRAME", "SS_HALFTONEFRAME", "SS_BKGNDFRAME", "SS_SYSICON", null
        };

        private static readonly string[] par_es1 = {
            "ES_CENTER", "ES_RIGHT", "ES_AUTOSCROLL", "ES_MARGIN", "ES_AUTOTAB", "ES_READONLY",
            "ES_COMMAND", "ES_UNREADABLE", "ES_AUTOSIZE", null, null, null, null, null, null, null
        };

        private static readonly string[] par_es2 = {
            null, "ES_SBCS", "ES_DBCS", "ES_MIXED", null
        };

        private static readonly string[] par_ls = {
            "LS_MULTIPLESEL", "LS_OWNERDRAW", "LS_NOADJUSTPOS", "LS_HORZSCROLL", "LS_EXTENDEDSEL",
            null, null, null, null, null, null, null, null, null, null, null
        };

        private static readonly string[] par_sbs = {
            "SBS_VERT", "SBS_THUMBSIZE", "SBS_AUTOTRACK", null, null, null, null, null, null,
            null, null, null, "SBS_AUTOSIZE", null, null, null
        };

        private static readonly string[] par_mls = {
            "MLS_WORDWRAP", "MLS_BORDER", "MLS_VSCROLL", "MLS_HSCROLL", "MLS_READONLY",
            "MLS_IGNORETAB", "MLS_DISABLEUNDO", null, null, null, null, null, null, null, null, null
        };

        private static readonly string[] par_spbs = {
            "SPBS_NUMERICONLY", "SPBS_READONLY", "SPBS_JUSTRIGHT", "SPBS_JUSTLEFT", "SPBS_MASTER",
            "SPBS_NOBORDER", null, "SPBS_PADWITHZEROS", "SPBS_FASTSPIN", null, null, null, null, null, null, null
        };

        private static readonly string[] par_ccs = {
            "CCS_EXTENDSEL", "CCS_MULTIPLESEL", "CCS_SINGLESEL", "CCS_AUTOPOSITION", "CCS_VERIFYPOINTERS",
            "CCS_READONLY", "CCS_MINIRECORDCORE", null, null, null, null, "CCS_MINIICONS", "CCS_NOCONTROLPTR",
            null, null, null
        };

        private static readonly string[] par_sls1 = {
            "SLS_VERTICAL", "SLS_BOTTOM", "SLS_TOP", "SLS_SNAPTOINCREMENT", "SLS_BUTTONSBOTTOM",
            "SLS_BUTTONSTOP", "SLS_OWNERDRAW", "SLS_READONLY", "SLS_RIBBONSTRIP", "SLS_HOMETOP",
            "SLS_PRIMARYSCALE2", null, null, null, null, null
        };

        private static readonly string[] par_sls2 = {
            "SLS_VERTICAL", "SLS_LEFT", "SLS_RIGHT", "SLS_SNAPTOINCREMENT", "SLS_BUTTONSLEFT",
            "SLS_BUTTONSRIGHT", "SLS_OWNERDRAW", "SLS_READONLY", "SLS_RIBBONSTRIP", "SLS_HOMERIGHT",
            "SLS_PRIMARYSCALE2", null, null, null, null, null
        };

        private static readonly string[] par_vs = {
            "VS_BITMAP", "VS_ICON", "VS_TEXT", "VS_RGB", "VS_COLORINDEX", "VS_BORDER", "VS_ITEMBORDER",
            "VS_SCALEBITMAPS", "VS_RIGHTTOLEFT", "VS_OWNERDRAW", null, null, null, null, null, null
        };

        private static readonly string[] par_bks = {
            "BKS_BACKPAGESBR", "BKS_BACKPAGESBL", "BKS_BACKPAGESTR", "BKS_BACKPAGESTL",
            "BKS_MAJORTABRIGHT", "BKS_MAJORTABLEFT", "BKS_MAJORTABTOP", "BKS_MAJORTABBOTTOM",
            "BKS_ROUNDEDTABS", "BKS_POLYGONTABS", "BKS_SPIRALBIND", null, "BKS_STATUSTEXTRIGHT",
            "BKS_STATUSTEXTCENTER", "BKS_TABTEXTRIGHT", "BKS_TABTEXTCENTER"
        };

        private static readonly string[] par_css = {
            "CSS_NOBUTTON", "CSS_NOTEXT", "CSS_NONUMBER", "CSS_POINTSELECT", "CSS_360", "CSS_MIDPOINT",
            "CSS_PROPORTIONALTICKS", "CSS_NOTICKS", "CSS_CIRCULARVALUE", null, null, null, null, null, null, null
        };

        private static void WriteFormatted(TextWriter hOut, string format, params object[] args)
        {
            hOut.Write(string.Format(format, args));
        }

        private static ushort ReadUInt16(byte[] buffer, int offset)
        {
            return (ushort)(buffer[offset] | (buffer[offset + 1] << 8));
        }

        private static uint ReadUInt32(byte[] buffer, int offset)
        {
            return (uint)(buffer[offset] | (buffer[offset + 1] << 8) | (buffer[offset + 2] << 16) | (buffer[offset + 3] << 24));
        }

        public static string Get(byte[] resData)
        {
            output = new StringWriter();

            WriteFormatted(output, "#ifndef OS2_INCLUDED\n");
            WriteFormatted(output, "   #ifndef INCL_NLS\n");
            WriteFormatted(output, "      #define INCL_NLS\n");
            WriteFormatted(output, "   #endif\n");
            WriteFormatted(output, "   #include <os2.h>\n");
            WriteFormatted(output, "#endif\n\n");
            WriteFormatted(output, "#ifndef BS_NOTEBOOK\n");
            WriteFormatted(output, "   #define BS_NOTEBOOK 8\n");
            WriteFormatted(output, "#endif\n\n");
            WriteFormatted(output, "#ifndef FCF_CLOSEBUTTON\n");
            WriteFormatted(output, "   #define FCF_CLOSEBUTTON 0x04000000L\n");
            WriteFormatted(output, "#endif\n\n");

            int endOfFilePtr = resData.Length;

            WriteFormatted(output, "CODEPAGE {0}\n", ReadUInt16(resData, 4));
            FormatMemoryFlags(ReadUInt16(resData, 0));
            WriteFormatted(output, "DLGTEMPLATE {0}{1}\nBEGIN\n", ReadUInt16(resData, 0), outputBuffer2.ToString());

            int resourceDataPtr = ReadUInt16(resData, 6);
            ProcessBlock(ref resourceDataPtr, resData, 0, 4);
            WriteFormatted(output, "END\n");

            if (nullsDetected)
            {
                WriteFormatted(output, "/* Warning: Nulls detected in strings */\n");
            }

            return output.ToString().Replace("\n", "\r\n");
        }

        private static void FormatControlParameters(int parameterBlockPtr, byte[] buffer, ushort indentationLevel)
        {
            WriteFormatted(output, "{0}PRESPARAMS ", new string(' ', indentationLevel));

            uint paramValue = ReadUInt32(buffer, parameterBlockPtr);
            if (paramValue > 0x32u)
            {
                WriteFormatted(output, "{0}, ", paramValue);
            }
            else
            {
                WriteFormatted(output, "{0}, ", par_pp[paramValue]);
            }

            if (paramValue == 15)
            {
                // Extract string from buffer
                string str = Encoding.ASCII.GetString(buffer, parameterBlockPtr + 8,
                    Array.IndexOf(buffer, (byte)0, parameterBlockPtr + 8) - (parameterBlockPtr + 8));
                WriteFormatted(output, "\"{0}\"\n", str);
                return;
            }

            uint paramCount = ReadUInt32(buffer, parameterBlockPtr + 4) >> 2;
            for (uint i = 0; i < paramCount; i++)
            {
                string prefix = (i > 0) ? ", " : "";
                uint paramDataValue = ReadUInt32(buffer, parameterBlockPtr + 8 + (int)(4 * i));
                WriteFormatted(output, "{0}0x{1:X8}", prefix, paramDataValue);
            }

            uint paramFlagsAndCount = ReadUInt32(buffer, parameterBlockPtr + 4);
            uint flags = paramFlagsAndCount & 3;
            switch (flags)
            {
                case 1u:
                    string prefix1 = (paramCount > 0) ? ", " : "";
                    byte byteValue = buffer[parameterBlockPtr + 8 + (int)(4 * paramCount)];
                    WriteFormatted(output, "{0}0x{1:X8}", prefix1, byteValue);
                    break;
                case 2u:
                    string prefix2 = (paramCount > 0) ? ", " : "";
                    ushort ushortValue = ReadUInt16(buffer, parameterBlockPtr + 8 + (int)(4 * paramCount));
                    WriteFormatted(output, "{0}0x{1:X8}", prefix2, ushortValue);
                    break;
                case 3u:
                    string prefix3 = (paramCount > 0) ? ", " : "";
                    uint uintValue = ReadUInt32(buffer, parameterBlockPtr + 8 + (int)(4 * paramCount)) & 0xFFFFFF;
                    WriteFormatted(output, "{0}0x{1:X8}", prefix3, uintValue);
                    break;
            }
            WriteFormatted(output, "\n");
        }

        private static void WriteFormatted2(byte[] buffer, int resourcePtr, uint dataLength)
        {
            uint wordCount = dataLength >> 1;
            for (uint i = 0; i < wordCount; i++)
            {
                if (i > 0)
                {
                    WriteFormatted(output, ", ");
                }
                ushort currentWord = ReadUInt16(buffer, resourcePtr + (int)(2 * i));
                WriteFormatted(output, "{0}", currentWord);
            }
            WriteFormatted(output, "\n");
        }

        private static uint FormatAndPrintIndexedStyle(uint result, string[] styleNamesTable, ushort styleTableSize)
        {
            if (result != 0)
            {
                if (outputBuffer.Length > 0)
                {
                    outputBuffer.Append(" | ");
                }

                string styleName;
                if (styleTableSize <= result || (result < styleNamesTable.Length && styleNamesTable[result] == null))
                {
                    styleName = $"0x{result:X}";
                }
                else
                {
                    styleName = styleNamesTable[result];
                }

                outputBuffer.Append(styleName);
                result = 0;
            }
            return result;
        }

        private static void FormatAndPrintStyles(int currentStyle, int defaultStyle, string[] styleNamesTablePtr, ushort bitCount, short direction)
        {
            int originalCurrentStyle = currentStyle;
            int styleDifference = originalCurrentStyle ^ defaultStyle;

            short bitIncrement = (direction == -1) ? (short)-1 : (short)1;
            ushort initialBitIndex = (direction == -1) ? (ushort)(bitCount - 1) : (ushort)0;

            if (bitCount > 0)
            {
                ushort processedBits = 0;
                ushort currentBitIndex = initialBitIndex;

                do
                {
                    if (((1 << currentBitIndex) & styleDifference) != 0)
                    {
                        if (outputBuffer.Length > 0)
                        {
                            outputBuffer.Append(" | ");
                        }

                        if (((1 << currentBitIndex) & originalCurrentStyle) == 0)
                        {
                            outputBuffer.Append("NOT ");
                        }

                        string styleName;
                        if (styleNamesTablePtr != null && currentBitIndex < styleNamesTablePtr.Length &&
                            styleNamesTablePtr[currentBitIndex] != null)
                        {
                            styleName = styleNamesTablePtr[currentBitIndex];
                        }
                        else
                        {
                            styleName = $"0x{1 << currentBitIndex:X}";
                        }

                        outputBuffer.Append(styleName);
                    }
                    currentBitIndex = (ushort)(currentBitIndex + bitIncrement);
                    processedBits++;
                } while (bitCount > processedBits);
            }
        }

        private static string GetControlName(ushort controlType, short controlStyle, out uint defaultStylePtr, out ushort textFormatFlagPtr)
        {
            defaultStylePtr = 0;
            textFormatFlagPtr = 1;

            string controlName;
            if (controlType != 0 && controlType < 0x42u && controlType < par_wc.Length && par_wc[controlType] != null)
            {
                controlName = par_wc[controlType];
            }
            else
            {
                idBuffer.Clear();
                idBuffer.AppendFormat("((PSZ)0x{0:X8}L)", controlType);
                controlName = idBuffer.ToString();
            }

            switch (controlType)
            {
                case 0:
                case 0xB:
                case 0xC:
                case 0xD:
                case 0xE:
                case 0xF:
                case 0x10:
                case 0x11:
                case 0x12:
                case 0x13:
                case 0x14:
                case 0x15:
                case 0x16:
                case 0x17:
                case 0x18:
                case 0x19:
                case 0x1A:
                case 0x1B:
                case 0x1C:
                case 0x1D:
                case 0x1E:
                case 0x1F:
                case 0x21:
                case 0x22:
                case 0x23:
                case 0x24:
                    return null;

                case 1: // Dialog
                    defaultStylePtr = 335544448;
                    return "DIALOG";

                case 2: // Combobox
                    defaultStylePtr = 0x80100001;
                    return "COMBOBOX";

                case 3: // Button
                    int buttonSubType = controlStyle & 0xF;
                    defaultStylePtr = (uint)(buttonSubType | 0x80000000);
                    if ((controlStyle & 0xF) != 0)
                    {
                        switch (buttonSubType)
                        {
                            case 1:
                                defaultStylePtr |= 0x20000;
                                return "CHECKBOX";
                            case 2:
                                defaultStylePtr |= 0x20000;
                                return "AUTOCHECKBOX";
                            case 3:
                                return "RADIOBUTTON";
                            case 4:
                                return "AUTORADIOBUTTON";
                            default:
                                defaultStylePtr = 0;
                                return null;
                        }
                    }
                    else
                    {
                        if ((controlStyle & 0x400) != 0)
                        {
                            defaultStylePtr |= 0x400;
                            return "DEFPUSHBUTTON";
                        }
                        else
                        {
                            return "PUSHBUTTON";
                        }
                    }

                case 4: // Menu
                    defaultStylePtr = 0;
                    return null;

                case 5: // Static
                    uint staticSubType = (uint)((controlStyle & 0x3F) | 0x80000000);
                    defaultStylePtr = staticSubType;
                    switch (staticSubType & 0x3F)
                    {
                        case 1:
                            if ((controlStyle & 0x100) != 0)
                            {
                                defaultStylePtr |= 0x100;
                                return "CTEXT";
                            }
                            else if ((controlStyle & 0x200) != 0)
                            {
                                defaultStylePtr |= 0x200;
                                return "RTEXT";
                            }
                            else
                            {
                                return "LTEXT";
                            }
                        case 2:
                            defaultStylePtr |= 0x10000;
                            return "GROUPBOX";
                        case 3:
                            textFormatFlagPtr = 1;
                            return "ICON";
                        case 4:
                            textFormatFlagPtr = 1;
                            defaultStylePtr = 0;
                            return null;
                        default:
                            defaultStylePtr = 0;
                            return null;
                    }

                case 6: // Entry field
                    defaultStylePtr = 0x80100004;
                    return "ENTRYFIELD";

                case 7: // Listbox
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "LISTBOX";

                case 8: // Scrollbar
                    defaultStylePtr = 0;
                    return null;

                case 9: // Titlebar
                    defaultStylePtr = 0;
                    return null;

                case 0xA: // MLE
                    defaultStylePtr = 0x80100002;
                    return "MLE";

                case 0x20: // Spinbutton
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "SPINBUTTON";

                case 0x25: // Container
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "CONTAINER";

                case 0x26: // Slider
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "SLIDER";

                case 0x27: // Valueset
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "VALUESET";

                case 0x28: // Notebook
                    defaultStylePtr = 0x80100000;
                    textFormatFlagPtr = 0;
                    return "NOTEBOOK";

                default:
                    if (controlType == 65)
                    {
                        defaultStylePtr = 0;
                    }
                    return null;
            }
        }

        private static void ProcessBlock(ref int currentResPtrPtr, byte[] buffer, int resourceBaseOffset, ushort indentationLevel)
        {
            int currentResourcePtr = currentResPtrPtr;
            WriteFormatted(output, "{0}", new string(' ', indentationLevel));

            uint controlDefaultStyle;
            ushort controlTextType;
            string controlName;

            if (ReadUInt16(buffer, currentResourcePtr + 4) != 0)
            {
                // Custom control
                controlName = null;
                controlDefaultStyle = 0;
                ushort textLength = ReadUInt16(buffer, currentResourcePtr + 4);
                int textOffset = ReadUInt16(buffer, currentResourcePtr + 6) + resourceBaseOffset;
                string controlText = Encoding.ASCII.GetString(buffer, textOffset, textLength);
                string stringBufferForName = $"\"{controlText}\"";
                controlTextType = 1;
                WriteFormatted(output, "{0,-16}", "CONTROL");
            }
            else
            {
                // Standard control
                ushort controlType = ReadUInt16(buffer, currentResourcePtr + 6);
                short controlStyle = (short)ReadUInt16(buffer, currentResourcePtr + 12);
                controlName = GetControlName(controlType, controlStyle, out controlDefaultStyle, out controlTextType);

                if (controlTextType == 0 && ReadUInt16(buffer, currentResourcePtr + 8) != 0)
                {
                    controlName = null;
                    controlDefaultStyle = 0;
                    controlTextType = 1;
                }

                if (controlType == 1)
                {
                    WriteFormatted(output, "{0,-8}", controlName ?? "CONTROL");
                }
                else
                {
                    WriteFormatted(output, "{0,-16}", controlName ?? "CONTROL");
                }
            }

            // Handle control text
            if (controlTextType == 1 && ReadUInt16(buffer, currentResourcePtr + 8) != 0 &&
                buffer[resourceBaseOffset + ReadUInt16(buffer, currentResourcePtr + 10)] == 0xFF)
            {
                controlTextType = 2;
            }

            if (controlTextType == 1)
            {
                output.Write("\"");
                int textLength = ReadUInt16(buffer, currentResourcePtr + 8);
                int textOffset = resourceBaseOffset + ReadUInt16(buffer, currentResourcePtr + 10);

                for (int i = 0; i < textLength; i++)
                {
                    byte currentChar = buffer[textOffset + i];
                    switch (currentChar)
                    {
                        case 0:
                            nullsDetected = true;
                            goto case 7;
                        case 1:
                        case 2:
                        case 3:
                        case 4:
                        case 5:
                        case 6:
                            output.Write((char)currentChar);
                            break;
                        case 7:
                        case 8:
                        case 9:
                        case 0xA:
                        case 0xB:
                        case 0xC:
                        case 0xD:
                            WriteFormatted(output, "\\x{0:X2}", currentChar);
                            break;
                        default:
                            if (currentChar == '"')
                            {
                                output.Write("\"\"");
                            }
                            else if (currentChar == '\\')
                            {
                                output.Write("\\\\");
                            }
                            else
                            {
                                output.Write((char)currentChar);
                            }
                            break;
                    }
                }
                output.Write("\", ");
            }
            else if (controlTextType == 2)
            {
                ushort resourceId = ReadUInt16(buffer, resourceBaseOffset + ReadUInt16(buffer, currentResourcePtr + 10) + 1);
                WriteFormatted(output, "{0}, ", resourceId);
            }

            // Format styles
            outputBuffer.Clear();
            ushort highWordStyle = ReadUInt16(buffer, currentResourcePtr + 14);
            uint currentStyle = highWordStyle;
            uint defaultStyle = (controlDefaultStyle >> 16) & 0xFFFF;

            if (highWordStyle != defaultStyle)
            {
                FormatAndPrintStyles((int)highWordStyle, (int)defaultStyle, par_ws, 0x10, -1);
            }

            ushort lowWordStyle = ReadUInt16(buffer, currentResourcePtr + 12);
            currentStyle = lowWordStyle;
            defaultStyle = controlDefaultStyle & 0xFFFF;

            if (lowWordStyle != defaultStyle)
            {
                ushort controlType = ReadUInt16(buffer, currentResourcePtr + 6);
                switch (controlType)
                {
                    case 0:
                    case 4:
                    case 9:
                    case 0xB:
                    case 0xC:
                    case 0xD:
                    case 0xE:
                    case 0xF:
                    case 0x10:
                    case 0x11:
                    case 0x12:
                    case 0x13:
                    case 0x14:
                    case 0x15:
                    case 0x16:
                    case 0x17:
                    case 0x18:
                    case 0x19:
                    case 0x1A:
                    case 0x1B:
                    case 0x1C:
                    case 0x1D:
                    case 0x1E:
                    case 0x1F:
                    case 0x21:
                    case 0x22:
                    case 0x23:
                    case 0x24:
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, null, 0x10, 1);
                        break;
                    case 1: // Frame
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_fs, 0x10, 1);
                        break;
                    case 2: // Combobox
                        uint style2u = ((uint)defaultStyle ^ (uint)currentStyle) >> 12;
                        FormatAndPrintStyles((int)(currentStyle & 0xFFF), (int)(defaultStyle & 0xFFF), par_cbs, 0x10, 1);
                        FormatAndPrintIndexedStyle(style2u, par_es2, 4);
                        break;
                    case 3: // Button
                        uint style3u = (uint)((byte)defaultStyle ^ (byte)currentStyle) & 0xF;
                        FormatAndPrintStyles((int)(currentStyle & 0xFFF0), (int)(defaultStyle & 0xFFF0), par_bs1, 0x10, 1);
                        FormatAndPrintIndexedStyle(style3u, par_bs2, 9);
                        break;
                    case 5: // Static
                        uint style5u = (uint)((byte)defaultStyle ^ (byte)currentStyle) & 0x3F;
                        FormatAndPrintStyles((int)(currentStyle & 0xFFC0), (int)(defaultStyle & 0xFFC0), par_dt, 0x10, 1);
                        FormatAndPrintIndexedStyle(style5u, par_ss, 0xC);
                        break;
                    case 6: // Entry field
                        uint style6u = ((uint)defaultStyle ^ (uint)currentStyle) >> 12;
                        FormatAndPrintStyles((int)(currentStyle & 0xFFF), (int)(defaultStyle & 0xFFF), par_es1, 0x10, 1);
                        FormatAndPrintIndexedStyle(style6u, par_es2, 4);
                        break;
                    case 7: // Listbox
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_ls, 0x10, 1);
                        break;
                    case 8: // Scrollbar
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_sbs, 0x10, 1);
                        break;
                    case 0xA: // MLE
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_mls, 0x10, 1);
                        break;
                    case 0x20: // Spinbutton
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_spbs, 0x10, 1);
                        break;
                    case 0x25: // Container
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_ccs, 0x10, 1);
                        break;
                    case 0x26: // Slider
                        string[] sliderStyles = ((currentStyle & 1) != 0) ? par_sls1 : par_sls2;
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, sliderStyles, 0x10, 1);
                        break;
                    case 0x27: // Valueset
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_vs, 0x10, 1);
                        break;
                    case 0x28: // Notebook
                        FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_bks, 0x10, 1);
                        break;
                    default:
                        if (controlType == 65)
                        {
                            FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, par_css, 0x10, 1);
                        }
                        else
                        {
                            FormatAndPrintStyles((int)currentStyle, (int)defaultStyle, null, 0x10, 1);
                        }
                        break;
                }
            }

            // Write control coordinates and ID
            WriteFormatted(output, "{0}, {1}, {2}, {3}, {4}",
                ReadUInt16(buffer, currentResourcePtr + 24),
                (short)ReadUInt16(buffer, currentResourcePtr + 16),
                (short)ReadUInt16(buffer, currentResourcePtr + 18),
                (short)ReadUInt16(buffer, currentResourcePtr + 20),
                (short)ReadUInt16(buffer, currentResourcePtr + 22));

            if (controlName == null)
            {
                string stringBufferForName;
                if (ReadUInt16(buffer, currentResourcePtr + 4) != 0)
                {
                    ushort textLength = ReadUInt16(buffer, currentResourcePtr + 4);
                    int textOffset = ReadUInt16(buffer, currentResourcePtr + 6) + resourceBaseOffset;
                    string controlText = Encoding.ASCII.GetString(buffer, textOffset, textLength);
                    stringBufferForName = $"\"{controlText}\"";
                }
                else
                {
                    ushort controlType = ReadUInt16(buffer, currentResourcePtr + 6);
                    stringBufferForName = $"((PSZ)0x{controlType:X8}L)";
                }
                WriteFormatted(output, ", {0}", stringBufferForName);
            }

            if (outputBuffer.Length > 0)
            {
                WriteFormatted(output, ", {0}", outputBuffer.ToString());
            }

            // Handle font data for dialogs
            if (ReadUInt16(buffer, currentResourcePtr + 6) == 1) // Dialog
            {
                short fontDataOffset = (short)ReadUInt16(buffer, currentResourcePtr + 28);
                if (fontDataOffset != 0 && fontDataOffset != -1)
                {
                    int fontStyleValue = (int)ReadUInt32(buffer, resourceBaseOffset + ReadUInt16(buffer, currentResourcePtr + 28));
                    if (fontStyleValue != 0)
                    {
                        if (outputBuffer.Length == 0)
                        {
                            WriteFormatted(output, ", ");
                        }
                        outputBuffer.Clear();
                        FormatAndPrintStyles(fontStyleValue, 0, par_fcf, 0x20, -1);
                        WriteFormatted(output, ", {0}", outputBuffer.ToString());
                    }
                }
                WriteFormatted(output, "\n");
            }
            else
            {
                WriteFormatted(output, "\n");

                // Handle control data
                short ctlDataOffset = (short)ReadUInt16(buffer, currentResourcePtr + 28);
                if (ctlDataOffset != 0 && ctlDataOffset != -1)
                {
                    int tempBlockPtr = ReadUInt16(buffer, currentResourcePtr + 28) + resourceBaseOffset;
                    ushort ctlDataSize = ReadUInt16(buffer, tempBlockPtr);
                    if (ctlDataSize != 0)
                    {
                        WriteFormatted(output, "{0}CTLDATA ", new string(' ', indentationLevel + 16));
                        WriteFormatted2(buffer, tempBlockPtr, ctlDataSize);
                    }
                }
            }

            // Handle child controls (presentation parameters)
            short childControlsOffset = (short)ReadUInt16(buffer, currentResourcePtr + 26);
            if (childControlsOffset != 0 && childControlsOffset != -1)
            {
                int childListStartPtr = ReadUInt16(buffer, currentResourcePtr + 26) + resourceBaseOffset;
                ushort childListSize = ReadUInt16(buffer, childListStartPtr);

                if (childListSize != 0)
                {
                    int currentChildPtr = childListStartPtr + 2;
                    int childListEndPtr = childListStartPtr + childListSize;

                    while (currentChildPtr < childListEndPtr)
                    {
                        int incrementValue = (ReadUInt16(buffer, currentResourcePtr + 6) == 1) ? 8 : 16;
                        FormatControlParameters(currentChildPtr, buffer, (ushort)(incrementValue + indentationLevel));
                        currentChildPtr += (int)(ReadUInt32(buffer, currentChildPtr + 4) + 8);
                    }
                }
            }

            // Handle child controls (nested controls)
            ushort childCount = ReadUInt16(buffer, currentResourcePtr + 2);
            if (childCount != 0)
            {
                currentResPtrPtr += 30;
                WriteFormatted(output, "{0}BEGIN\n", new string(' ', indentationLevel));

                for (ushort i = 1; i < childCount; i++)
                {
                    ProcessBlock(ref currentResPtrPtr, buffer, resourceBaseOffset, (ushort)(indentationLevel + 4));
                }

                WriteFormatted(output, "{0}END\n", new string(' ', indentationLevel));
            }
            else
            {
                currentResPtrPtr += 30;
            }
        }

        private static void FormatMemoryFlags(int memoryFlags)
        {
            outputBuffer2.Clear();

            string loadOption;
            if ((memoryFlags & 0x40) != 0 || (memoryFlags & 0x1010) == 0)
            {
                loadOption = ((memoryFlags & 0x40) != 0) ? "PRELOAD" : "LOADONCALL";
                string moveableOption = ((memoryFlags & 0x10) != 0) ? " MOVEABLE" : "";
                string discardableOption = ((memoryFlags & 0x1000) != 0) ? " DISCARDABLE" : "";
                string fixedOption = ((memoryFlags & 0x1010) != 0) ? "" : " FIXED";

                outputBuffer2.AppendFormat(" {0}{1}{2}{3}", loadOption, moveableOption, discardableOption, fixedOption);
            }
        }
    }
}