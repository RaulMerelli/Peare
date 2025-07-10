using System.Runtime.InteropServices;

namespace Peare
{
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct IMAGE_LE_HEADER
    {
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 2)]
        public char[] SignatureWord;

        public byte ByteOrder;
        public byte WordOrder;
        public uint ExecutableFormatLevel;
        public ushort CPUType;
        public ushort TargetOperatingSystem;
        public uint ModuleVersion;
        public uint ModuleTypeFlags;
        public uint NumberOfMemoryPages;
        public uint InitialObjectCSNumber;
        public uint InitialEIP;
        public uint InitialSSObjectNumber;
        public uint InitialESP;
        public uint MemoryPageSize;
        public uint BytesOnLastPage;
        public uint FixupSectionSize;
        public uint FixupSectionChecksum;
        public uint LoaderSectionSize;
        public uint LoaderSectionChecksum;
        public uint ObjectTableOffset;
        public uint ObjectTableEntries;
        public uint ObjectPageMapOffset;
        public uint ObjectIterateDataMapOffset;
        public uint ResourceTableOffset;
        public uint ResourceTableEntries;
        public uint ResidentNamesTableOffset;
        public uint EntryTableOffset;
        public uint ModuleDirectivesTableOffset;
        public uint ModuleDirectivesTableEntries;
        public uint FixupPageTableOffset;
        public uint FixupRecordTableOffset;
        public uint ImportedModulesNameTableOffset;
        public uint ImportedModulesCount;
        public uint ImportedProcedureNameTableOffset;
        public uint PerPageChecksumTableOffset;
        public uint DataPagesOffsetFromTopOfFile;
        public uint PreloadPagesCount;
        public uint NonResidentNamesTableOffsetFromTopOfFile;
        public uint NonResidentNamesTableLength;
        public uint NonResidentNamesTableChecksum;
        public uint AutomaticDataObject;
        public uint DebugInformationOffset;
        public uint DebugInformationLength;
        public uint PreloadInstancePagesNumber;
        public uint DemandInstancePagesNumber;
        public uint HeapSize;
        public uint StackSize;

        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 8)]
        public byte[] Reserved;

        public uint WindowsVXDVersionInfoResourceOffset;
        public uint WindowsVXDVersionInfoResourceLength;
        public ushort WindowsVXDDeviceID;
        public ushort WindowsDDKVersion;
    }

}
