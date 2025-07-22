using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace PeareModule
{
    public static class LxResources
    {
        // Special thanks to EDM/2 Wiki for providing the updated doc:
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
                Console.WriteLine("The file is not a Linear Executable (LX).");
                return relations;
            }

            int resourceTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x50);
            uint resourceEntryCount = BitConverter.ToUInt32(fileBytes, lxHeaderOffset + 0x54);

            if (resourceTableOffsetInHeader == 0 || resourceEntryCount == 0)
            {
                Console.WriteLine("No resource table found in the file.");
                return relations;
            }

            int resourceTableOffset = lxHeaderOffset + resourceTableOffsetInHeader;
            int objectTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x40);
            int objectTableOffset = lxHeaderOffset + objectTableOffsetInHeader;

            int resourceNameTableOffsetInHeader = BitConverter.ToInt32(fileBytes, lxHeaderOffset + 0x58);
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

                string typeName = LxResourceTypes.TryGetValue(typeID, out string tName)
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

        // Special thanks to OsFree project for providing the method to extract ITERDATA2 pages.
        // https://github.com/ErisBlastar/osfree/blob/9472591e4e8f9c8e3b78e6b8d6f0634c63387fd3/tools/lxlite/os2exe.pas#L259
        // This method is a almost complete conversion of their Pascal function UnpackMethod2.
        public static unsafe byte[] UnpackMethod2(ref byte[] srcData, int srcDataSize, int dstDataSize, out bool success)
        {
            byte[] destData = new byte[dstDataSize];

            fixed (byte* pSrc = srcData, pDst = destData)
            {
                byte* src = pSrc;
                byte* dst = pDst;
                int sOf = 0, dOf = 0, bOf;
                byte B1, B2;

                bool srcAvail(int N) => sOf + N <= srcDataSize;
                bool dstAvail(int N) => dOf + N <= dstDataSize;

                while (dOf < dstDataSize)
                {
                    if (!srcAvail(1))
                    {
                        break;
                    }
                    B1 = src[sOf];

                    switch (B1 & 3)
                    {
                        case 0:
                            if (B1 == 0)
                            {
                                if (srcAvail(2))
                                {
                                    if (src[sOf + 1] == 0)
                                    {
                                        sOf += 2;
                                        break;
                                    }
                                    else if (srcAvail(3) && dstAvail(src[sOf + 1]))
                                    {
                                        for (int i = 0; i < src[sOf + 1]; i++)
                                            dst[dOf + i] = src[sOf + 2];
                                        sOf += 3;
                                        dOf += src[sOf - 2];
                                    }
                                    else
                                    {
                                        success = false;
                                        return null;
                                    }
                                }
                                else
                                {
                                    success = false;
                                    return null;
                                }
                            }
                            else
                            {
                                int count = B1 >> 2;
                                if (srcAvail(count + 1) && dstAvail(count))
                                {
                                    for (int i = 0; i < count; i++)
                                        dst[dOf + i] = src[sOf + 1 + i];
                                    dOf += count;
                                    sOf += count + 1;
                                }
                                else
                                {
                                    success = false;
                                    return null;
                                }
                            }
                            break;

                        case 1:
                            if (!srcAvail(2))
                            {
                                success = false;
                                return null;
                            }
                            bOf = (*(ushort*)(src + sOf)) >> 7;
                            B2 = (byte)(((B1 >> 4) & 7) + 3);
                            B1 = (byte)((B1 >> 2) & 3);
                            sOf += 2;
                            if (srcAvail(B1) && dstAvail(B1 + B2) && (dOf + B1 - bOf >= 0))
                            {
                                for (int i = 0; i < B1; i++)
                                    dst[dOf + i] = src[sOf + i];
                                dOf += B1;
                                sOf += B1;
                                for (int i = 0; i < B2; i++)
                                    dst[dOf + i] = dst[dOf - bOf + i];
                                dOf += B2;
                            }
                            else
                            {
                                success = false;
                                return null;
                            }
                            break;

                        case 2:
                            if (!srcAvail(2))
                            {
                                success = false;
                                return null;
                            }
                            bOf = (*(ushort*)(src + sOf)) >> 4;
                            B1 = (byte)(((B1 >> 2) & 3) + 3);
                            if (dstAvail(B1) && (dOf - bOf >= 0))
                            {
                                for (int i = 0; i < B1; i++)
                                    dst[dOf + i] = dst[dOf - bOf + i];
                                dOf += B1;
                                sOf += 2;
                            }
                            else
                            {
                                success = false;
                                return null;
                            }
                            break;

                        case 3:
                            if (!srcAvail(3))
                            {
                                success = false;
                                return null;
                            }
                            B2 = (byte)(((*(ushort*)(src + sOf)) >> 6) & 0x3F);
                            B1 = (byte)((src[sOf] >> 2) & 0x0F);
                            bOf = (*(ushort*)(src + sOf + 1)) >> 4;
                            sOf += 3;
                            if (srcAvail(B1) && dstAvail(B1 + B2) && (dOf + B1 - bOf >= 0))
                            {
                                for (int i = 0; i < B1; i++)
                                    dst[dOf + i] = src[sOf + i];
                                dOf += B1;
                                sOf += B1;
                                for (int i = 0; i < B2; i++)
                                    dst[dOf + i] = dst[dOf - bOf + i];
                                dOf += B2;
                            }
                            else
                            {
                                success = false;
                                return null;
                            }
                            break;
                    }
                }

                if (dOf < dstDataSize)
                {
                    for (int i = dOf; i < dstDataSize; i++)
                        dst[i] = 0;
                }

                success = true;
                return destData;
            }
        }

        // Generated from the docs by ChatGPT, must check if this works or not...
        public static byte[] DecompressRlePage(byte[] compressedData, int logicalPageSize)
        {
            if (compressedData == null || compressedData.Length == 0)
            {
                return new byte[0];
            }

            using (MemoryStream input = new MemoryStream(compressedData))
            using (MemoryStream output = new MemoryStream(logicalPageSize))
            {
                long bytesDecompressed = 0;

                // Iterate until the logical page size is reached in the output
                while (bytesDecompressed < logicalPageSize && input.Position < input.Length)
                {
                    // Read LX_nIter (uint16_t) - Number of iterations
                    // Read LX_nBytes (uint16_t) - Number of bytes to iterate/copy
                    // These are little-endian from the file.

                    // Ensure there are enough bytes left for LX_nIter and LX_nBytes
                    if (input.Length - input.Position < 4)
                    {
                        Console.WriteLine("Warning: Incomplete RLE header (LX_nIter or LX_nBytes).");
                        break;
                    }

                    byte[] headerBytes = new byte[4];
                    input.Read(headerBytes, 0, 4);

                    ushort nIter = BitConverter.ToUInt16(headerBytes, 0); // LX_nIter 
                    ushort nBytes = BitConverter.ToUInt16(headerBytes, 2); // LX_nBytes 

                    if (nIter == 0 || nBytes == 0)
                    {
                        // This could indicate end of stream, or padding, or an error.
                        // Depending on the specific RLE variant, 0 might have special meaning.
                        // For now, let's assume it means "stop" or "skip padding".
                        Console.WriteLine($"Debug: Encountered RLE header with nIter={nIter}, nBytes={nBytes}. Stopping.");
                        break;
                    }

                    // Ensure there are enough bytes left for LX_Iterdata
                    if (input.Length - input.Position < nBytes)
                    {
                        Console.WriteLine($"Warning: Incomplete RLE data block. Expected {nBytes} bytes, but only {input.Length - input.Position} left.");
                        break;
                    }

                    byte[] iteratedData = new byte[nBytes];
                    input.Read(iteratedData, 0, nBytes); // Read the LX_Iterdata block 

                    // Perform the iteration/copy
                    for (int i = 0; i < nIter; i++)
                    {
                        if (bytesDecompressed + nBytes > logicalPageSize)
                        {
                            // Don't write past the logical page boundary.
                            // Copy only the remaining bytes required to fill the page.
                            int remainingBytes = (int)(logicalPageSize - bytesDecompressed);
                            output.Write(iteratedData, 0, remainingBytes);
                            bytesDecompressed += remainingBytes;
                            break; // Page is full
                        }

                        output.Write(iteratedData, 0, nBytes);
                        bytesDecompressed += nBytes;
                    }
                }

                // Fill the rest of the page with zeros if not completely filled
                while (output.Length < logicalPageSize)
                {
                    output.WriteByte(0x00);
                }

                return output.ToArray();
            }
        }

        public static byte[] OpenResourceLX(ModuleResources.ModuleProperties properties, string typeName, string targetResourceName, out string message, out bool found)
        {
            message = "";
            found = false;
            List<byte> result = new List<byte>();
            byte[] headerBytes = new byte[172];
            byte[] fileBytes = File.ReadAllBytes(properties.filePath);
            int lxHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);

            Console.WriteLine($"Attempting to open resource Type: {typeName}, Name: {targetResourceName}");
            Console.WriteLine($"File size: {fileBytes.Length} bytes.");

            Array.Copy(fileBytes, lxHeaderOffset, headerBytes, 0, 172);
            IMAGE_LX_HEADER header = ModuleResources.Deserialize<IMAGE_LX_HEADER>(headerBytes);

            // Calculate absolute offsets of tables
            int resourceTableOffset = lxHeaderOffset + (int)header.ResourceTableOffset;
            int objectTableOffset = lxHeaderOffset + (int)header.ObjectTableOffset;
            int objectPageTableOffset = lxHeaderOffset + (int)header.ObjectPageTableOffset;

            // Search for the resource in the resource table
            for (int i = 0; i < header.NumResourceTableEntries; i++)
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

                string currentTypeName = LxResourceTypes.TryGetValue(typeID, out string tName) ? tName : $"#{typeID}";

                if (currentTypeName == typeName && nameID.ToString() == targetResourceName)
                {
                    Console.WriteLine($"--- Matching resource found in resource table! ---");
                    Console.WriteLine($"  Type: {currentTypeName}, Name ID: {nameID}, Resource Size: {resourceSize} bytes.");
                    Console.WriteLine($"  Object Number: {objectNum}, Offset In Object: {offsetInObject}");

                    int objEntryOffset = objectTableOffset + ((objectNum - 1) * 24); // Each entry 24 bytes

                    byte[] tableEntryBytes = new byte[24];
                    Array.Copy(fileBytes, objEntryOffset, tableEntryBytes, 0, 24);
                    LX_OBJECT_TABLE_ENTRY tableEntry = ModuleResources.Deserialize<LX_OBJECT_TABLE_ENTRY>(tableEntryBytes);

                    Console.WriteLine($"  Object Page Table Index (1-based from Object Table): {tableEntry.PageTableIndex}, Page Table Entries Count: {tableEntry.PageTableEntries}");

                    if (tableEntry.PageTableEntries == 0)
                    {
                        message = "The object associated with the resource contains no pages.";
                        Console.WriteLine(message);
                        return result.ToArray();
                    }

                    // Calculate which logical page contains the resource's starting offset
                    int startPageIndexInObject = (int)(offsetInObject / header.PageSize);
                    int startOffsetWithinPage = (int)(offsetInObject % header.PageSize);

                    Console.WriteLine($"  Resource starts at offset {offsetInObject} in object. Corresponds to logical page {startPageIndexInObject} with offset {startOffsetWithinPage}.");

                    // Initialize array to contain the entire resource
                    byte[] resData = new byte[resourceSize];
                    uint bytesRead = 0; // Counter for bytes read so far for the resource

                    int currentPageIndexInObject = startPageIndexInObject;
                    int currentOffsetWithinPage = startOffsetWithinPage;

                    // Loop through necessary pages to read the entire resource
                    while (bytesRead < resourceSize && currentPageIndexInObject < tableEntry.PageTableEntries)
                    {
                        // Calculate absolute offset of current page entry in Object Page Table
                        int globalPageEntryIndex = (int)tableEntry.PageTableIndex + currentPageIndexInObject;
                        if (globalPageEntryIndex < 0)
                        {
                            message = $"Error: Global page index ({globalPageEntryIndex}) negative. pageTableIndex might be 0 or invalid.";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        int pageEntryOffset = objectPageTableOffset + ((globalPageEntryIndex - 1) * 8);

                        if (pageEntryOffset + 8 > fileBytes.Length || pageEntryOffset < 0)
                        {
                            message = $"Error: Object page entry (global index {globalPageEntryIndex}) out of range or negative offset during multi-page reading.";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        // Bounds checks for Object Page Table Entry fields
                        if (pageEntryOffset + 0 + 4 > fileBytes.Length ||
                            pageEntryOffset + 4 + 2 > fileBytes.Length ||
                            pageEntryOffset + 6 + 2 > fileBytes.Length)
                        {
                            message = $"Essential Object Page Table Entry fields for logical page {currentPageIndexInObject} out of bounds.";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        uint currentPageDataOffset = BitConverter.ToUInt32(fileBytes, pageEntryOffset);
                        ushort pageDataLengthInFile = BitConverter.ToUInt16(fileBytes, pageEntryOffset + 4); // Actual data size of this page in file
                        ushort pageFlags = BitConverter.ToUInt16(fileBytes, pageEntryOffset + 6);

                        Console.WriteLine($"    Reading Logical Page {currentPageIndexInObject}: PageDataOffset: 0x{currentPageDataOffset:X}, LengthInFile: {pageDataLengthInFile}, Flags: 0x{pageFlags:X2}");

                        uint currentBaseOffset;
                        if (pageFlags == 0x01) // Iterated Data Page
                        {
                            currentBaseOffset = header.ObjectIterPagesOffset;
                            Console.WriteLine("    DEBUG: Detected Iterated Data Page.");
                            int compressedPhysicalOffset = (int)header.DataPagesOffset + ((int)currentPageDataOffset << (int)header.PageOffsetShift);

                            if (compressedPhysicalOffset < 0 || (long)compressedPhysicalOffset + pageDataLengthInFile > fileBytes.LongLength)
                            {
                                message = $"Error: Compressed data for page {currentPageIndexInObject} out of file bounds. Offset: 0x{compressedPhysicalOffset:X}, Length: {pageDataLengthInFile}.";
                                Console.WriteLine(message);
                                return result.ToArray();
                            }

                            byte[] compressedBytes = new byte[pageDataLengthInFile];
                            Array.Copy(fileBytes, compressedPhysicalOffset, compressedBytes, 0, pageDataLengthInFile);

                            try
                            {
                                ModuleResources.DumpRaw(compressedBytes);
                                // *** Call the new RLE decompression function ***
                                byte[] decompressedPageData = DecompressRlePage(compressedBytes, (int)header.PageSize); // pageSize is the logical size 
                                pageDataLengthInFile = (ushort)decompressedPageData.Length; // Update length for consistency, though it should be pageSize
                                Console.WriteLine($"    DEBUG: RLE page decompressed successfully. Decompressed size: {decompressedPageData.Length} bytes.");
                                if (decompressedPageData.Length < header.PageSize)
                                {
                                    Console.WriteLine($"    DEBUG: Decompressed size ({decompressedPageData.Length}) smaller than PageSize ({header.PageSize}). Page will be treated as partially zero-filled.");
                                }

                                ModuleResources.DumpRaw(decompressedPageData);
                                // In this case, we handle resource copying after decompression.
                                // The resource might span multiple pages.
                                // We don't do 'return decompressedPageData;' here, but copy into resData.
                                uint bytesToCopyFromDecompressedPage = Math.Min(resourceSize - bytesRead, (uint)(decompressedPageData.Length - currentOffsetWithinPage));
                                bytesToCopyFromDecompressedPage = Math.Min(bytesToCopyFromDecompressedPage, (uint)(header.PageSize - currentOffsetWithinPage)); // Don't copy more than the logical page size

                                Array.Copy(decompressedPageData, currentOffsetWithinPage, resData, (int)bytesRead, (int)bytesToCopyFromDecompressedPage);
                                bytesRead += bytesToCopyFromDecompressedPage;
                                currentPageIndexInObject++;
                                currentOffsetWithinPage = 0;
                                continue; // Move to next iteration of main loop
                            }
                            catch (Exception ex)
                            {
                                message = $"Error during RLE decompression of page {currentPageIndexInObject}: {ex.Message}";
                                Console.WriteLine(message);
                                return result.ToArray();
                            }
                        }
                        else if (pageFlags == 0x00) // Legal Physical Page
                        {
                            currentBaseOffset = header.DataPagesOffset;
                            Console.WriteLine("    DEBUG: Detected Legal Data Page.");
                        }
                        else if (pageFlags == 0x05) // Compressed Page - This is ITERDATA2 (UnpackMethod2)
                        {
                            Console.WriteLine("DEBUG: Detected Compressed Page (0x05 - ITERDATA2). Attempting decompression with UnpackMethod2.");

                            // Calculate compressedPhysicalOffset for flag 0x05
                            int compressedPhysicalOffset = (int)header.DataPagesOffset + ((int)currentPageDataOffset << (int)header.PageOffsetShift);

                            if (compressedPhysicalOffset < 0 || (long)compressedPhysicalOffset + pageDataLengthInFile > fileBytes.LongLength)
                            {
                                message = $"Error: Compressed data for page {currentPageIndexInObject} out of file bounds. Offset: 0x{compressedPhysicalOffset:X}, Length: {pageDataLengthInFile}.";
                                Console.WriteLine(message);
                                return result.ToArray();
                            }

                            byte[] rawCompressedBlock = new byte[pageDataLengthInFile];
                            Array.Copy(fileBytes, compressedPhysicalOffset, rawCompressedBlock, 0, pageDataLengthInFile);

                            try
                            {
                                bool decompressionSuccess;
                                // Pass rawCompressedBlock and its original length to UnpackMethod2
                                byte[] decompressedPageData = UnpackMethod2(ref rawCompressedBlock, rawCompressedBlock.Length, (int)header.PageSize, out decompressionSuccess);

                                if (decompressionSuccess)
                                {
                                    Console.WriteLine($"DEBUG: Page decompressed successfully with UnpackMethod2. Decompressed size: {decompressedPageData.Length} bytes.");
                                    // Copy decompressed data to final resource
                                    uint bytesToCopyFromDecompressedPage = Math.Min(resourceSize - bytesRead, (uint)(decompressedPageData.Length - currentOffsetWithinPage));
                                    bytesToCopyFromDecompressedPage = Math.Min(bytesToCopyFromDecompressedPage, (uint)(header.PageSize - currentOffsetWithinPage));

                                    Array.Copy(decompressedPageData, currentOffsetWithinPage, resData, (int)bytesRead, (int)bytesToCopyFromDecompressedPage);
                                    bytesRead += bytesToCopyFromDecompressedPage;
                                    currentPageIndexInObject++;
                                    currentOffsetWithinPage = 0;
                                    continue; // Move to next iteration of loop
                                }
                                else
                                {
                                    Console.WriteLine("ERROR: Page decompression with UnpackMethod2 failed.");
                                    message = "Page decompression with UnpackMethod2 failed.";
                                    return result.ToArray(); // Or handle error
                                }
                            }
                            catch (Exception ex)
                            {
                                Console.WriteLine($"ERROR: Exception during LxLz page decompression: {ex.Message}");
                                message = $"Exception during LxLz decompression: {ex.Message}";
                                return result.ToArray(); // Or handle error
                            }
                        }
                        else if (pageFlags == 0x03) // Zero Filled Page - Data will be zeros, nothing to physically read
                        {
                            Console.WriteLine("    DEBUG: Detected Zero-Filled Page. Will fill corresponding resource part with zeros.");
                            // Calculate how many bytes from this page should be part of the resource and are zero-filled.
                            // It's the minimum between remaining resource bytes and bytes this page can contribute (from current offset to end of logical page)
                            uint bytesToFillZero = Math.Min(resourceSize - bytesRead, (uint)(header.PageSize - currentOffsetWithinPage));
                            // resData is already initialized with zeros, so no need for Array.Clear
                            bytesRead += bytesToFillZero;
                            currentPageIndexInObject++;
                            currentOffsetWithinPage = 0; // For next page, start from beginning
                            continue; // Move to next iteration of loop
                        }
                        else
                        {
                            message = $"Error: Unsupported page type (flags = 0x{pageFlags:X2}) for resource extraction at logical page {currentPageIndexInObject}.";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        // This block will only execute for pageFlags == 0x00 (Legal Physical Page)
                        // as 0x01, 0x05 and 0x03 use 'continue' or 'return'
                        int currentPhysicalDataBlockStart = (int)currentBaseOffset + ((int)currentPageDataOffset << (int)header.PageOffsetShift);

                        // Physical offset in file to start copying resource-specific data from this page
                        int copyPhysicalOffset = currentPhysicalDataBlockStart + currentOffsetWithinPage;

                        // How many bytes we can copy from this *specific part* of the page for the *resource*
                        uint bytesToCopyFromThisPage = Math.Min(resourceSize - bytesRead, (uint)(pageDataLengthInFile - currentOffsetWithinPage));
                        bytesToCopyFromThisPage = Math.Min(bytesToCopyFromThisPage, (uint)(header.PageSize - currentOffsetWithinPage));

                        Console.WriteLine($"    Copying from Physical Offset: 0x{copyPhysicalOffset:X}, Bytes to Copy in this iteration: {bytesToCopyFromThisPage}");

                        if (copyPhysicalOffset < 0 || (long)copyPhysicalOffset + bytesToCopyFromThisPage > fileBytes.LongLength)
                        {
                            message = $"Error: Attempt to read beyond file bounds for page {currentPageIndexInObject}. Calculated offset: 0x{copyPhysicalOffset:X}, Bytes to copy: {bytesToCopyFromThisPage}.";
                            Console.WriteLine(message);
                            return result.ToArray();
                        }

                        Array.Copy(fileBytes, copyPhysicalOffset, resData, (int)bytesRead, (int)bytesToCopyFromThisPage);
                        bytesRead += bytesToCopyFromThisPage;

                        currentPageIndexInObject++; // Move to next logical page of object
                        currentOffsetWithinPage = 0; // For subsequent pages, reading always starts from page beginning
                    }

                    if (bytesRead < resourceSize)
                    {
                        message = $"Warning: Not all resource bytes were read ({bytesRead}/{resourceSize}). Missing {resourceSize - bytesRead} bytes. This might indicate a resource fragmented beyond mapped pages or a malformed file.";
                        Console.WriteLine(message);
                        // Remaining bytes in resData will be 0x00, which might be expected behavior for unmapped/zero-filled pages
                    }
                    else
                    {
                        message = $"LX resource '{typeName}' (ID: {nameID}) found and extracted successfully.\nSize: {resourceSize} bytes.\r\n";
                        found = true;
                    }
                    Console.WriteLine(message);
                    return resData;
                }
            }

            message = $"Resource '{typeName}' (Name: {targetResourceName}) not found in LX file.";
            Console.WriteLine(message);
            return result.ToArray();
        }
    }
}
