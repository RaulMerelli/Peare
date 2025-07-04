using System.Runtime.InteropServices;

// Special thanks to ReactOS project
// This is a direct conversion of their C struct
// https://doxygen.reactos.org/d8/d12/bmfd_8h_source.html#l00177
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct FONTINFO16
{
    public ushort dfVersion;               // WORD = 2 bytes
    public uint dfSize;                    // DWORD = 4 bytes

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 60)]
    public byte[] dfCopyright;             // CHAR[60] = 60 bytes

    public ushort dfType;                  // WORD
    public ushort dfPoints;                // WORD
    public ushort dfVertRes;               // WORD
    public ushort dfHorizRes;              // WORD
    public ushort dfAscent;                // WORD
    public ushort dfInternalLeading;       // WORD
    public ushort dfExternalLeading;       // WORD

    public byte dfItalic;                  // BYTE
    public byte dfUnderline;               // BYTE
    public byte dfStrikeOut;               // BYTE

    public ushort dfWeight;                // WORD

    public byte dfCharSet;                 // BYTE
    public ushort dfPixWidth;              // WORD
    public ushort dfPixHeight;             // WORD
    public byte dfPitchAndFamily;          // BYTE

    public ushort dfAvgWidth;              // WORD
    public ushort dfMaxWidth;              // WORD

    public byte dfFirstChar;               // BYTE
    public byte dfLastChar;                // BYTE
    public byte dfDefaultChar;             // BYTE
    public byte dfBreakChar;               // BYTE

    public ushort dfWidthBytes;            // WORD
    public uint dfDevice;                  // DWORD
    public uint dfFace;                    // DWORD
    public uint dfBitsPointer;             // DWORD
    public uint dfBitsOffset;              // DWORD

    public byte dfReserved;                // BYTE

    // Version 3.00 fields:
    public uint dfFlags;                   // DWORD
    public ushort dfAspace;                // WORD
    public ushort dfBspace;                // WORD
    public ushort dfCspace;                // WORD
    public uint dfColorPointer;            // DWORD

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
    public uint[] dfReserved1;             // DWORD[4]

    [MarshalAs(UnmanagedType.ByValArray, SizeConst = 1)]
    public byte[] dfCharTable;             // BYTE[1], placeholder for variable length
}
