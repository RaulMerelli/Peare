using System;
using System.Collections.Generic;
using System.Text;

namespace PeareModule
{
    // WSZ structure based on the format research by Tim De Baets:
    // https://github.com/tdebaets/wmp-wsz-format
    // This C# implementation was written for Peare and does not depend on WMP,
    // its COM type library, or registry entries.
    internal static class WszDecoder
    {
        private const byte ElementNamed = 0x00;
        private const byte ElementButton = 0x03;
        private const byte ElementTheme = 0x0B;
        private const byte ElementView = 0x0C;
        private const byte ElementSubview = 0x0D;
        private const byte ElementClsidMask = 0x80;

        public static bool TryDecode(byte[] data, out string decoded)
        {
            decoded = null;
            if (!LooksLikeWsz(data))
                return false;

            try
            {
                decoded = new Parser(data).Decode();
                return true;
            }
            catch (WszFormatException ex)
            {
                Console.WriteLine("Header matched a Windows Media Player WSZ skin, but decoding failed: " + ex.Message);
                return false;
            }
            catch (OverflowException ex)
            {
                Console.WriteLine("Header matched a Windows Media Player WSZ skin, but an offset overflowed: " + ex.Message);
                return false;
            }
        }

        private static bool LooksLikeWsz(byte[] data)
        {
            if (data == null || data.Length < 9)
                return false;

            // The root must be a THEME element. It has no sibling and its
            // predefined-element padding word is zero.
            ushort nextSiblingOffset = ReadUInt16(data, 0);
            ushort firstChildOffset = ReadUInt16(data, 2);
            byte elementType = data[4];
            byte childCount = data[5];

            if (nextSiblingOffset != 0 || elementType != ElementTheme ||
                data[7] != 0 || data[8] != 0)
                return false;

            if (childCount == 0)
                return firstChildOffset == 0;

            if (firstChildOffset == 0)
                return false;

            // Element offsets are measured from the start of the element.
            int firstChildPosition = firstChildOffset;
            return firstChildPosition >= 9 && firstChildPosition < data.Length;
        }

        private static ushort ReadUInt16(byte[] data, int offset)
        {
            return (ushort)(data[offset] | (data[offset + 1] << 8));
        }

        private sealed class Parser
        {
            private const int MaximumDepth = 256;
            private const int MaximumRecords = 1000000;

            private static readonly IDictionary<Guid, string> KnownObjects =
                new Dictionary<Guid, string>
                {
                    { new Guid("8856F961-340A-11D0-A96B-00C04FD705A2"), "browser" },
                    { new Guid("AA1AC37B-49A8-4B41-AF69-B0176C5FFC33"), "plugin" },
                    { new Guid("A8A55FAC-82EA-4BD7-BD7B-11586A4D99E4"), "Skinlist" },
                    { new Guid("395BF287-6477-495F-8427-2C09A23C3248"), "taskcenter" }
                };

            private readonly byte[] data;
            private readonly StringBuilder output;
            private int position;
            private int recordsParsed;

            public Parser(byte[] data)
            {
                this.data = data;
                output = new StringBuilder();
            }

            public string Decode()
            {
                output.AppendLine("Windows Media Player WSZ skin");
                ParseElement(0, data.Length, true);

                if (position < data.Length)
                    SkipZeroPadding(data.Length, "trailing data");

                if (position != data.Length)
                    Fail("The parser did not consume the complete resource.");

                return output.ToString().TrimEnd();
            }

