using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Peare
{
    public static class NeResources
    {
        // Windows resource types
        // Based on Win1 and Win2 prgref and WINMOD.C for Win3 by Matt Pietrek, 1992
        // Win1: https://www.os2museum.com/files/docs/win10sdk/windows-1.03-sdk-prgref-1986.pdf
        // Win2: https://www.os2museum.com/files/docs/win20sdk/windows-2.0-sdk-prgref-1987.pdf
        public static Dictionary<int, string> WindowsNeResourceTypes = new Dictionary<int, string>
        {
            { 0x01, "RT_CURSOR" },
            { 0x02, "RT_BITMAP" },
            { 0x03, "RT_ICON" },
            { 0x04, "RT_MENU" },
            { 0x05, "RT_DIALOG" },
            { 0x06, "RT_STRING" },
            { 0x07, "RT_FONTDIR" }, // FONTDIR do not exist in Win1 prgref, but it works?! Was it undocumented?
            { 0x08, "RT_FONT" },
            { 0x09, "RT_ACCELERATOR" },
            { 0x0A, "RT_RCDATA" },
            { 0x0B, "RT_MESSAGETABLE" }, // In WinMod by Matt Pietrek, 1992, File: WINMOD.C this is ErrorTable
            { 0x0C, "RT_GROUP_CURSOR" },
            { 0x0D, "RT_UNKNOWN(13)" }, 
            { 0x0E, "RT_GROUP_ICON" },
            { 0x0F, "RT_NAMETABLE" },
            { 0x10, "RT_VERSION" },
            { 257, "RT_DRV_RAW"} // Not found in any docs, this is an addition by me, as 257 is always found only in drv files
        };

        // OS/2 resource types
        public static Dictionary<int, string> OS2NeResourceTypes = new Dictionary<int, string>
        {
            { 1, "RT_POINTER" },
            { 2, "RT_BITMAP" },
            { 3, "RT_MENU" },
            { 4, "RT_DIALOG" },
            { 5, "RT_STRING" },
            { 6, "RT_FONTDIR" },
            { 7, "RT_FONT" },
            { 8, "RT_ACCELTABLE" },
            { 9, "RT_RCDATA" },
            { 10, "RT_MESSAGE" },
            { 11, "RT_DLGINCLUDE" },
            { 12, "RT_VKEYTBL" },
            { 13, "RT_KEYTBL" },
            { 14, "RT_CHARTBL" },
            { 15, "RT_DISPLAYINFO" },
            { 16, "RT_FKASHORT" },
            { 17, "RT_FKALONG" },
            { 18, "RT_HELPTABLE" },
            { 19, "RT_HELPSUBTABLE" },
            { 20, "RT_FDDIR" },
            { 21, "RT_FD" }
        };


        public static List<string[]> OpenNE(string filePath)
        {
            List<string[]> relations = new List<string[]>();
            byte[] fileBytes = File.ReadAllBytes(filePath);

            // verify MZ Header (DOS Stub) ---
            if (fileBytes.Length < 0x1A)
                throw new Exception("File too short for a valid MZ header.");

            ushort e_lfarlc = BitConverter.ToUInt16(fileBytes, 0x18);
            if (e_lfarlc != 0x0040)
                throw new Exception("File is not a valid NE executable (e_lfarlc != 0x0040).");

            // get NE Header Offset
            if (fileBytes.Length < 0x40)
                throw new Exception("File too short for a valid NE header.");

            int neHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
            if (neHeaderOffset < 0 || neHeaderOffset + 64 > fileBytes.Length)
                throw new Exception($"NE header offset out of range (0x{neHeaderOffset:X}).");

            // verify NE signature
            if (neHeaderOffset + 1 >= fileBytes.Length || fileBytes[neHeaderOffset] != 'N' || fileBytes[neHeaderOffset + 1] != 'E')
                throw new Exception("File is not an NE executable ('NE' signature not found at correct offset).");

            // get target OS
            if (neHeaderOffset + 0x36 + 1 > fileBytes.Length)
            {
                throw new Exception("File too short to determine target OS from NE header.");
            }
            byte osType = fileBytes[neHeaderOffset + 0x36];

            bool isOS2 = (osType == 0x01); // OS/2
            bool isWindows = !(osType == 0x01); // Can 0x00 (unknown) be also OS/2?

            Console.WriteLine($"[DEBUG] Detected OS Type: 0x{osType:X} ({(isOS2 ? "OS/2" : (isWindows ? "Windows" : "Unknown"))})");

            if (isWindows)
            {
                Console.WriteLine("[DEBUG] Parsing as Windows NE.");
                // get Windows resource table offset
                if (neHeaderOffset + 0x24 + 2 > fileBytes.Length)
                    throw new Exception("Resource table offset out of range or file too short for complete NE header.");
                ushort resourceTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
                int resourceTablePos = neHeaderOffset + resourceTableOffset;

                if (resourceTablePos + 2 > fileBytes.Length)
                    throw new Exception("Resource table offset points out of file bounds (or too close to end).");

                // ushort alignShift = BitConverter.ToUInt16(fileBytes, resourceTablePos);
                int currentParsePos = resourceTablePos + 2; // starts after AlignmentShiftCount

                int safeParsingEnd = fileBytes.Length;
                try
                {
                    if (neHeaderOffset + 0x26 + 2 <= fileBytes.Length)
                    {
                        ushort residentNamesTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x26);
                        if (residentNamesTableOffset > 0)
                        {
                            int potentialNextTablePos = neHeaderOffset + residentNamesTableOffset;
                            if (potentialNextTablePos > currentParsePos && potentialNextTablePos < fileBytes.Length)
                            {
                                safeParsingEnd = potentialNextTablePos;
                            }
                        }
                    }
                }
                catch 
                { 
                }

                // resource table parsing loop for Windows
                while (true)
                {
                    if (currentParsePos + 2 > fileBytes.Length || currentParsePos + 2 > safeParsingEnd)
                    {
                        Console.WriteLine($"[DEBUG] Terminating loop: parsing limit reached at 0x{currentParsePos:X}.");
                        break;
                    }

                    ushort typeID = BitConverter.ToUInt16(fileBytes, currentParsePos);
                    if (typeID == 0)
                    {
                        Console.WriteLine("[DEBUG] TypeID 0x0000 found. End of resource table.");
                        break;
                    }

                    bool isNamedType = (typeID & 0x8000) == 0; // bit 0x8000 (most significant) indicates whether it is a numeric ID (0x8000) or a string offset (0)
                    ushort typeValue = (ushort)(typeID & 0x7FFF); // numeric value or offset for the string name

                    string typeName;
                    if (isNamedType) // if bit 0x8000 is not set, it is an offset to a string
                    {
                        int nameOffset = resourceTablePos + typeValue;

                        if (nameOffset < resourceTablePos || nameOffset >= safeParsingEnd || nameOffset >= fileBytes.Length)
                        {
                            typeName = "#[InvalidTypeOffset:0x" + typeValue.ToString("X") + "]";
                        }
                        else
                        {
                            if (nameOffset + 1 > fileBytes.Length || nameOffset + 1 > safeParsingEnd)
                            {
                                typeName = "#[TypeLenOutOfBound:0x" + nameOffset.ToString("X") + "]";
                            }
                            else
                            {
                                byte nameLen = fileBytes[nameOffset];
                                if (nameLen == 0)
                                {
                                    typeName = string.Empty;
                                }
                                else if (nameLen > 127 || nameOffset + 1 + nameLen > fileBytes.Length || nameOffset + 1 + nameLen > safeParsingEnd)
                                {
                                    typeName = "#[TypeCorruptedName:0x" + nameLen.ToString("X") + " @ 0x" + nameOffset.ToString("X") + "]";
                                }
                                else
                                {
                                    try
                                    {
                                        typeName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                                    }
                                    catch
                                    {
                                        typeName = "#[TypeDecodingError:0x" + nameOffset.ToString("X") + "]";
                                    }
                                }
                            }
                        }
                    }
                    else // It is a numeric ID for the type
                    {
                        if (WindowsNeResourceTypes.TryGetValue(typeValue, out string name))
                            typeName = name;
                        else
                            typeName = $"#{typeValue}";
                    }

                    // read ResourceCount and advance currentParsePos
                    if (currentParsePos + 8 > fileBytes.Length || currentParsePos + 8 > safeParsingEnd)
                    {
                        Console.WriteLine($"[DEBUG] Terminating loop: not enough bytes for complete ResourceTypeHeader at 0x{currentParsePos:X}.");
                        break;
                    }
                    ushort resourceCount = BitConverter.ToUInt16(fileBytes, currentParsePos + 2);
                    currentParsePos += 8; // Advance the pointer after the resource type header

                    List<string> resourceNames = new List<string>();

                    // Resource Parsing Loop for this Type
                    for (int i = 0; i < resourceCount; i++)
                    {
                        if (currentParsePos + 12 > fileBytes.Length || currentParsePos + 12 > safeParsingEnd)
                        {
                            Console.WriteLine($"[DEBUG] Truncated resources for type '{typeName}' at 0x{currentParsePos:X}. Read {i} of {resourceCount}.");
                            resourceNames.Add($"#[TruncatedResources-{i + 1}/{resourceCount}]");
                            break;
                        }

                        ushort idField = BitConverter.ToUInt16(fileBytes, currentParsePos + 6); // ResourceID is at offset 6 in NE_Resource

                        string resourceName;
                        if ((idField & 0x8000) == 0) // If bit 0x8000 is not set, it is an offset to a string
                        {
                            int nameOffset = resourceTablePos + idField;

                            if (nameOffset < resourceTablePos || nameOffset >= safeParsingEnd || nameOffset >= fileBytes.Length)
                            {
                                resourceName = "#[InvalidResourceOffset:0x" + idField.ToString("X") + "]";
                            }
                            else
                            {
                                if (nameOffset + 1 > fileBytes.Length || nameOffset + 1 > safeParsingEnd)
                                {
                                    resourceName = "#[ResLenOutOfBound:0x" + nameOffset.ToString("X") + "]";
                                }
                                else
                                {
                                    byte nameLen = fileBytes[nameOffset];
                                    if (nameLen == 0)
                                    {
                                        resourceName = string.Empty;
                                    }
                                    else if (nameLen > 127 || nameOffset + 1 + nameLen > fileBytes.Length || nameOffset + 1 + nameLen > safeParsingEnd)
                                    {
                                        resourceName = "#[ResCorruptedName:0x" + nameLen.ToString("X") + " @ 0x" + nameOffset.ToString("X") + "]";
                                    }
                                    else
                                    {
                                        try
                                        {
                                            resourceName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                                        }
                                        catch
                                        {
                                            resourceName = "#[ResDecodingError:0x" + nameOffset.ToString("X") + "]";
                                        }
                                    }
                                }
                            }
                        }
                        else // It is a numeric ID for the resource
                        {
                            resourceName = "#" + (idField & 0x7FFF);
                        }

                        resourceNames.Add(resourceName);
                        currentParsePos += 12; // Advance the pointer after the resource header
                    }

                    relations.Add(new string[] { "Root", typeName });
                    foreach (var name in resourceNames)
                    {
                        relations.Add(new string[] { typeName, name });
                    }
                }
            }
            else if (isOS2)
            {
                Console.WriteLine("[DEBUG] Parsing as OS/2 NE.");

                // Get Resource Table Offset for OS/2
                // For a valid OS/2 NE, the offset ne_rsrctab (0x24) points to a table of (etype, ename) pairs.

                // Instead of 0x24 for resourceTableOffset, we’ll use the ne_rsrctab field located at neHeaderOffset + 0x24.
                // The documentation states "ne_rsrctab is the offset of the resource table".
                // This offset should point directly to the beginning of the etype/ename list.
                if (neHeaderOffset + 0x24 + 2 > fileBytes.Length)
                    throw new Exception("Resource table offset out of range or file too short for complete NE header (OS/2).");

                ushort os2ResourceTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
                int resourceTablePos = neHeaderOffset + os2ResourceTableOffset;

                // After obtaining resourceTablePos
                if (neHeaderOffset + 0x26 + 2 > fileBytes.Length)
                    throw new Exception("Resource table size field out of range (OS/2).");

                ushort rsrcsize = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x26);
                int safeParsingEnd = resourceTablePos + rsrcsize;

                Console.WriteLine($"[DEBUG] OS/2 Resource table spans from 0x{resourceTablePos:X} to 0x{safeParsingEnd:X} (size: 0x{rsrcsize:X} bytes).");

                if (resourceTablePos < neHeaderOffset || resourceTablePos >= fileBytes.Length)
                {
                    Console.WriteLine($"[DEBUG] OS/2 Resource table offset 0x{os2ResourceTableOffset:X} from NE header is invalid.");
                    // This can happen if there are no resources or the offset is malformed.
                    // It's not necessarily a fatal error if the goal is just to read the relationships.
                    return relations;
                }

                // For OS/2, the resource table is a sequence of (etype, ename) pairs, each 4 bytes.
                // The loop ends when either etype or ename is 0.

                ushort numResources = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x34);
                int currentParsePos = resourceTablePos;

                for (int i = 0; i < numResources; i++)
                {
                    if (currentParsePos + 4 > fileBytes.Length || currentParsePos + 4 > safeParsingEnd)
                    {
                        Console.WriteLine("[DEBUG] OS/2 resource table truncated or corrupted.");
                        break;
                    }

                    ushort etype = BitConverter.ToUInt16(fileBytes, currentParsePos);
                    ushort ename = BitConverter.ToUInt16(fileBytes, currentParsePos + 2);

                    if (etype == 0)
                    {
                        Console.WriteLine($"[DEBUG] OS/2 Resource table terminator (etype=0x0000) found at offset 0x{currentParsePos:X}.");
                        break;
                    }

                    string typeName;
                    if (OS2NeResourceTypes.TryGetValue(etype, out string name))
                    {
                        typeName = name;
                    }
                    else
                    {
                        typeName = $"#{etype}"; // User-defined or unknown types
                    }

                    // For OS/2, 'ename' is a numeric resource ID.
                    string resourceName = $"#{ename}";

                    relations.Add(new string[] { typeName, resourceName }); // Each entry is a direct relationship (Type, Resource ID)

                    currentParsePos += 4; // Move to the next entry (etype + ename)
                }
            }
            else
            {
                throw new Exception($"Unsupported NE target OS type: 0x{osType:X}. Only Windows (0x02, 0x03, 0x05, 0x08) and OS/2 (0x01) are supported for resource parsing.");
            }

            return relations;
        }

        public static byte[] OpenResourceNE(string currentFilePath,string typeName, string targetResourceName, out string message, out bool found)
        {
            List<byte> result = new List<byte>();
            message = "";
            found = false;

            byte[] fileBytes = File.ReadAllBytes(currentFilePath);
            int neHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
            byte neExeType = fileBytes[neHeaderOffset + 0x36]; // 0x36 = ne_exetyp

            if (neExeType == 1) // OS/2 NE
            {
                ushort resourceOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
                ushort segmentTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x22);
                ushort segmentCount = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x1C);
                ushort resourceCount = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x34);
                ushort alignShift = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x32);
                int align = 1 << alignShift;

                int resourceTablePos = neHeaderOffset + resourceOffset;
                int segmentTablePos = neHeaderOffset + segmentTableOffset;

                for (int i = 0; i < resourceCount; i++)
                {
                    int entryOffset = resourceTablePos + i * 4;
                    if (entryOffset + 4 > fileBytes.Length)
                    {
                        message = "Resource table out of bounds.";
                        return result.ToArray();
                    }

                    ushort etype = BitConverter.ToUInt16(fileBytes, entryOffset);
                    ushort ename = BitConverter.ToUInt16(fileBytes, entryOffset + 2);

                    string currentTypeName = OS2NeResourceTypes.TryGetValue(etype, out string tname) ? tname : $"#{etype}";
                    string currentResourceName = ename.ToString();

                    if (currentTypeName == typeName && currentResourceName == targetResourceName)
                    {
                        // Resources are always in the last <ne_cres> segments
                        int segmentIndex = segmentCount - resourceCount + i;
                        int segEntryOffset = segmentTablePos + segmentIndex * 8;

                        if (segEntryOffset + 8 > fileBytes.Length)
                        {
                            message = "Segment table entry out of bounds.";
                            return result.ToArray();
                        }

                        ushort ssector = BitConverter.ToUInt16(fileBytes, segEntryOffset);
                        ushort cb = BitConverter.ToUInt16(fileBytes, segEntryOffset + 2);
                        // ushort sflags = BitConverter.ToUInt16(fileBytes, segEntryOffset + 4); 
                        // ushort smin   = BitConverter.ToUInt16(fileBytes, segEntryOffset + 6);

                        int dataOffset = ssector << alignShift;

                        if (dataOffset + cb > fileBytes.Length)
                        {
                            message = "Resource data out of bounds.";
                            return result.ToArray();
                        }

                        byte[] resData = new byte[cb];
                        Array.Copy(fileBytes, dataOffset, resData, 0, cb);

                        message = $"OS/2 Resource {typeName} {targetResourceName} found.\nLength: {cb} byte.";
                        found = true;
                        return resData;
                    }
                }

                message = $"OS/2 Resource {typeName} {targetResourceName} not found.";
                return result.ToArray();
            }
            else // Windows NE
            {
                int resourceTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
                int resourceTablePos = neHeaderOffset + resourceTableOffset;
                ushort alignShift = BitConverter.ToUInt16(fileBytes, resourceTablePos);
                int align = 1 << alignShift;
                int pos = resourceTablePos + 2;

                while (true)
                {
                    if (pos + 2 > fileBytes.Length) break;

                    ushort typeID = BitConverter.ToUInt16(fileBytes, pos);
                    if (typeID == 0) break;

                    bool isTypeNamed = (typeID & 0x8000) == 0;
                    ushort typeVal = (ushort)(typeID & 0x7FFF);

                    string currentTypeName;
                    if (isTypeNamed)
                    {
                        int nameOffset = resourceTablePos + typeVal;
                        if (nameOffset < fileBytes.Length)
                        {
                            byte len = fileBytes[nameOffset];
                            currentTypeName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, len);
                        }
                        else currentTypeName = "#[InvalidType]";
                    }
                    else
                    {
                        currentTypeName = WindowsNeResourceTypes.ContainsKey(typeVal) ? WindowsNeResourceTypes[typeVal] : $"#{typeVal}";
                    }

                    ushort resourceCount = BitConverter.ToUInt16(fileBytes, pos + 2);
                    pos += 8;

                    if (currentTypeName == typeName)
                    {
                        for (int i = 0; i < resourceCount; i++)
                        {
                            if (pos + 12 > fileBytes.Length) break;

                            ushort offsetUnits = BitConverter.ToUInt16(fileBytes, pos);
                            ushort lengthUnits = BitConverter.ToUInt16(fileBytes, pos + 2);
                            ushort idField = BitConverter.ToUInt16(fileBytes, pos + 6);

                            bool isName = (idField & 0x8000) == 0;

                            bool match = false;

                            if (!isName)
                            {
                                int idVal = idField & 0x7FFF;
                                if (idVal.ToString() == targetResourceName)
                                    match = true;
                            }
                            else
                            {
                                int nameOffset = resourceTablePos + idField;
                                if (nameOffset < fileBytes.Length)
                                {
                                    byte nameLen = fileBytes[nameOffset];
                                    if (nameOffset + 1 + nameLen <= fileBytes.Length)
                                    {
                                        string resName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                                        if (resName == targetResourceName)
                                            match = true;
                                    }
                                }
                            }

                            if (match)
                            {
                                int dataOffset = offsetUnits * align;
                                int dataLength = lengthUnits * align;

                                byte[] resData = new byte[dataLength];
                                Array.Copy(fileBytes, dataOffset, resData, 0, dataLength);

                                message = $"Resource NE {typeName} {targetResourceName} selected.\nLength: {dataLength} byte.";
                                result = resData.ToList();
                                found = true;
                                return result.ToArray();
                            }

                            pos += 12;
                        }

                        if (found) break;
                    }
                    else
                    {
                        pos += resourceCount * 12;
                    }
                }

                if (!found)
                {
                    message = $"Resource {typeName} {targetResourceName} not found.";
                }
                return result.ToArray();
            }
        }

    }
}