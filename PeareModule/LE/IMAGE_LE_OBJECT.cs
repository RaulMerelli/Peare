using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct IMAGE_LE_OBJECT
    {
        public uint VirtualSize;
        public uint RelocBaseAddr; // Was BaseRelocAddress in C
        public uint Flags;         // Was ObjectFlags in C
        public uint PageTableIndex;
        public uint PageTableEntries;
        public uint Reserved;      // Was char Reserved[4] in C
    }
}