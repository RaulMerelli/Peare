using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Peare
{
    public static class LeResources
    {
        public static List<string[]> OpenLE(string filePath)
        {
            List<string[]> relations = new List<string[]>();
            byte[] fileBytes = File.ReadAllBytes(filePath);
            int mzOffset = 0;
            if (BitConverter.ToUInt16(fileBytes, mzOffset) != 0x5A4D) throw new Exception("Non MZ header");
            int leOffset = BitConverter.ToInt32(fileBytes, mzOffset + 0x3C);
            if (fileBytes[leOffset] != 'L' || fileBytes[leOffset + 1] != 'E')
                throw new Exception("Non-LE executable");

            // read LE header
            IMAGE_LE_HEADER header = ReadLeHeader(fileBytes, leOffset);

            // calculate absolute positions
            int resTabPos = leOffset + (int)header.ResourceTableOffset;
            int objTabPos = leOffset + (int)header.ObjectTableOffset;

            ReadLeResourceTable(fileBytes, resTabPos, leOffset);
            ReadLeObjectTable(fileBytes, objTabPos, header.ObjectTableEntries);
            return relations;
        }

        public static void ReadLeObjectTable(byte[] buf, int pos, uint count)
        {
            int p = pos;
            int entrySize = 24; // struct size of LE_OBJECT_TABLE_ENTRY

            for (int i = 0; i < count; i++)
            {
                if (p + entrySize > buf.Length) break;

                LE_OBJECT_TABLE_ENTRY entry = new LE_OBJECT_TABLE_ENTRY
                {
                    VirtualSize = BitConverter.ToUInt32(buf, p),
                    BaseRelocAddress = BitConverter.ToUInt32(buf, p + 4),
                    ObjectFlags = (LE_OBJECT_FLAGS)BitConverter.ToUInt16(buf, p + 8),
                    PageTableIndex = BitConverter.ToUInt32(buf, p + 12),
                    PageTableEntries = BitConverter.ToUInt32(buf, p + 16),
                    Reserved = new char[4]
                };

                // Reserved 4 bytes (char array) 
                for (int r = 0; r < 4; r++)
                    entry.Reserved[r] = (char)buf[p + 20 + r];

                p += entrySize;

                Console.WriteLine($"Object {i}: Size={entry.VirtualSize}, Flags={entry.ObjectFlags}");
            }
        }

        public static void ReadLeResourceTable(byte[] buf, int pos, int baseOffset)
        {
            if (pos + 2 > buf.Length)
                return;

            ushort alignShift = BitConverter.ToUInt16(buf, pos);
            int align = 1 << alignShift;
            int p = pos + 2;

            while (true)
            {
                if (p + 2 > buf.Length) break;

                ushort typeID = BitConverter.ToUInt16(buf, p);
                if (typeID == 0) break; // resources end

                bool isNamed = (typeID & 0x8000) == 0;
                ushort type = (ushort)(typeID & 0x7FFF);

                string typeName;
                if (isNamed)
                {
                    int nameOffset = baseOffset + type;
                    if (nameOffset < buf.Length)
                    {
                        byte nameLen = buf[nameOffset];
                        if (nameOffset + 1 + nameLen <= buf.Length)
                            typeName = Encoding.ASCII.GetString(buf, nameOffset + 1, nameLen);
                        else
                            typeName = "#[InvalidTypeName]";
                    }
                    else
                    {
                        typeName = "#[InvalidTypeOffset]";
                    }
                }
                else
                {
                    // LE Resource Types
                    var resourceTypes = new Dictionary<ushort, string> {
                        {1, "CURSOR"}, {2, "BITMAP"}, {3, "ICON"}, {4, "MENU"},
                        {5, "DIALOG"}, {6, "STRING"}, {7, "FONTDIR"}, {8, "FONT"},
                        {9, "ACCELERATOR"}, {10, "RCDATA"}, {11, "MESSAGETABLE"}
                    };
                    if (resourceTypes.ContainsKey(type))
                        typeName = resourceTypes[type];
                    else
                        typeName = $"#{type}";
                }

                if (p + 8 > buf.Length) break;
                ushort resourceCount = BitConverter.ToUInt16(buf, p + 2);
                p += 8;

                List<string> resourceNames = new List<string>();
                for (int i = 0; i < resourceCount; i++)
                {
                    if (p + 12 > buf.Length) break;

                    ushort idField = BitConverter.ToUInt16(buf, p + 6);
                    string resourceName;

                    if ((idField & 0x8000) == 0)
                    {
                        int nameOffset = baseOffset + idField;
                        if (nameOffset < buf.Length)
                        {
                            byte nameLen = buf[nameOffset];
                            if (nameOffset + 1 + nameLen <= buf.Length)
                                resourceName = Encoding.ASCII.GetString(buf, nameOffset + 1, nameLen);
                            else
                                resourceName = "#[Invalid]";
                        }
                        else
                        {
                            resourceName = "#[Invalid]";
                        }
                    }
                    else
                    {
                        resourceName = "#" + (idField & 0x7FFF);
                    }
                    resourceNames.Add(resourceName);
                    p += 12;
                }

                Console.WriteLine($"{typeName} ({resourceCount} risorse)");
            }
        }

        public static IMAGE_LE_HEADER ReadLeHeader(byte[] buf, int offset)
        {
            IMAGE_LE_HEADER header = new IMAGE_LE_HEADER();

            using (MemoryStream ms = new MemoryStream(buf, offset, Marshal.SizeOf<IMAGE_LE_HEADER>()))
            using (BinaryReader br = new BinaryReader(ms))
            {
                header.SignatureWord = br.ReadChars(2);
                header.ByteOrder = br.ReadByte();
                header.WordOrder = br.ReadByte();
                header.ExecutableFormatLevel = br.ReadUInt32();
                header.CPUType = br.ReadUInt16();
                header.TargetOperatingSystem = br.ReadUInt16();
                header.ModuleVersion = br.ReadUInt32();
                header.ModuleTypeFlags = br.ReadUInt32();
                header.NumberOfMemoryPages = br.ReadUInt32();
                header.InitialObjectCSNumber = br.ReadUInt32();
                header.InitialEIP = br.ReadUInt32();
                header.InitialSSObjectNumber = br.ReadUInt32();
                header.InitialESP = br.ReadUInt32();
                header.MemoryPageSize = br.ReadUInt32();
                header.BytesOnLastPage = br.ReadUInt32();
                header.FixupSectionSize = br.ReadUInt32();
                header.FixupSectionChecksum = br.ReadUInt32();
                header.LoaderSectionSize = br.ReadUInt32();
                header.LoaderSectionChecksum = br.ReadUInt32();
                header.ObjectTableOffset = br.ReadUInt32();
                header.ObjectTableEntries = br.ReadUInt32();
                header.ObjectPageMapOffset = br.ReadUInt32();
                header.ObjectIterateDataMapOffset = br.ReadUInt32();
                header.ResourceTableOffset = br.ReadUInt32();
                header.ResourceTableEntries = br.ReadUInt32();
                header.ResidentNamesTableOffset = br.ReadUInt32();
                header.EntryTableOffset = br.ReadUInt32();
                header.ModuleDirectivesTableOffset = br.ReadUInt32();
                header.ModuleDirectivesTableEntries = br.ReadUInt32();
                header.FixupPageTableOffset = br.ReadUInt32();
                header.FixupRecordTableOffset = br.ReadUInt32();
                header.ImportedModulesNameTableOffset = br.ReadUInt32();
                header.ImportedModulesCount = br.ReadUInt32();
                header.ImportedProcedureNameTableOffset = br.ReadUInt32();
                header.PerPageChecksumTableOffset = br.ReadUInt32();
                header.DataPagesOffsetFromTopOfFile = br.ReadUInt32();
                header.PreloadPagesCount = br.ReadUInt32();
                header.NonResidentNamesTableOffsetFromTopOfFile = br.ReadUInt32();
                header.NonResidentNamesTableLength = br.ReadUInt32();
                header.NonResidentNamesTableChecksum = br.ReadUInt32();
                header.AutomaticDataObject = br.ReadUInt32();
                header.DebugInformationOffset = br.ReadUInt32();
                header.DebugInformationLength = br.ReadUInt32();
                header.PreloadInstancePagesNumber = br.ReadUInt32();
                header.DemandInstancePagesNumber = br.ReadUInt32();
                header.HeapSize = br.ReadUInt32();
                header.StackSize = br.ReadUInt32();
                header.Reserved = br.ReadBytes(8);
                header.WindowsVXDVersionInfoResourceOffset = br.ReadUInt32();
                header.WindowsVXDVersionInfoResourceLength = br.ReadUInt32();
                header.WindowsVXDDeviceID = br.ReadUInt16();
                header.WindowsDDKVersion = br.ReadUInt16();
            }

            return header;
        }

    }
}