            private void ParseElement(int level, int parentEnd, bool isLastSibling)
            {
                if (level > MaximumDepth)
                    Fail("The element nesting depth is excessive.");
                CountRecord();

                int start = position;
                ushort nextSiblingOffset = ReadUInt16(parentEnd);
                ushort firstChildOffset = ReadUInt16(parentEnd);
                byte elementType = ReadByte(parentEnd);
                byte childCount = ReadByte(parentEnd);
                byte attributeCount = ReadByte(parentEnd);

                if (level == 0)
                {
                    if (elementType != ElementTheme)
                        FailAt(start, "The root element is not THEME.");
                    if (nextSiblingOffset != 0)
                        FailAt(start, "The root element unexpectedly has a sibling offset.");
                }
                else if (elementType == ElementTheme)
                {
                    FailAt(start, "A nested THEME element is not valid.");
                }

                if (isLastSibling && nextSiblingOffset != 0)
                    FailAt(start, "The final sibling has a non-zero next offset.");
                if (!isLastSibling && nextSiblingOffset == 0)
                    FailAt(start, "A non-final sibling has a zero next offset.");

                int elementEnd = nextSiblingOffset == 0
                    ? parentEnd
                    : CheckedOffset(start, nextSiblingOffset, "next element");

                if (elementEnd <= start || elementEnd > parentEnd)
                    FailAt(start, "The next-element offset is outside its parent.");

                int firstChildPosition = -1;
                if (firstChildOffset != 0)
                    firstChildPosition = CheckedOffset(start, firstChildOffset, "first child");

                if (childCount == 0 && firstChildOffset != 0)
                    FailAt(start, "An element without children has a first-child offset.");
                if (childCount != 0 && firstChildOffset == 0)
                    FailAt(start, "An element with children has no first-child offset.");

                int headerEnd = firstChildOffset == 0 ? elementEnd : firstChildPosition;
                if (headerEnd < position || headerEnd > elementEnd)
                    FailAt(start, "The first-child offset overlaps the element header or exceeds the element.");

                string elementName;
                string elementDetails = null;

                switch (elementType)
                {
                    case ElementNamed:
                    case ElementButton:
                        elementName = ReadWideString(headerEnd);
                        bool isButtonName = String.Equals(elementName, "buttonelement", StringComparison.OrdinalIgnoreCase);
                        if (elementType == ElementNamed && isButtonName)
                            FailAt(start, "BUTTONELEMENT uses the wrong element ID.");
                        if (elementType == ElementButton && !isButtonName)
                            FailAt(start, "Element ID 0x03 is reserved for BUTTONELEMENT.");
                        break;

                    case ElementTheme:
                        ReadPaddingWord(headerEnd);
                        elementName = "theme";
                        break;

                    case ElementView:
                        ReadPaddingWord(headerEnd);
                        elementName = "view";
                        break;

                    case ElementSubview:
                        ReadPaddingWord(headerEnd);
                        elementName = "subview";
                        break;

                    default:
                        if ((elementType & ElementClsidMask) == 0)
                            FailAt(start, "Unrecognized element ID 0x" + elementType.ToString("X2") + ".");

                        ReadPaddingWord(headerEnd);
                        Guid clsid = ReadGuid(headerEnd);
                        string objectName;
                        if (KnownObjects.TryGetValue(clsid, out objectName))
                            elementName = objectName;
                        else
                            elementName = "object";
                        elementDetails = clsid.ToString("B").ToUpperInvariant();
                        break;
                }

                AppendIndent(level);
                output.Append(elementName);
                output.Append(" @0x");
                output.Append(start.ToString("X8"));
                output.Append(" [id=0x");
                output.Append(elementType.ToString("X2"));
                output.Append(", children=");
                output.Append(childCount);
                output.Append(", attributes=");
                output.Append(attributeCount);
                output.Append(']');
                if (!String.IsNullOrEmpty(elementDetails))
                {
                    output.Append(' ');
                    output.Append(elementDetails);
                }
                output.AppendLine();

                for (int i = 0; i < attributeCount; i++)
                    ParseAttribute(level + 1, headerEnd, i == attributeCount - 1);

                if (position != headerEnd)
                    FailAt(position, "The element header does not end at its first-child/next-element offset.");

                for (int i = 0; i < childCount; i++)
                    ParseElement(level + 1, elementEnd, i == childCount - 1);

                if (position < elementEnd)
                    SkipZeroPadding(elementEnd, "element padding");
                if (position != elementEnd)
                    FailAt(position, "The element exceeds its declared boundary.");
            }

