using System.Runtime.InteropServices;

namespace PeareModule
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct LX_OBJECT_TABLE_ENTRY
    {
        public uint VirtualSize;
        public uint BaseRelocAddress;
        public LX_OBJECT_FLAGS ObjectFlags;
        public uint PageTableIndex;
        public uint PageTableEntries;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 4)]
        public char[] Reserved;
    }
}
