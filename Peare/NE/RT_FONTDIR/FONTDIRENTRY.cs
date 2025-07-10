using System.Runtime.InteropServices;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct FONTDIRENTRY
{
    public ushort dfVersion;          // 0x00 - 2 bytes
    public uint dfSize;               // 0x02 - 4 bytes

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 60)]
    public byte[] dfCopyright;       // 0x06 - 60 bytes

    public ushort dfType;            // 0x42 - 2 bytes
    public ushort dfPoints;          // 0x44 - 2 bytes
    public ushort dfVertRes;         // 0x46 - 2 bytes
    public ushort dfHorizRes;        // 0x48 - 2 bytes
    public ushort dfAscent;          // 0x4A - 2 bytes
    public ushort dfInternalLeading; // 0x4C - 2 bytes
    public ushort dfExternalLeading; // 0x4E - 2 bytes

    public byte dfItalic;            // 0x50 - 1 byte
    public byte dfUnderline;         // 0x51 - 1 byte
    public byte dfStrikeOut;         // 0x52 - 1 byte

    public ushort dfWeight;          // 0x53 - 2 bytes (warning: misaligned!)

    public byte dfCharSet;           // 0x55 - 1 byte
    public ushort dfPixWidth;        // 0x56 - 2 bytes
    public ushort dfPixHeight;       // 0x58 - 2 bytes
    public byte dfPitchAndFamily;    // 0x5A - 1 byte
    public ushort dfAvgWidth;        // 0x5B - 2 bytes
    public ushort dfMaxWidth;        // 0x5D - 2 bytes
    public byte dfFirstChar;         // 0x5F - 1 byte
    public byte dfLastChar;          // 0x60 - 1 byte
    public byte dfDefaultChar;       // 0x61 - 1 byte
    public byte dfBreakChar;         // 0x62 - 1 byte
    public ushort dfWidthBytes;      // 0x63 - 2 bytes
    public uint dfDevice;            // 0x65 - 4 bytes
    public uint dfFace;              // 0x69 - 4 bytes
    public uint dfReserved;          // 0x6D - 4 bytes

    // The structure ends here at 0x71 (decimal 113), so the string fields start from offset 0x71

    // Variable-length strings must be read separately:
    // szDeviceName (null-terminated string)
    // szFaceName (null-terminated string), immediately following szDeviceName
}