            private void ParseAttribute(int level, int elementHeaderEnd, bool isLastAttribute)
            {
                CountRecord();

                int start = position;
                ushort nextAttributeOffset = ReadUInt16(elementHeaderEnd);
                if (isLastAttribute && nextAttributeOffset != 0)
                    FailAt(start, "The final attribute has a non-zero next offset.");
                if (!isLastAttribute && nextAttributeOffset == 0)
                    FailAt(start, "A non-final attribute has a zero next offset.");

                int attributeEnd = nextAttributeOffset == 0
                    ? elementHeaderEnd
                    : CheckedOffset(start, nextAttributeOffset, "next attribute");

                if (attributeEnd <= position || attributeEnd > elementHeaderEnd)
                    FailAt(start, "The next-attribute offset is invalid.");

                byte attributeType = ReadByte(attributeEnd);
                string description;

                switch (attributeType)
                {
                    case 0x00:
                        description = FormatNamedAttribute("named", attributeEnd);
                        break;

                    case 0xE0:
                        description = FormatNamedAttribute("event", attributeEnd);
                        break;

                    case 0x80:
                        ushort unknown = ReadUInt16(attributeEnd);
                        description = FormatNamedAttribute("JScript, unknown=" + unknown, attributeEnd);
                        break;

                    case 0x40:
                        int namedAddend = ReadInt32(attributeEnd);
                        string namedProperty = ReadWideString(attributeEnd);
                        string namedValue = ReadWideString(attributeEnd);
                        description = "wmpprop " + Quote(namedProperty) + " = " +
                                      Quote(FormatWmpPropValue(namedValue, namedAddend));
                        break;

                    case 0x48:
                    case 0xC8:
                        // The documented layout contains a four-byte addend before
                        // the dispid. Real WMP resources also use a compact layout
                        // that starts directly with dispid + padding. Detect both
                        // from the position of the required zero padding word.
                        int addend = 0;
                        ushort propertyDispId;
                        int valueStart = position;

                        if (HasZeroWord(valueStart + 6, attributeEnd) &&
                            IsWideStringToEnd(valueStart + 8, attributeEnd))
                        {
                            addend = ReadInt32(attributeEnd);
                            propertyDispId = ReadUInt16(attributeEnd);
                            ReadPaddingWord(attributeEnd);
                        }
                        else if (HasZeroWord(valueStart + 2, attributeEnd) &&
                                 IsWideStringToEnd(valueStart + 4, attributeEnd))
                        {
                            propertyDispId = ReadUInt16(attributeEnd);
                            ReadPaddingWord(attributeEnd);
                        }
                        else
                        {
                            FailAt(valueStart, "Invalid wmpprop attribute layout.");
                            propertyDispId = 0; // unreachable
                        }

                        string propertyValue = ReadWideString(attributeEnd);
                        description = (attributeType == 0x48 ? "wmpprop" : "wmpprop2") +
                                      " dispid=" + propertyDispId + " value=" +
                                      Quote(FormatWmpPropValue(propertyValue, addend));
                        break;

                    default:
                        description = ParseUnnamedAttribute(attributeType, attributeEnd);
                        break;
                }

                if (position != attributeEnd)
                    FailAt(position, "The attribute length does not match its next offset.");

                AppendIndent(level);
                output.Append("attribute @0x");
                output.Append(start.ToString("X8"));
                output.Append(" [type=0x");
                output.Append(attributeType.ToString("X2"));
                output.Append("] ");
                output.AppendLine(description);
            }

            private string FormatNamedAttribute(string kind, int attributeEnd)
            {
                string name = ReadWideString(attributeEnd);
                string value = ReadWideString(attributeEnd);
                return kind + " " + Quote(name) + " = " + Quote(value);
            }

            private string ParseUnnamedAttribute(byte attributeType, int attributeEnd)
            {
                ushort dispId = ReadUInt16(attributeEnd);
                ReadPaddingWord(attributeEnd);
                string value;

                switch (attributeType)
                {
                    case 0x01:
                        ushort boolValue = ReadUInt16(attributeEnd);
                        value = boolValue == 0 ? "false" : boolValue == 1 ? "true" : "true (raw=" + boolValue + ")";
                        return "boolean dispid=" + dispId + " value=" + value;

                    case 0x04:
                        return "integer dispid=" + dispId + " value=" + ReadInt32(attributeEnd);

                    case 0x18:
                        return "resourcestring dispid=" + dispId + " id=" + ReadInt32(attributeEnd);

                    case 0x08:
                        value = ReadWideString(attributeEnd);
                        return "string dispid=" + dispId + " value=" + Quote(value);

                    case 0x28:
                        value = ReadWideString(attributeEnd);
                        return "wmpenabled dispid=" + dispId + " value=" + Quote(value);

                    case 0x0D:
                        value = ReadWideString(attributeEnd);
                        return "sysint dispid=" + dispId + " value=" + Quote(value);

                    case 0x88:
                        value = ReadWideString(attributeEnd);
                        return "JScript dispid=" + dispId + " value=" + Quote(value);

                    default:
                        FailAt(position - 1, "Unrecognized attribute type 0x" + attributeType.ToString("X2") + ".");
                        return null;
                }
            }

