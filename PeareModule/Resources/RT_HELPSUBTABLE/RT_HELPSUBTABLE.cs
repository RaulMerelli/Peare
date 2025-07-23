using System;

namespace PeareModule
{
    public static class RT_HELPSUBTABLE
    {
        public static string Get(byte[] data)
        {
            if (data == null || data.Length < 2)
            {
                return ""; // Or throw an exception, depending on desired error handling
            }

            int size = BitConverter.ToUInt16(data, 0);
            if (size == 0) // Handle cases where size might be 0, though not explicitly mentioned for HELPSUBTABLE.
                return "";

            System.Text.StringBuilder sb = new System.Text.StringBuilder();
            sb.Append("RT_HELPSUBTABLE\r\n{\r\n");

            int offset = 2; // Start after the 'size' field

            // Each subitem has 'size' integers, and we know wnd and help are the first two
            int subItemSizeInBytes = size * 2; // Each integer is 16 bits (2 bytes)

            while (offset + subItemSizeInBytes <= data.Length)
            {
                int wnd = BitConverter.ToUInt16(data, offset);
                int help = BitConverter.ToUInt16(data, offset + 2);

                sb.Append($"  {wnd}, {help}");

                // If size is more than 2, append the remaining integers
                for (int i = 2; i < size; i++)
                {
                    if (offset + (i * 2) + 2 <= data.Length) // Ensure we don't go out of bounds
                    {
                        int additionalValue = BitConverter.ToUInt16(data, offset + (i * 2));
                        sb.Append($", {additionalValue}");
                    }
                    else
                    {
                        // Data unexpectedly ends early for an additional value
                        // This might indicate corrupted data or an unexpected format.
                        // For now, we'll break and process what we have.
                        break;
                    }
                }
                sb.Append("\r\n");
                offset += subItemSizeInBytes;
            }

            sb.Append("}\r\n");
            return sb.ToString();
        }
    }
}