using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace PeareModule
{
    public static class LeResources
    {
        // Copied from LxResourceTypes. It may differ.
        public static Dictionary<int, string> LeResourceTypes = new Dictionary<int, string>
        {
            { 0x01, "RT_POINTER" },            // mouse pointer shape
            { 0x02, "RT_BITMAP" },             // bitmap
            { 0x03, "RT_MENU" },               // menu template
            { 0x04, "RT_DIALOG" },             // dialog template
            { 0x05, "RT_STRING" },             // string tables
            { 0x06, "RT_FONTDIR" },            // font directory
            { 0x07, "RT_FONT" },               // font
            { 0x08, "RT_ACCELTABLE" },         // accelerator tables
            { 0x09, "RT_RCDATA" },             // binary data
            { 0x0A, "RT_MESSAGE" },            // error message tables
            { 0x0B, "RT_DLGINCLUDE" },         // dialog include file name
            { 0x0C, "RT_VKEYTBL" },            // key to virtual-key tables
            { 0x0D, "RT_KEYTBL" },             // key to UGL tables
            { 0x0E, "RT_CHARTBL" },            // glyph to character tables
            { 0x0F, "RT_DISPLAYINFO" },        // screen display information
            { 0x10, "RT_FKASHORT" },           // function key area short form
            { 0x11, "RT_FKALONG" },            // function key area long form
            { 0x12, "RT_HELPTABLE" },          // Help table for Cary Help manager
            { 0x13, "RT_HELPSUBTABLE" },       // Help subtable for Cary Help manager
            { 0x14, "RT_FDDIR" },              // DBCS unique/font driver directory
            { 0x15, "RT_FD" }                  // DBCS unique/font driver
        };

        public static List<string[]> OpenLE(string filePath)
        {
            // This is the same as LX
            List<string[]> relations = new List<string[]>();
            byte[] fileBytes = File.ReadAllBytes(filePath);

            int leHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
            string signature = Encoding.ASCII.GetString(fileBytes, leHeaderOffset, 2);

            if (signature != "LE")
            {
                Console.WriteLine("The file is not a Linear Executable (LE).");
                return relations;
            }

            int resourceTableOffsetInHeader = BitConverter.ToInt32(fileBytes, leHeaderOffset + 0x50);
            uint resourceEntryCount = BitConverter.ToUInt32(fileBytes, leHeaderOffset + 0x54);

            if (resourceTableOffsetInHeader == 0 || resourceEntryCount == 0)
            {
                Console.WriteLine("No resource table found in the file.");
                return relations;
            }

            int resourceTableOffset = leHeaderOffset + resourceTableOffsetInHeader;
            int objectTableOffsetInHeader = BitConverter.ToInt32(fileBytes, leHeaderOffset + 0x40);
            int objectTableOffset = leHeaderOffset + objectTableOffsetInHeader;

            int resourceNameTableOffsetInHeader = BitConverter.ToInt32(fileBytes, leHeaderOffset + 0x58);
            int resourceNameTableOffset = resourceNameTableOffsetInHeader != 0
                ? resourceNameTableOffsetInHeader
                : 0;

            for (int i = 0; i < resourceEntryCount; i++)
            {
                int entryOffset = resourceTableOffset + i * 14;
                if (entryOffset + 14 > fileBytes.Length)
                    continue;

                ushort typeID = BitConverter.ToUInt16(fileBytes, entryOffset + 0x00);
                ushort nameID = BitConverter.ToUInt16(fileBytes, entryOffset + 0x02);
                uint resourceSize = BitConverter.ToUInt32(fileBytes, entryOffset + 0x04);
                ushort objectNum = BitConverter.ToUInt16(fileBytes, entryOffset + 0x08);
                uint offsetInObject = BitConverter.ToUInt32(fileBytes, entryOffset + 0x0A);

                string typeName = LeResourceTypes.TryGetValue(typeID, out string tName)
                    ? tName
                    : $"#{typeID}";

                int objectEntryOffset = objectTableOffset + (objectNum - 1) * 0x18;
                if (objectEntryOffset + 8 > fileBytes.Length)
                    continue;

                if (!relations.Any(x => x[0] == "Root" && x[1] == typeName))
                {
                    relations.Add(new string[] { "Root", typeName });
                }

                relations.Add(new string[] { typeName, $"#{nameID}" });
            }
            return relations;
        }

        public static byte[] OpenResourceLE(ModuleResources.ModuleProperties properties, string typeName, string targetResourceName, out string message, out bool found)
        {
            // This method is quite buggy but mostly works, so it is a start I guess.
            // This format is so little documented (especially LE_OBJECT_PAGE_TABLE_ENTRY, where online docs were describind fields as unknown or with wrong sizes...)
            // that I'm already happy by having it at least somehow open.
            message = "";
            found = false;
            List<byte> result = new List<byte>();
            byte[] headerBytes = new byte[172];
            byte[] fileBytes = File.ReadAllBytes(properties.filePath);
            int leHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);

            Console.WriteLine($"Attempting to open resource Type: {typeName}, Name: {targetResourceName}");
            Console.WriteLine($"File size: {fileBytes.Length} bytes.");

            Array.Copy(fileBytes, leHeaderOffset, headerBytes, 0, 172);
            IMAGE_LE_HEADER header = ModuleResources.Deserialize<IMAGE_LE_HEADER>(headerBytes);

            // Calculate absolute offsets of tables
            int resourceTableOffset = leHeaderOffset + (int)header.ResourceTableOffset;
            int objectTableOffset = leHeaderOffset + (int)header.ObjectTableOffset;
            int objectPageMapOffset = leHeaderOffset + (int)header.ObjectPageMapOffset;

            // Search for the resource in the resource table
            for (int i = 0; i < header.ResourceTableEntries; i++)
            {
                int entryOffset = resourceTableOffset + i * 14; // Each entry is 14 bytes
                if (entryOffset + 14 > fileBytes.Length || entryOffset < 0)
                {
                    continue;
                }

                ushort typeID = BitConverter.ToUInt16(fileBytes, entryOffset);
                ushort nameID = BitConverter.ToUInt16(fileBytes, entryOffset + 2);
                uint resourceSize = BitConverter.ToUInt32(fileBytes, entryOffset + 4);
                ushort objectNum = BitConverter.ToUInt16(fileBytes, entryOffset + 8);
                uint offsetInObject = BitConverter.ToUInt32(fileBytes, entryOffset + 10);
                string currentTypeName = LeResourceTypes.TryGetValue(typeID, out string tName) ? tName : $"#{typeID}";

                if (currentTypeName == typeName && nameID.ToString() == targetResourceName)
                {
                    Console.WriteLine($"--- Matching resource found in resource table! ---");
                    Console.WriteLine($"  Type: {currentTypeName}, Name ID: {nameID}, Resource Size: {resourceSize} bytes.");
                    // For example Object Number: 7, Offset In Object: 0
                    Console.WriteLine($"  Object Number: {objectNum}, Offset In Object: {offsetInObject}");

                    // Handle Object Table Entry
                    int objEntryOffset = objectTableOffset + ((objectNum - 1) * Marshal.SizeOf(typeof(LE_OBJECT_TABLE_ENTRY)));
                    byte[] objectEntryBytes = new byte[Marshal.SizeOf(typeof(LE_OBJECT_TABLE_ENTRY))];
                    Array.Copy(fileBytes, objEntryOffset, objectEntryBytes, 0, Marshal.SizeOf(typeof(LE_OBJECT_TABLE_ENTRY)));
                    LE_OBJECT_TABLE_ENTRY tableEntry = ModuleResources.Deserialize<LE_OBJECT_TABLE_ENTRY>(objectEntryBytes);

                    // For example Object Page Table Index (1-based): 55, Page Table Entries Count: 5
                    Console.WriteLine($"  Object Page Table Index (1-based): {tableEntry.PageTableIndex}, Page Table Entries Count: {tableEntry.PageTableEntries}");

                    if (tableEntry.PageTableEntries == 0)
                    {
                        message = "The object associated with the resource contains no entries.";
                        Console.WriteLine(message);
                        return result.ToArray();
                    }

                    int pageDescriptorEntryOffset = objectPageMapOffset + (int)((tableEntry.PageTableIndex - 1) * Marshal.SizeOf(typeof(LE_OBJECT_PAGE_TABLE_ENTRY)));
                    byte[] pageDescriptorBytes = new byte[Marshal.SizeOf(typeof(LE_OBJECT_PAGE_TABLE_ENTRY))];
                    Array.Copy(fileBytes, pageDescriptorEntryOffset, pageDescriptorBytes, 0, Marshal.SizeOf(typeof(LE_OBJECT_PAGE_TABLE_ENTRY)));
                    LE_OBJECT_PAGE_TABLE_ENTRY pageEntry = ModuleResources.Deserialize<LE_OBJECT_PAGE_TABLE_ENTRY>(pageDescriptorBytes);

                    int currentPageDataOffset = (int)header.DataPagesOffsetFromTopOfFile + ((pageEntry.PageTableIndex - 1) * (int)header.MemoryPageSize);
                    ushort pageFlags = pageEntry.Flags;
                    ushort pageDataLengthInFile = (ushort)header.MemoryPageSize;
                    // We take data also over the page end, as resources can overflow to next pages.
                    // This might be not the correct way to do it, but for Legal Physical Pages seems to be working fine...
                    byte[] page = fileBytes.Skip(currentPageDataOffset).Take(fileBytes.Length - currentPageDataOffset).ToArray();

                    Console.WriteLine($"--- Extracted Page Data ---");
                    Console.WriteLine($"  Object Page Table Index (1-based): {tableEntry.PageTableIndex}");
                    Console.WriteLine($"  Global Page Index (from descriptor): {pageEntry.PageTableIndex}");
                    Console.WriteLine($"  Calculated Actual Page Data Offset: 0x{currentPageDataOffset:X}");
                    Console.WriteLine($"  Extracted Page Size: {page.Length} bytes.");

                    found = true;
                    //ModuleResources.DumpRaw(page);

                    int currentBaseOffset;
                    if (pageFlags == 0x00) // Legal Physical Page
                    {
                        currentBaseOffset = currentPageDataOffset;
                        Console.WriteLine("    DEBUG: Detected Legal Data Page.");

                        if (offsetInObject + resourceSize > page.Length)
                        {
                            message = $"Resource data (offset {offsetInObject}, size {resourceSize}) exceeds page bounds ({page.Length}).";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        // Extract the resource bytes from the page
                        byte[] resourceData = page.Skip((int)offsetInObject).Take((int)resourceSize).ToArray();
                        result.AddRange(resourceData); // Add to the result list

                        message = $"LE resource '{typeName}' (ID: {nameID}) found and extracted successfully.\nSize: {resourceSize} bytes.\r\n";
                        Console.WriteLine($"--- Extracted Resource Data ---");
                        Console.WriteLine($"  Resource offset within page: {offsetInObject}");
                        Console.WriteLine($"  Resource size: {resourceSize} bytes.");
                        ModuleResources.DumpRaw(resourceData); // Dump the extracted resource
                        return result.ToArray();
                    }
                    else
                    {
                        message = "The page associated with the resource is not a legal physical page.";
                        Console.WriteLine(message);
                        return result.ToArray();
                    }
                }
            }

            message = $"Resource '{typeName}' (Name: {targetResourceName}) not found in LE file.";
            Console.WriteLine(message);
            return result.ToArray();
        }
    }
}
