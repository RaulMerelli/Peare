using System.Runtime.InteropServices;

namespace Peare
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct IMAGE_LX_HEADER
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
        public char[] Signature;                  // 00h
        public byte ByteOrder;                    // 02h
        public byte WordOrder;                    // 03h
        public uint FormatLevel;                  // 04h
        public ushort CPUType;                    // 08h
        public ushort OSType;                     // 0Ah
        public uint ModuleVersion;                // 0Ch
        public uint ModuleFlags;                  // 10h
        public uint ModuleNumPages;               // 14h
        public uint EipObjectNum;                 // 18h
        public uint EIP;                          // 1Ch
        public uint EspObjectNum;                 // 20h
        public uint ESP;                          // 24h
        public uint PageSize;                     // 28h
        public uint PageOffsetShift;              // 2Ch
        public uint FixupSectionSize;             // 30h
        public uint FixupSectionChecksum;         // 34h
        public uint LoaderSectionSize;            // 38h
        public uint LoaderSectionChecksum;        // 3Ch
        public uint ObjectTableOffset;            // 40h
        public uint NumObjectsInModule;           // 44h
        public uint ObjectPageTableOffset;        // 48h
        public uint ObjectIterPagesOffset;        // 4Ch
        public uint ResourceTableOffset;          // 50h
        public uint NumResourceTableEntries;      // 54h
        public uint ResidentNameTableOffset;      // 58h
        public uint EntryTableOffset;             // 5Ch
        public uint ModuleDirectivesOffset;       // 60h
        public uint NumModuleDirectives;          // 64h
        public uint FixupPageTableOffset;         // 68h
        public uint FixupRecordTableOffset;       // 6Ch
        public uint ImportModuleTableOffset;      // 70h
        public uint NumImportModuleEntries;       // 74h
        public uint ImportProcTableOffset;        // 78h
        public uint PerPageChecksumOffset;        // 7Ch
        public uint DataPagesOffset;              // 80h
        public uint NumPreloadPages;              // 84h
        public uint NonResNameTableOffset;        // 88h
        public uint NonResNameTableLength;        // 8Ch
        public uint NonResNameTableChecksum;      // 90h
        public uint AutoDSObjectNum;              // 94h
        public uint DebugInfoOffset;              // 98h
        public uint DebugInfoLength;              // 9Ch
        public uint NumInstancePreload;           // A0h
        public uint NumInstanceDemand;            // A4h
        public uint HeapSize;                     // A8h
    }
}