            private byte ReadByte(int limit)
            {
                EnsureAvailable(1, limit);
                return data[position++];
            }

            private ushort ReadUInt16(int limit)
            {
                EnsureAvailable(2, limit);
                ushort value = (ushort)(data[position] | (data[position + 1] << 8));
                position += 2;
                return value;
            }

            private int ReadInt32(int limit)
            {
                EnsureAvailable(4, limit);
                int value = data[position] |
                            (data[position + 1] << 8) |
                            (data[position + 2] << 16) |
                            (data[position + 3] << 24);
                position += 4;
                return value;
            }

            private Guid ReadGuid(int limit)
            {
                EnsureAvailable(16, limit);
                byte[] guidBytes = new byte[16];
                Buffer.BlockCopy(data, position, guidBytes, 0, guidBytes.Length);
                position += guidBytes.Length;
                return new Guid(guidBytes);
            }

            private string ReadWideString(int limit)
            {
                int start = position;
                while (position + 1 < limit)
                {
                    if (data[position] == 0 && data[position + 1] == 0)
                    {
                        int byteLength = position - start;
                        string value = Encoding.Unicode.GetString(data, start, byteLength);
                        position += 2;
                        return value;
                    }
                    position += 2;
                }

                FailAt(start, "A Unicode string is not zero-terminated within its boundary.");
                return null;
            }

            private void ReadPaddingWord(int limit)
            {
                ushort padding = ReadUInt16(limit);
                if (padding != 0)
                    FailAt(position - 2, "A reserved padding word is non-zero.");
            }

            private bool HasZeroWord(int offset, int limit)
            {
                return offset >= 0 && offset <= limit - 2 &&
                       data[offset] == 0 && data[offset + 1] == 0;
            }

            private bool IsWideStringToEnd(int offset, int end)
            {
                if (offset < 0 || end > data.Length || offset > end - 2 ||
                    ((end - offset) & 1) != 0)
                {
                    return false;
                }

                for (int i = offset; i + 1 < end; i += 2)
                {
                    if (data[i] == 0 && data[i + 1] == 0)
                        return i + 2 == end;
                }

                return false;
            }

            private void EnsureAvailable(int count, int limit)
            {
                if (limit < 0 || limit > data.Length || position < 0 ||
                    count < 0 || position > limit - count)
                {
                    FailAt(position, "Unexpected end of WSZ data.");
                }
            }

            private void SkipZeroPadding(int end, string description)
            {
                while (position < end && data[position] == 0)
                    position++;

                if (position != end)
                    FailAt(position, "Unexpected non-zero " + description + ".");
            }

            private void CountRecord()
            {
                recordsParsed++;
                if (recordsParsed > MaximumRecords)
                    Fail("The resource contains too many elements or attributes.");
            }

            private static int CheckedOffset(int basePosition, ushort relativeOffset, string description)
            {
                try
                {
                    return checked(basePosition + relativeOffset);
                }
                catch (OverflowException)
                {
                    throw new OverflowException("The " + description + " offset is too large.");
                }
            }

            private static string FormatWmpPropValue(string value, int addend)
            {
                if (addend > 0)
                    return value + "+" + addend;
                if (addend < 0)
                    return value + addend;
                return value;
            }

            private static string Quote(string value)
            {
                if (value == null)
                    return "<null>";

                StringBuilder result = new StringBuilder(value.Length + 2);
                result.Append('"');
                foreach (char ch in value)
                {
                    switch (ch)
                    {
                        case '\\': result.Append("\\\\"); break;
                        case '"': result.Append("\\\""); break;
                        case '\r': result.Append("\\r"); break;
                        case '\n': result.Append("\\n"); break;
                        case '\t': result.Append("\\t"); break;
                        default:
                            if (Char.IsControl(ch))
                                result.Append("\\u" + ((int)ch).ToString("X4"));
                            else
                                result.Append(ch);
                            break;
                    }
                }
                result.Append('"');
                return result.ToString();
            }

            private void AppendIndent(int level)
            {
                output.Append(' ', level * 2);
            }

            private static void Fail(string message)
            {
                throw new WszFormatException(message);
            }

            private static void FailAt(int offset, string message)
            {
                throw new WszFormatException("Offset 0x" + offset.ToString("X8") + ": " + message);
            }
        }

        private sealed class WszFormatException : Exception
        {
            public WszFormatException(string message)
                : base(message)
            {
            }
        }
    }
}
