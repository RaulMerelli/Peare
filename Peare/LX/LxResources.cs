using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Peare
{
    public static class LxResources
    {
        // Special thanks to EDM/2 Wiki for proving the updated doc:
        // https://www.edm2.com/index.php/IBM_OS/2_16/32-bit_Object_Module_Format_(OMF)_and_Linear_eXecutable_Module_Format_(LX)
        public static Dictionary<int, string> LxResourceTypes = new Dictionary<int, string>
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

        public static List<string[]> OpenLX(string filePath)
        {
            List<string[]> relations = new List<string[]>();
            byte[] fileBytes = File.ReadAllBytes(filePath);

            int lxHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
            string signature = Encoding.ASCII.GetString(fileBytes, lxHeaderOffset, 2);

            if (signature != "LX")
            {
                Console.WriteLine("Il file non è un Linear Executable (LX).");
                return relations;
            }

            int resourceTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x50);
            ushort resourceEntryCount = BitConverter.ToUInt16(fileBytes, lxHeaderOffset + 0x54);

            if (resourceTableOffsetInHeader == 0 || resourceEntryCount == 0)
            {
                Console.WriteLine("Nessuna tabella risorse trovata nel file.");
                return relations;
            }

            int resourceTableOffset = lxHeaderOffset + resourceTableOffsetInHeader;
            int objectTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x40);
            int objectTableOffset = lxHeaderOffset + objectTableOffsetInHeader;

            int resourceNameTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x58);
            int resourceNameTableOffset = resourceNameTableOffsetInHeader != 0
                ? resourceNameTableOffsetInHeader
                : 0;

            string ReadResourceName(ushort nameID)
            {
                const ushort OFFSET_FLAG = 0x8000;

                if ((nameID & OFFSET_FLAG) != 0 && resourceNameTableOffset != 0)
                {
                    int offset = nameID & 0x7FFF;
                    int nameEntryOffset = resourceNameTableOffset + offset;

                    if (nameEntryOffset >= fileBytes.Length)
                        return $"#ERR_OOB";

                    byte length = fileBytes[nameEntryOffset];
                    if (length == 0 || nameEntryOffset + 1 + length > fileBytes.Length)
                        return $"#ERR_LEN";

                    return Encoding.ASCII.GetString(fileBytes, nameEntryOffset + 1, length);
                }

                return $"#{nameID}";
            }

            for (int i = 0; i < resourceEntryCount; i++)
            {
                int entryOffset = resourceTableOffset + i * 14;
                if (entryOffset + 14 > fileBytes.Length)
                    continue;

                ushort typeID = BitConverter.ToUInt16(fileBytes, entryOffset);
                ushort nameID = BitConverter.ToUInt16(fileBytes, entryOffset + 2);
                // uint resourceSize = BitConverter.ToUInt32(fileBytes, entryOffset + 4);
                ushort objectNum = BitConverter.ToUInt16(fileBytes, entryOffset + 8);
                // uint offsetInObject = BitConverter.ToUInt32(fileBytes, entryOffset + 10);

                string typeName = LxResourceTypes.TryGetValue(typeID, out string tName)
                    ? tName
                    : $"Type_{typeID}";

                string resourceName = ReadResourceName(nameID);

                int objectEntryOffset = objectTableOffset + (objectNum - 1) * 0x18;
                if (objectEntryOffset + 8 > fileBytes.Length)
                    continue;

                if (!relations.Any(x => x[0] == "Root" && x[1] == typeName))
                {
                    relations.Add(new string[] { "Root", typeName });
                }

                relations.Add(new string[] { typeName, resourceName });
            }

            return relations;
        }



        public static byte[] OpenResourceLX(string typeName, string targetResourceName, out string message, out bool found)
        {
            // NOT WORKING

            message = "";
            found = false;

            List<byte> result = new List<byte>();
            byte[] fileBytes = File.ReadAllBytes(Program.currentFilePath);

            int lxHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);

            int resourceTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x50);
            ushort resourceEntryCount = BitConverter.ToUInt16(fileBytes, lxHeaderOffset + 0x54);

            if (resourceTableOffsetInHeader == 0 || resourceEntryCount == 0)
            {
                message = "Nessuna tabella risorse trovata nel file LX.";
                return result.ToArray();
            }

            int resourceTableOffset = lxHeaderOffset + resourceTableOffsetInHeader;
            int objectTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x40);
            int objectTableOffset = lxHeaderOffset + objectTableOffsetInHeader;

            for (int i = 0; i < resourceEntryCount; i++)
            {
                int entryOffset = resourceTableOffset + i * 14;
                if (entryOffset + 14 > fileBytes.Length)
                    break;

                ushort typeID = BitConverter.ToUInt16(fileBytes, entryOffset);
                ushort nameID = BitConverter.ToUInt16(fileBytes, entryOffset + 2);
                uint resSize = BitConverter.ToUInt32(fileBytes, entryOffset + 4);
                ushort objectNum = BitConverter.ToUInt16(fileBytes, entryOffset + 8);
                uint offsetInObject = BitConverter.ToUInt32(fileBytes, entryOffset + 10);

                string actualTypeName = LxResourceTypes.TryGetValue(typeID, out string tName) ? tName : $"#{typeID}";

                string actualResourceName = $"#{nameID}";

                if (actualTypeName == typeName && actualResourceName == targetResourceName)
                {
                    int objectEntryOffset = objectTableOffset + (objectNum - 1) * 0x18;
                    if (objectEntryOffset + 0x18 > fileBytes.Length)
                        continue;

                    uint objectDataOffset = BitConverter.ToUInt32(fileBytes, objectEntryOffset + 8);
                    long resFileOffset = objectDataOffset + offsetInObject;

                    if (resFileOffset + resSize > fileBytes.Length)
                    {
                        message = "Risorsa oltre la dimensione del file.";
                        return result.ToArray();
                    }

                    byte[] resData = new byte[resSize];
                    Array.Copy(fileBytes, resFileOffset, resData, 0, resSize);

                    message = $"Resource LX {typeName} {targetResourceName} selected.\nLength: {resSize} byte.\r\n";

                    result = resData.ToList();

                    found = true;
                    break;
                }
            }

            if (!found)
            {
                message = $"Risorsa {typeName} {targetResourceName} non trovata nel file LX.";
            }
            return result.ToArray();
        }

    }
}
