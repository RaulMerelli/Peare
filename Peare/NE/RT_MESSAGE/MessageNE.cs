using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace Peare
{
    public static class MessageNE
    {
        public static string Get(byte[] data)
        {
            //Program.DumpRaw(data);

            int offset = 0;
            if (data.Length >= 2 && data[0] == 0xB5 && data[1] == 0x01)
            {
                offset = 2;  // Skip first two bytes for OS/2
            }
            StringBuilder output = new StringBuilder();
            output.AppendLine("MESSAGETABLE");
            output.AppendLine("{");

            while (offset + 1 < data.Length)
            {
                ushort msgId = BitConverter.ToUInt16(data, offset);
                offset += 1;

                int end = Array.IndexOf<byte>(data, 0x00, offset);
                if (end == -1)
                    break;

                int length = end - offset;
                if (length < 0)
                    break;

                string message = Encoding.ASCII.GetString(data, offset, length);
                message = message.Replace("\"", "\\\"");

                output.AppendLine($"\t0x{msgId:X4}, \"{message}\"");

                offset = end + 1;
            }

            output.AppendLine("}");
            return output.ToString();
        }

    }
}
