using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct LE_OBJECT_PAGE_TABLE_ENTRY 
    {
        public byte unknown1;
        public byte unknown2;
        public byte PageTableIndex; // For example can be 55
        public byte Flags;           // Represents the 8-bit flags (0 is not compressed for example)
    }
}