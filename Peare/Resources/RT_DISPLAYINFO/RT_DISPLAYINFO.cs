using System.Text;

namespace Peare
{
    // Special thanks to bitsavers.trailing-edge.com rfor proving some IBM docs containig useful info
    // https://bitsavers.trailing-edge.com/pdf/ibm/pc/os2/OS2_2.x/IBM_OS2_2.0_Technical_Library_1992/S10G-6267-00_OS2_2.0_Presentation_Driver_Reference_199203.pdf
    // page 244
    public class RT_DISPLAYINFO
    {
        public static string Get(byte[] data)
        {
            if (data == null || data.Length < 26)
                return "Invalid data (must be at least 26 bytes).";

            ushort ReadUShort(int offset) => (ushort)(data[offset] | (data[offset + 1] << 8));

            ushort cb = ReadUShort(0x00);
            ushort cxIcon = ReadUShort(0x02);
            ushort cyIcon = ReadUShort(0x04);
            ushort cxPointer = ReadUShort(0x06);
            ushort cyPointer = ReadUShort(0x08);
            ushort cxBorder = ReadUShort(0x0A);
            ushort cyBorder = ReadUShort(0x0C);
            ushort cxHSlider = ReadUShort(0x0E);
            ushort cyVSlider = ReadUShort(0x10);
            ushort cxSizeBorder = ReadUShort(0x12);
            ushort cySizeBorder = ReadUShort(0x14);
            ushort cxDeviceAlign = ReadUShort(0x16);
            ushort cyDeviceAlign = ReadUShort(0x18);

            var sb = new StringBuilder();
            sb.AppendLine("RT_DISPLAYINFO");
            sb.AppendLine("{");
            sb.AppendLine($"\tSize:              {cb} bytes");
            sb.AppendLine($"\tIcon Size:         {cxIcon} x {cyIcon} px");
            sb.AppendLine($"\tPointer Size:      {cxPointer} x {cyPointer} px");
            sb.AppendLine($"\tBorder Size:       {cxBorder} x {cyBorder} px");
            sb.AppendLine($"\tSlider Size:       {cxHSlider} (H) x {cyVSlider} (V) px");
            sb.AppendLine($"\tSize Border:       {cxSizeBorder} x {cySizeBorder} px");
            sb.AppendLine($"\tDevice Alignment:  {cxDeviceAlign} x {cyDeviceAlign} px");
            sb.AppendLine("}");

            return sb.ToString();
        }
    }
}
