using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

namespace PeareModule
{
    public static class RT_BITMAP
    {
        // This is an entrypoint also for RT_POINTER. A bitmap array can be the base for both the types.
        // This return as list of Bitmap for compatibility with OS/2 Bitmap Array
        public static List<Bitmap> Get(byte[] resData)
        {
            var result = new List<Bitmap>();

            try
            {
                // Check for OS/2 BITMAPARRAYHEADER (starts with 'BA')
                if (resData.Length >= 2 && resData[0] == 0x42 && resData[1] == 0x41) // 'BA'
                {
                    int offset = 0;
                    while (offset < resData.Length)
                    {
                        // Ensure enough data for the initial 'BA' header check and size info
                        if (offset + 14 > resData.Length)
                        {
                            Console.WriteLine($"[DEBUG] Not enough data for 'BA' header at offset {offset}. Aborting.");
                            break;
                        }

                        // Validate 'BA' signature for the current entry
                        if (resData[offset] != 0x42 || resData[offset + 1] != 0x41)
                        {
                            Console.WriteLine($"[DEBUG] Invalid 'BA' signature at offset {offset}. Aborting.");
                            break;
                        }

                        // int headerSize = BitConverter.ToInt32(resData, offset + 2); // Not used in current logic, consider removing if truly unused.
                        int nextOffset = BitConverter.ToInt32(resData, offset + 6);
                        int bmpOffset = offset + 14;

                        if (bmpOffset >= resData.Length)
                        {
                            Console.WriteLine($"[DEBUG] BMP data offset {bmpOffset} beyond end of data. Aborting.");
                            break;
                        }

                        // Calculate the size of the current bitmap data
                        int currentBmpSize = (nextOffset > 0 && nextOffset > offset && nextOffset < resData.Length)
                                                ? nextOffset - bmpOffset
                                                : resData.Length - bmpOffset;

                        // Ensure we don't try to read beyond the array bounds
                        if (bmpOffset + currentBmpSize > resData.Length)
                        {
                            Console.WriteLine($"[DEBUG] Calculated BMP size ({currentBmpSize}) from offset {bmpOffset} exceeds data length. Adjusting.");
                            currentBmpSize = resData.Length - bmpOffset;
                        }

                        byte[] bmpData = resData.Skip(bmpOffset).Take(currentBmpSize).ToArray();

                        Bitmap bmp = TryDecodeBitmap(bmpData, resData);
                        bool imageDecoded = bmp != null;

                        if (imageDecoded)
                        {
                            result.Add(bmp);
                        }

                        string status = imageDecoded ? "loaded successfully" : "failed to load";
                        Console.WriteLine($"Bitmap or Pointer {status} at offset {offset}");

                        // Move to the next offset for the next bitmap
                        if (nextOffset <= offset || nextOffset >= resData.Length)
                        {
                            Console.WriteLine($"[DEBUG] Next offset {nextOffset} is invalid or end of data. Stopping loop.");
                            break;
                        }
                        offset = nextOffset;
                    }
                }
                else // Not an OS/2 BITMAPARRAYHEADER, try to decode as a single bitmap
                {
                    Bitmap bmp = TryDecodeBitmap(resData, resData);
                    bool imageDecoded = bmp != null;

                    if (imageDecoded)
                    {
                        result.Add(bmp);
                    }

                    string status = imageDecoded ? "loaded successfully" : "failed to load";
                    Console.WriteLine($"Single Bitmap or Pointer {status}");
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("Failed to process bitmap data: " + ex.Message);
            }
            return result;
        }


        private static Bitmap TryDecodeBitmap(byte[] data, byte[] fullData)
        {
            Bitmap bmp = Decode_BITMAP(data, fullData);
            if (bmp != null)
            {
                return bmp;
            }

            bmp = Decode_RT_POINTER_V1(data, fullData);
            if (bmp != null)
            {
                Console.WriteLine("Data was pointer. Decoded with Decode_RT_POINTER.");
                return bmp;
            }

            bmp = Decode_RT_POINTER_V2(data, fullData);
            if (bmp != null)
            {
                Console.WriteLine("Data was pointer. Decoded with Decode_RT_POINTER_V2.");
                return bmp;
            }

            bmp = Decode_BITMAP_Win1_Win2(data);
            if (bmp != null)
            {
                Console.WriteLine("Data was bitmap. Decoded with Decode_BITMAP_Win1_Win2.");
                return bmp;
            }

            return null;
        }


        public static Bitmap Decode_BITMAP_Win1_Win2(byte[] resourceData)
        {
            // Check for null input data
            if (resourceData == null)
            {
                Console.WriteLine("Error: Resource data cannot be null.");
                return null;
            }

            // Check for minimum header size
            if (resourceData.Length < 16) // Minimum header is 16 bytes
            {
                Console.WriteLine("Error: Resource data is too short to contain the header.");
                return null;
            }

            // Read key values from the header (Little Endian)
            UInt32 resourceId = BitConverter.ToUInt32(resourceData, 0);   // Offset 0-3
            UInt16 width = BitConverter.ToUInt16(resourceData, 4);       // Offset 4-5
            UInt16 height = BitConverter.ToUInt16(resourceData, 6);      // Offset 6-7
            UInt16 bytesPerLine = BitConverter.ToUInt16(resourceData, 8); // Offset 8-9 (Stride)

            Console.WriteLine($"Header Detected: ResourceID=0x{resourceId:X}, Width={width}, Height={height}, BytesPerLine={bytesPerLine}");

            // Calculate the expected pixel data size based on the header
            int expectedPixelDataSize = height * bytesPerLine;
            int headerSize = 16; // Fixed header size

            // Adjust expected pixel data size if the provided data is shorter
            if (resourceData.Length < headerSize + expectedPixelDataSize)
            {
                // Theoretical minimum for 1bpp
                if (resourceData.Length - headerSize < (width + 7) / 8 * height)
                {
                    Console.WriteLine($"Warning: Dump data seems insufficient for a {width}x{height} 1bpp image with stride {bytesPerLine}. Attempting to decode the {resourceData.Length - headerSize} available bytes anyway.");
                }
                expectedPixelDataSize = resourceData.Length - headerSize; // Adapt size to what is actually available
            }

            // Ensure width and height are valid before creating Bitmap
            if (width == 0 || height == 0)
            {
                Console.WriteLine($"Error: Invalid image dimensions. Width: {width}, Height: {height}.");
                return null;
            }

            // Create the monochrome bitmap
            Bitmap bitmap = null;
            try
            {
                bitmap = new Bitmap(width, height, PixelFormat.Format1bppIndexed);
            }
            catch (ArgumentException ex)
            {
                Console.WriteLine($"Error creating bitmap with dimensions {width}x{height}: {ex.Message}");
                return null;
            }
            catch (Exception ex)
            {
                Console.WriteLine($"An unexpected error occurred while creating the bitmap: {ex.Message}");
                return null;
            }

            // Set the palette for black and white (0 = black, 1 = white)
            ColorPalette pal = bitmap.Palette;
            pal.Entries[0] = Color.Black; // Bit 0 is black
            pal.Entries[1] = Color.White; // Bit 1 is white
            bitmap.Palette = pal;

            BitmapData bmpData = null;
            try
            {
                // Lock the bitmap bits for direct memory access
                // The internal C# bitmap Scan0 will handle its own stride, which might differ from bytesPerLine
                bmpData = bitmap.LockBits(new Rectangle(0, 0, bitmap.Width, bitmap.Height), ImageLockMode.WriteOnly, PixelFormat.Format1bppIndexed);

                IntPtr destPtr = bmpData.Scan0;
                int sourceOffset = headerSize;

                for (int y = 0; y < height; y++)
                {
                    // Calculate bytes to copy for the current line, ensuring we don't read beyond resourceData bounds
                    int bytesToCopy = Math.Min(bytesPerLine, resourceData.Length - sourceOffset);

                    // Also ensure we don't write beyond the bitmap's stride for the current line
                    bytesToCopy = Math.Min(bytesToCopy, bmpData.Stride);

                    if (bytesToCopy <= 0)
                    {
                        Console.WriteLine($"Warning: No more valid data to copy for row {y}. Remaining resource data length: {resourceData.Length - sourceOffset}.");
                        break; // No more valid data to copy
                    }

                    // Copy data from source buffer to bitmap buffer
                    Marshal.Copy(resourceData, sourceOffset, destPtr, bytesToCopy);

                    sourceOffset += bytesPerLine; // Advance in the source data buffer based on its stride
                    destPtr = new IntPtr(destPtr.ToInt64() + bmpData.Stride); // Advance in the bitmap buffer based on its internal stride
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error during bitmap data copying: {ex.Message}");
                // If an error occurs here, ensure the bitmap is disposed before returning null
                if (bitmap != null)
                {
                    bitmap.Dispose();
                }
                return null;
            }
            finally
            {
                if (bmpData != null)
                {
                    bitmap.UnlockBits(bmpData);
                }
            }

            return bitmap;
        }

        private static Bitmap Decode_BITMAP(byte[] bmpData, byte[] resData)
        {
            try
            {
                Console.Write("\r\n\r\nData found:\r\n");
                ModuleResources.DumpRaw(bmpData);

                var bmpOS2 = Decode_BITMAP_OS2_V1(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine("Data was bitmap. Decoded with Decode_BITMAP_OS2_V1.");
                    return bmpOS2;
                }

                bmpOS2 = Decode_BITMAP_OS2_ArrayPart(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine("Data was bitmap. Decoded with Decode_BITMAP_OS2_ArrayPart.");
                    return bmpOS2;
                }

                bmpOS2 = Decode_BITMAP_OS2_V2(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine("Data was bitmap. Decoded with Decode_BITMAP_OS2_V2.");
                    return bmpOS2;
                }

                string dataStripped = "";

                // Check if it's a full BMP file (starts with 'BM')
                if (bmpData.Length >= 14 && bmpData[0] == 0x42 && bmpData[1] == 0x4D)
                {
                    uint bfOffBits = BitConverter.ToUInt32(bmpData, 10);

                    if (bfOffBits <= bmpData.Length)
                    {
                        // Valid bfOffBits, try loading with GDI+
                        try
                        {
                            using (MemoryStream ms = new MemoryStream(bmpData))
                            {
                                Console.WriteLine("Data was bitmap. Decoded with GDI+.");
                                return new Bitmap(ms);
                            }
                        }
                        catch
                        {
                            // Remove file header and proceed with DIB
                            dataStripped = "after stripping the first 14 bytes ";
                            bmpData = bmpData.Skip(14).ToArray();
                        }
                    }
                    else
                    {
                        // Invalid bfOffBits, remove file header and proceed
                        dataStripped = "after stripping the first 14 bytes ";
                        bmpData = bmpData.Skip(14).ToArray();
                    }
                }

                // Try again after removing the BMP signature
                bmpOS2 = Decode_BITMAP_OS2_V1(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine($"Data was bitmap. Decoded {dataStripped}with Decode_BITMAP_OS2_V1.");
                    return bmpOS2;
                }

                bmpOS2 = Decode_BITMAP_OS2_ArrayPart(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine($"Data was bitmap. Decoded {dataStripped}with Decode_BITMAP_OS2_ArrayPart.");
                    return bmpOS2;
                }

                bmpOS2 = Decode_BITMAP_OS2_V2(bmpData, resData);
                if (bmpOS2 != null)
                {
                    Console.WriteLine($"Data was bitmap. Decoded {dataStripped}with Decode_BITMAP_OS2_V2.");
                    return bmpOS2;
                }

                // Windows-style DIB (BITMAPINFOHEADER)
                try
                {
                    int biSize = BitConverter.ToInt32(bmpData, 0);
                    int bfOffBits = 14 + biSize;
                    int bfSize = bfOffBits + bmpData.Length - biSize;

                    using (MemoryStream ms = new MemoryStream())
                    using (BinaryWriter bw = new BinaryWriter(ms))
                    {
                        bw.Write((ushort)0x4D42); // 'BM'
                        bw.Write((uint)bfSize);
                        bw.Write((ushort)0);
                        bw.Write((ushort)0);
                        bw.Write((uint)bfOffBits);
                        bw.Write(bmpData);
                        ms.Position = 0;
                        Console.WriteLine("Data was bitmap. Decoded after stripping the first 14 bytes with Windows-style DIB.");
                        return new Bitmap(ms);
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Reading as Windows-style DIB (BITMAPINFOHEADER) failed");
                }

                return null;
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseBMP failed: " + ex.Message);
                return null;
            }
        }

        private static Bitmap Decode_RT_POINTER_V1(byte[] CIresData, byte[] bitmapArray)
        {
            using (var ms = new MemoryStream(CIresData))
            using (var reader = new BinaryReader(ms))
            {
                // === First block (mask) ===
                ushort header = reader.ReadUInt16();
                // "CI", "IC", "CP", "PT" are basically the same format, for a different use.
                // The main difference is that the bitmap data block might be missing and
                // there is only the first block (mask) that contains also the bitmap data.
                // IC icon (OS/2 1.x)
                // CI icon (OS/2 2.x+)
                // PT pointer
                if (header != 0x4943 && header != 0x4349 && header != 0x5043 && header != 0x5043 && header != 0x5450) 
                    return null; 
                _ = reader.ReadUInt32(); // fileSize
                ushort xHotspotMask = reader.ReadUInt16(); // xHotspot
                ushort yHotspotMask = reader.ReadUInt16(); // yHotspot
                uint bitmapOffsetMask = reader.ReadUInt32();

                if (reader.ReadUInt32() != 12) return null; // BitmapCoreHeader size
                ushort widthMask = reader.ReadUInt16();
                ushort heightMask = reader.ReadUInt16(); // This is double the height of the bitmap!
                ushort planesMask = reader.ReadUInt16(); // planes
                ushort bppMask = reader.ReadUInt16(); // bpp (should be 1)

                if (bppMask < 1)
                {
                    // Invalid bpp detected.
                    Console.WriteLine("Invalid bpp detected. bpp has manually set to 1.");
                    bppMask = 1;
                }

                if (planesMask != 1 || (bppMask != 1 && bppMask != 4 && bppMask != 8))
                    return null;

                int numColorsMask = 1 << bppMask;

                // --- COLOR PALETTE ---
                var paletteMask = new Color[numColorsMask];
                for (int i = 0; i < numColorsMask; i++)
                {
                    byte blue = reader.ReadByte();
                    byte green = reader.ReadByte();
                    byte red = reader.ReadByte();
                    paletteMask[i] = Color.FromArgb(red, green, blue);
                }

                // === Second block (color image) ===
                if (reader.BaseStream.Position + 2 <= reader.BaseStream.Length && reader.ReadUInt16() == header)
                {
                    // Second block found!
                    _ = reader.ReadUInt32(); // fileSize
                    ushort xHotspot = reader.ReadUInt16();
                    ushort yHotspot = reader.ReadUInt16();
                    uint bitmapOffset = reader.ReadUInt32();

                    if (reader.ReadUInt32() != 12) return null;
                    ushort width = reader.ReadUInt16();
                    ushort height = reader.ReadUInt16();
                    ushort planes = reader.ReadUInt16();
                    ushort bpp = reader.ReadUInt16();

                    if (planes != 1 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24)) // Pointers can be 1, 4, 8, 24 bpp
                    {
                        return null; // Unsupported color format
                    }
                    int numColors = 1 << bpp;
                    if (bpp == 24) numColors = 0; // 24-bit images usually don't have a palette

                    var palette = new Color[numColors];
                    for (int i = 0; i < numColors; i++)
                    {
                        byte blue = reader.ReadByte();
                        byte green = reader.ReadByte();
                        byte red = reader.ReadByte();
                        palette[i] = Color.FromArgb(red, green, blue);
                    }

                    // === Calculate size ===
                    int stride = ((width * bpp + 31) / 32) * 4;
                    int strideMask = ((widthMask * bppMask + 31) / 32) * 4;

                    // We use the real weight, already given by the bitmap, not from the mask.
                    // From the mask it would be heightMask / 2
                    int bitmapSize = stride * height;
                    int maskSize = strideMask * height;

                    // === Read data from bitmapArray ===
                    if (bitmapOffsetMask + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];
                    if (bitmapOffset + bitmapSize > bitmapArray.Length)
                    {
                        // Out of offset. Must fallback to the mask as bitmap data.
                        Console.WriteLine("Invalid bitmapOffset detected. Using bitmapOffsetMask.");
                        Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    }
                    else
                    {
                        Array.Copy(bitmapArray, bitmapOffset, colorData, 0, bitmapSize);
                    }
                    Array.Copy(bitmapArray, bitmapOffsetMask + maskSize, maskData, 0, maskSize); // We skip the first half that we don't care about

                    return GenerateBitmapFromData(colorData, maskData, width, height, bpp, palette);
                }
                else
                {
                    // Second block not found.
                    // We use the bitmap data from the first half of mask data.

                    int stride = ((widthMask * bppMask + 31) / 32) * 4;
                    int maskStride = ((widthMask + 31) / 32) * 4; // 1bpp mask

                    int realHeight = heightMask / 2;
                    int bitmapSize = stride * realHeight;
                    int maskSize = maskStride * realHeight;

                    if (bitmapOffsetMask + bitmapSize + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];

                    Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    Array.Copy(bitmapArray, bitmapOffsetMask + bitmapSize, maskData, 0, maskSize);

                    return GenerateBitmapFromData(colorData, maskData, widthMask, realHeight, bppMask, paletteMask);
                }
            }
        }

        private static Bitmap Decode_RT_POINTER_V2(byte[] CIresData, byte[] bitmapArray)
        {
            using (var ms = new MemoryStream(CIresData))
            using (var reader = new BinaryReader(ms))
            {
                // "CI", "IC", "CP", "PT" are basically the same format, for a different use.
                // The main difference is that the bitmap data block might be missing and
                // there is only the first block (mask) that contains also the bitmap data.
                // IC icon (OS/2 1.x)
                // CI icon (OS/2 2.x+)
                // PT pointer
                ushort header = reader.ReadUInt16();
                if (header != 0x4943 && header != 0x4349 && header != 0x5043 && header != 0x5450)
                {
                    return null;
                }

                // Skip fileSize and read hotspot and bitmapOffset for the mask
                _ = reader.ReadUInt32(); // fileSize (total size of the resource)
                ushort xHotspotMask = reader.ReadUInt16(); // xHotspot
                ushort yHotspotMask = reader.ReadUInt16(); // yHotspot
                uint bitmapOffsetMask = reader.ReadUInt32(); // Offset to the actual bitmap data (for the mask)

                // Read BITMAPINFOHEADER2 for the mask
                int maskInfoOffset = (int)reader.BaseStream.Position; // Current position is the start of BITMAPINFOHEADER2
                uint cbFixMask = reader.ReadUInt32();
                uint widthMask = reader.ReadUInt32();
                uint heightMask = reader.ReadUInt32(); // This height is typically double the actual height for icons/pointers (mask + image)
                ushort planesMask = reader.ReadUInt16();
                ushort bppMask = reader.ReadUInt16(); // Should be 1 bpp for the mask
                _ = reader.ReadUInt32(); // ulCompression (should be 0 for uncompressed)
                uint cbImageMask = reader.ReadUInt32(); // Size of the raw pixel data for mask
                _ = reader.ReadUInt32(); // xpelsPerMeter
                _ = reader.ReadUInt32(); // ypelsPerMeter
                uint cclrUsedMask = reader.ReadUInt32(); // Number of colors in the color table (for mask, usually 2 for black/white)
                _ = reader.ReadUInt32(); // clrImportant

                // Validate mask properties
                if (planesMask != 1 || bppMask != 1) // Mask should typically be 1 bpp and 1 plane
                {
                    Console.WriteLine("Warning: Mask properties unexpected (planes != 1 or bpp != 1). Attempting to proceed.");
                    // return null; // Or handle more gracefully
                }

                // Determine the number of colors in the palette for the mask
                int numColorsMask = (int)(cclrUsedMask != 0 ? (int)cclrUsedMask : (1 << (bppMask * planesMask)));
                if (numColorsMask == 0) numColorsMask = 2; // For 1bpp, at least 2 colors (black/white)

                int colorTableOffsetMask = 14 + (int)cbFixMask;
                ms.Seek(colorTableOffsetMask, SeekOrigin.Begin);

                // --- COLOR PALETTE for the mask ---
                var paletteMask = new Color[numColorsMask];
                for (int i = 0; i < numColorsMask; i++)
                {
                    // RGB2 structure: BYTE bBlue, BYTE bGreen, BYTE bRed, BYTE fcOptions
                    byte blue = reader.ReadByte();
                    byte green = reader.ReadByte();
                    byte red = reader.ReadByte();
                    _ = reader.ReadByte(); // fcOptions - not used for color representation
                    paletteMask[i] = Color.FromArgb(red, green, blue);
                }

                // --- Second block (color image) ---
                if (reader.BaseStream.Position + 2 <= reader.BaseStream.Length && reader.ReadUInt16() == header)
                {
                    // Second block found! This is the actual color image data.
                    _ = reader.ReadUInt32(); // fileSize
                    ushort xHotspot = reader.ReadUInt16();
                    ushort yHotspot = reader.ReadUInt16();
                    uint bitmapOffset = reader.ReadUInt32(); // Offset to the actual bitmap data (for the color image)

                    // Read BITMAPINFOHEADER2 for the color image
                    int colorInfoOffset = (int)reader.BaseStream.Position;
                    uint cbFix = reader.ReadUInt32();
                    uint width = reader.ReadUInt32();
                    uint height = reader.ReadUInt32(); // This should be the actual height
                    ushort planes = reader.ReadUInt16();
                    ushort bpp = reader.ReadUInt16();
                    _ = reader.ReadUInt32(); // ulCompression
                    uint cbImage = reader.ReadUInt32(); // Size of the raw pixel data for color image
                    _ = reader.ReadUInt32(); // xpelsPerMeter
                    _ = reader.ReadUInt32(); // ypelsPerMeter
                    uint cclrUsed = reader.ReadUInt32(); // Number of colors in the color table
                    _ = reader.ReadUInt32(); // clrImportant

                    // Validate color image properties
                    if (planes != 1 || (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 24)) // Pointers can be 1, 4, 8, 24 bpp
                    {
                        return null; // Unsupported color format
                    }
                    int numColors = (int)(cclrUsed != 0 ? (int)cclrUsed : (1 << (bpp * planes)));
                    if (bpp == 24) numColors = 0; // 24-bit images usually don't have a palette

                    int colorTableOffset = colorInfoOffset + (int)cbFix;
                    ms.Seek(colorTableOffset, SeekOrigin.Begin);

                    // --- COLOR PALETTE for the color image ---
                    var palette = new Color[numColors];
                    for (int i = 0; i < numColors; i++)
                    {
                        byte blue = reader.ReadByte();
                        byte green = reader.ReadByte();
                        byte red = reader.ReadByte();
                        _ = reader.ReadByte(); // fcOptions
                        palette[i] = Color.FromArgb(red, green, blue);
                    }

                    // --- Read data from bitmapArray ---
                    // The actual pixel data for both mask and color image is typically in bitmapArray,
                    // and the offsets (`bitmapOffsetMask`, `bitmapOffset`) point to their start within `bitmapArray`.

                    int stride = (((int)width * bpp + 31) / 32) * 4;
                    int strideMask = (((int)widthMask * bppMask + 31) / 32) * 4;

                    // We use the real weight, already given by the bitmap, not from the mask.
                    // From the mask it would be heightMask / 2
                    int bitmapSize = stride * (int)height;
                    int maskSize = strideMask * (int)height;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];
                    if (bitmapOffset + bitmapSize > bitmapArray.Length)
                    {
                        // Out of offset. Must fallback to the mask as bitmap data.
                        Console.WriteLine("Invalid bitmapOffset detected. Using bitmapOffsetMask.");
                        Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    }
                    else
                    {
                        Array.Copy(bitmapArray, bitmapOffset, colorData, 0, bitmapSize);
                    }
                    Array.Copy(bitmapArray, bitmapOffsetMask + maskSize, maskData, 0, maskSize); // We skip the first half that we don't care about

                    return GenerateBitmapFromData(colorData, maskData, (int)width, (int)height, bpp, palette);
                }
                else
                {
                    // Second block not found.
                    // We use the bitmap data from the first half of mask data.

                    int stride = (((int)widthMask * bppMask + 31) / 32) * 4;
                    int maskStride = (((int)widthMask + 31) / 32) * 4; // 1bpp mask

                    int realHeight = (int)heightMask / 2;
                    int bitmapSize = stride * realHeight;
                    int maskSize = maskStride * realHeight;

                    if (bitmapOffsetMask + bitmapSize + maskSize > bitmapArray.Length)
                        return null;

                    byte[] colorData = new byte[bitmapSize];
                    byte[] maskData = new byte[maskSize];

                    Array.Copy(bitmapArray, bitmapOffsetMask, colorData, 0, bitmapSize);
                    Array.Copy(bitmapArray, bitmapOffsetMask + bitmapSize, maskData, 0, maskSize);

                    return GenerateBitmapFromData(colorData, maskData, (int)widthMask, realHeight, bppMask, paletteMask);
                }
            }
        }
        private static Bitmap Decode_BITMAP_OS2_V2(byte[] data, byte[] resData)
        {
            ushort usType = BitConverter.ToUInt16(data, 0);

            if (usType != 0x4D42) // 'BM' in little-endian
                return null;

            // Offsets for BITMAPFILEHEADER2 fields
            const int BFH2_OFFBITS_OFFSET = 10;
            const int BFH2_BMP2_OFFSET = 14; // Start of BITMAPINFOHEADER2 within BITMAPFILEHEADER2

            // Offsets for BITMAPINFOHEADER2 fields (relative to the start of BITMAPINFOHEADER2)
            const int BIH2_CBFIX_OFFSET = 0;
            const int BIH2_CX_OFFSET = 4;
            const int BIH2_CY_OFFSET = 8;
            const int BIH2_CPLANES_OFFSET = 12;
            const int BIH2_CBITCOUNT_OFFSET = 14;
            const int BIH2_ULCOMPRESSION_OFFSET = 16;
            const int BIH2_CBIMAGE_OFFSET = 20;
            const int BIH2_CCLRUSED_OFFSET = 32;

            // Read BITMAPFILEHEADER2
            uint pixelDataOffset = BitConverter.ToUInt32(data, BFH2_OFFBITS_OFFSET);    // offset to the pel data from the beginning of the *file*
            int bitmapInfoOffset = BFH2_BMP2_OFFSET;                                   // BITMAPINFOHEADER2 starts at BFH2_BMP2_OFFSET relative to the start of 'data'

            // Read BITMAPINFOHEADER2 fields
            uint cbFix = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_CBFIX_OFFSET);          // Size of the structure.
            uint width = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_CX_OFFSET);             // Width
            uint height = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_CY_OFFSET);           // Height
            ushort cPlanes = BitConverter.ToUInt16(data, bitmapInfoOffset + BIH2_CPLANES_OFFSET);     // Color planes (usually 1)
            ushort bitCount = BitConverter.ToUInt16(data, bitmapInfoOffset + BIH2_CBITCOUNT_OFFSET); // Bits per pixel
            uint ulCompression = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_ULCOMPRESSION_OFFSET); // Compression scheme
            uint cbImage = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_CBIMAGE_OFFSET);     // Size of the compressed/uncompressed pel data
            uint cclrUsed = BitConverter.ToUInt32(data, bitmapInfoOffset + BIH2_CCLRUSED_OFFSET);     // Number of colors in the color table

            // Validate cPlanes. The docs say "I've never seen this set to anything other than 1."
            if (cPlanes != 1)
            {
                Console.WriteLine($"Warning: cPlanes is {cPlanes}. Expected 1.");
                // Puoi scegliere di lanciare un'eccezione, loggare o ignorare in base alla tolleranza agli errori.
            }

            // Determine the number of colors in the palette
            int numColors;
            if (bitCount <= 8) // Paletted images usually have bitCount 1, 4, 8
            {
                numColors = (int)(cclrUsed != 0 ? (int)cclrUsed : (1 << bitCount));
            }
            else // For 16, 24-bit, 32-bit images, cclrUsed gives the actual number of colors or 0 if no palette.
            {
                numColors = (int)cclrUsed;
            }

            Color[] palette = null;
            if (numColors > 0)
            {
                palette = new Color[numColors];
                int colorTableOffset = bitmapInfoOffset + (int)cbFix;

                // Populate the palette
                for (int i = 0; i < numColors; i++)
                {
                    // RGB2 structure: BYTE bBlue, BYTE bGreen, BYTE bRed, BYTE fcOptions
                    byte bBlue = data[colorTableOffset + (i * 4)];
                    byte bGreen = data[colorTableOffset + (i * 4) + 1];
                    byte bRed = data[colorTableOffset + (i * 4) + 2];
                    //byte fcOptions = data[colorTableOffset + (i * 4) + 3]; // Not used for color representation

                    palette[i] = Color.FromArgb(bRed, bGreen, bBlue);
                }
            }

            // --- Extract the raw/compressed pixel data from resData ---
            // This is the *compressed* or *uncompressed* data as it appears in the file.
            // If cbImage is 0, it means uncompressed and the size should be calculated.
            // For compressed data, cbImage should contain the actual size of the compressed data.
            int actualCompressedDataSize = (int)(cbImage == 0 ? (resData.Length - pixelDataOffset) : cbImage);
            if (pixelDataOffset + actualCompressedDataSize > resData.Length)
            {
                actualCompressedDataSize = resData.Length - (int)pixelDataOffset; // Adjust if header says more than available data
            }

            byte[] compressedOrRawData = new byte[actualCompressedDataSize];
            Array.Copy(resData, (int)pixelDataOffset, compressedOrRawData, 0, actualCompressedDataSize);

            // --- Handle Compression ---
            byte[] decompressedPixelData = null;

            Console.WriteLine("Bitmap is compressd: " + ulCompression);

            switch (ulCompression)
            {
                case 0: // BCA_UNCOMP - Uncompressed
                    decompressedPixelData = compressedOrRawData;
                    break;
                case 1: // BCA_HUFFMAN1D - Huffman encoding, monochrome bitmaps only
                    Console.WriteLine("Error: Huffman 1D compression is not implemented. Cannot decompress.");
                    return null;
                case 2: // BCA_RLE4 - Run-length encoded, 4 bits/pixel
                    decompressedPixelData = DecompressRLE4(compressedOrRawData, (int)width, (int)height, palette);
                    break;
                case 3: // BCA_RLE8 - RLE, 8 bits/pixel
                    decompressedPixelData = DecompressRLE8(compressedOrRawData, (int)width, (int)height);
                    break;
                case 4: // BCA_RLE24 - RLE, 24 bits/pixel
                    decompressedPixelData = DecompressRLE24(compressedOrRawData, (int)width, (int)height);
                    break;
                default:
                    Console.WriteLine($"Error: Unsupported compression type: {ulCompression}.");
                    return null;
            }

            if (decompressedPixelData == null)
            {
                Console.WriteLine("Error: Decompression failed or was not possible for the given format.");
                return null;
            }

            // The GenerateBitmapFromData function expects uncompressed, padded pixel data.
            return GenerateBitmapFromData(decompressedPixelData, null, (int)width, (int)height, bitCount, palette);
        }

        private static byte[] DecompressRLE4(byte[] compressedData, int width, int height, Color[] palette)
        {
            // RLE4: 4 bits per pixel. Each byte contains two 4-bit pixels.
            // A common RLE scheme for 4bpp uses special codes.
            // E.g., for Windows BMP RLE4:
            // - Byte 1: Count (N)
            // - Byte 2: Two pixels (e.g., AABB for 4 bits each)
            //   -> N pairs of (A, B) pixels are repeated.
            // - If Byte 1 == 0:
            //   - Byte 2 == 0: End of scanline (EOL)
            //   - Byte 2 == 1: End of bitmap (EOB)
            //   - Byte 2 == 2: Delta (Byte 3 = X, Byte 4 = Y offset) - less common for basic RLE
            //   - Byte 2 > 2: Absolute mode: Byte 2 (N) is number of *pixels* to read, followed by N/2 bytes.
            //     The N bytes are padded to a word (16-bit) boundary.

            int uncompressedBytesPerRow = (((width * 4 + 7) / 8) + 3) / 4 * 4; // Padded to 4-byte boundary
            byte[] decompressed = new byte[uncompressedBytesPerRow * height];
            MemoryStream ms = new MemoryStream(compressedData);
            BinaryReader reader = new BinaryReader(ms);

            int currentX = 0;
            int currentY = height - 1; // Bitmaps are typically stored bottom-up

            int it = 0;
            try
            {
                while (reader.BaseStream.Position < reader.BaseStream.Length && currentY >= 0)
                {
                    byte count = reader.ReadByte();

                    if (count == 0x00) // Escape character
                    {
                        byte command = reader.ReadByte();
                        it++;
                        //GenerateBitmapFromData(decompressed, null, width, height, 4, palette).Save(it.ToString()+".BMP");
                        switch (command)
                        {
                            case 0x00: // End of Line (EOL)
                                currentX = 0;
                                currentY--;
                                // Align to next row's start in decompressed array (if not already there)
                                // This implicit padding is handled by currentX=0 and the `bytesPerOutputRow` calculation
                                // as we move to the next row.
                                break;
                            case 0x01: // End of Bitmap (EOB)
                                goto EndDecompression; // Exit loop
                            case 0x02: // Delta
                                int dx = reader.ReadByte();
                                int dy = reader.ReadByte();
                                currentX += dx;
                                currentY -= dy; // Move up for OS/2 bitmaps
                                break;
                            default: // Absolute Mode: 'command' is the number of pixels
                                int numPixelsToRead = command;
                                int numBytesToRead = (numPixelsToRead + 1) / 2; // Each byte holds 2 pixels (4-bit)

                                for (int i = 0; i < numBytesToRead; i++)
                                {
                                    byte twoPixels = reader.ReadByte();
                                    if (currentX < width)
                                    {
                                        Set4BitPixel(decompressed, currentX, currentY, (byte)((twoPixels >> 4) & 0x0F), width, uncompressedBytesPerRow);
                                        currentX++;
                                    }
                                    if (currentX < width)
                                    {
                                        Set4BitPixel(decompressed, currentX, currentY, (byte)(twoPixels & 0x0F), width, uncompressedBytesPerRow);
                                        currentX++;
                                    }
                                }
                                // Absolute Mode padding: Data is padded to a word boundary (16-bit, 2 bytes)
                                if (numBytesToRead % 2 != 0)
                                {
                                    reader.ReadByte(); // Read padding byte
                                }
                                break;
                        }
                    }
                    else // Encoded Mode: 'count' pixels, followed by the two 4-bit values (one byte)
                    {
                        byte twoPixels = reader.ReadByte();
                        byte pixel1 = (byte)((twoPixels >> 4) & 0x0F);
                        byte pixel2 = (byte)(twoPixels & 0x0F);

                        for (int i = 0; i < count; i++)
                        {
                            if (currentX < width)
                            {
                                Set4BitPixel(decompressed, currentX, currentY, i % 2 == 0 ? pixel1 : pixel2, width, uncompressedBytesPerRow);
                                currentX++;
                            }
                            else
                            {
                                // If we hit width limit, move to next row (should ideally be EOL before this)
                                // This might indicate an invalid stream or that the 'count' spans multiple lines.
                                // Assuming 'count' applies to current line.
                                break;
                            }
                        }
                    }
                }
            }
            catch (EndOfStreamException)
            {
                Console.WriteLine("Warning: Unexpected end of compressed RLE4 data stream.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error during RLE4 decompression: {ex.Message}");
                return null;
            }

        EndDecompression:
            return decompressed;
        }

        private static byte[] DecompressRLE8(byte[] compressedData, int width, int height)
        {
            // RLE8: 8 bits per pixel (1 byte per pixel).
            // Similar RLE scheme to RLE4, but values are 1 byte.
            // - Byte 1: Count (N)
            // - Byte 2: Pixel value (P)
            //   -> N occurrences of P are repeated.
            // - If Byte 1 == 0:
            //   - Byte 2 == 0: End of scanline (EOL)
            //   - Byte 2 == 1: End of bitmap (EOB)
            //   - Byte 2 == 2: Delta (Byte 3 = X, Byte 4 = Y offset)
            //   - Byte 2 > 2: Absolute mode: Byte 2 (N) is number of *pixels* to read, followed by N bytes.
            //     The N bytes are padded to a word (16-bit) boundary.

            int uncompressedBytesPerRow = ((width + 3) / 4) * 4; // Padded to 4-byte boundary
            byte[] decompressed = new byte[uncompressedBytesPerRow * height];
            MemoryStream ms = new MemoryStream(compressedData);
            BinaryReader reader = new BinaryReader(ms);

            int currentX = 0;
            int currentY = height - 1; // Bitmaps are typically stored bottom-up

            try
            {
                while (reader.BaseStream.Position < reader.BaseStream.Length && currentY >= 0)
                {
                    byte count = reader.ReadByte();

                    if (count == 0x00) // Escape character
                    {
                        byte command = reader.ReadByte();
                        switch (command)
                        {
                            case 0x00: // End of Line (EOL)
                                currentX = 0;
                                currentY--;
                                break;
                            case 0x01: // End of Bitmap (EOB)
                                goto EndDecompression; // Exit loop
                            case 0x02: // Delta
                                int dx = reader.ReadByte();
                                int dy = reader.ReadByte();
                                currentX += dx;
                                currentY -= dy; // Move up for OS/2 bitmaps
                                break;
                            default: // Absolute Mode: 'command' is the number of pixels
                                int numPixelsToRead = command;
                                for (int i = 0; i < numPixelsToRead; i++)
                                {
                                    byte pixelValue = reader.ReadByte();
                                    if (currentX < width)
                                    {
                                        Set8BitPixel(decompressed, currentX, currentY, pixelValue, width, uncompressedBytesPerRow);
                                        currentX++;
                                    }
                                }
                                // Absolute Mode padding: Data is padded to a word boundary (16-bit, 2 bytes)
                                if (numPixelsToRead % 2 != 0)
                                {
                                    reader.ReadByte(); // Read padding byte
                                }
                                break;
                        }
                    }
                    else // Encoded Mode: 'count' pixels, followed by pixel value
                    {
                        byte pixelValue = reader.ReadByte();
                        for (int i = 0; i < count; i++)
                        {
                            if (currentX < width)
                            {
                                Set8BitPixel(decompressed, currentX, currentY, pixelValue, width, uncompressedBytesPerRow);
                                currentX++;
                            }
                            else
                            {
                                break; // Reached end of line, should be EOL next
                            }
                        }
                    }
                }
            }
            catch (EndOfStreamException)
            {
                Console.WriteLine("Warning: Unexpected end of compressed RLE8 data stream.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error during RLE8 decompression: {ex.Message}");
                return null;
            }

        EndDecompression:
            return decompressed;
        }

        private static byte[] DecompressRLE24(byte[] compressedData, int width, int height)
        {
            // RLE24: 24 bits per pixel (3 bytes per pixel).
            // RLE for 24bpp is simpler, usually just run-length encoding.
            // - Byte 1: Count (N)
            // - Bytes 2,3,4: BGR color value
            //   -> N occurrences of BGR color are repeated.
            // There might be special codes (0x00 followed by command), but often 24bpp is just simple runs.
            // We'll implement the simple run mode, as explicit OS/2 RLE24 documentation is scarce.
            // If a file uses complex RLE24 (like 0x00 escape codes), this would need adjustment.

            int uncompressedBytesPerRow = (((width * 3 + 3) / 4) * 4); // Padded to 4-byte boundary
            byte[] decompressed = new byte[uncompressedBytesPerRow * height];
            MemoryStream ms = new MemoryStream(compressedData);
            BinaryReader reader = new BinaryReader(ms);

            int currentX = 0;
            int currentY = height - 1; // Bitmaps are typically stored bottom-up

            try
            {
                while (reader.BaseStream.Position < reader.BaseStream.Length && currentY >= 0)
                {
                    byte count = reader.ReadByte();

                    if (count == 0x00)
                    {
                        // This is a common pattern for special codes in RLE.
                        // For 24bpp, the common sub-codes are EOL, EOB, Delta.
                        // We'll use a pragmatic approach, similar to RLE4/RLE8 special codes.
                        byte command = reader.ReadByte();
                        switch (command)
                        {
                            case 0x00: // End of Line (EOL)
                                currentX = 0;
                                currentY--;
                                break;
                            case 0x01: // End of Bitmap (EOB)
                                goto EndDecompression;
                            case 0x02: // Delta
                                int dx = reader.ReadByte();
                                int dy = reader.ReadByte();
                                currentX += dx;
                                currentY -= dy;
                                break;
                            default: // Absolute Mode: 'command' is the number of pixels
                                     // For 24bpp absolute mode, it's 'command' * 3 bytes.
                                int numPixelsToRead = command;
                                for (int i = 0; i < numPixelsToRead; i++)
                                {
                                    byte b = reader.ReadByte();
                                    byte g = reader.ReadByte();
                                    byte r = reader.ReadByte();
                                    if (currentX < width)
                                    {
                                        Set24BitPixel(decompressed, currentX, currentY, b, g, r, width, uncompressedBytesPerRow);
                                        currentX++;
                                    }
                                }
                                // Absolute Mode padding: Data is padded to a word boundary (16-bit)
                                // For 24bpp, this means padding to an even number of bytes for the data block.
                                // If (numPixelsToRead * 3) is odd, add a padding byte.
                                if ((numPixelsToRead * 3) % 2 != 0)
                                {
                                    reader.ReadByte(); // Read padding byte
                                }
                                break;
                        }
                    }
                    else // Encoded Mode: 'count' pixels, followed by BGR value
                    {
                        byte b = reader.ReadByte();
                        byte g = reader.ReadByte();
                        byte r = reader.ReadByte();
                        for (int i = 0; i < count; i++)
                        {
                            if (currentX < width)
                            {
                                Set24BitPixel(decompressed, currentX, currentY, b, g, r, width, uncompressedBytesPerRow);
                                currentX++;
                            }
                            else
                            {
                                break; // Reached end of line, should be EOL next
                            }
                        }
                    }
                }
            }
            catch (EndOfStreamException)
            {
                Console.WriteLine("Warning: Unexpected end of compressed RLE24 data stream.");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"Error during RLE24 decompression: {ex.Message}");
                return null;
            }

        EndDecompression:
            return decompressed;
        }

        // Helper to calculate the starting index of a row in the decompressed array.
        // Bitmap data is usually stored bottom-up, so row 0 in the image is height-1 in the array.
        private static int GetRowStartIndex(int y, int height, int uncompressedBytesPerRow)
        {
            return (height - 1 - y) * uncompressedBytesPerRow;
        }

        // Set a 4-bit pixel (packed in bytes)
        private static void Set4BitPixel(byte[] data, int x, int y, byte pixelValue, int width, int uncompressedBytesPerRow)
        {
            int rowStart = GetRowStartIndex(y, (int)Math.Ceiling((double)data.Length / uncompressedBytesPerRow), uncompressedBytesPerRow);
            int byteIndex = rowStart + (x / 2); // Each byte holds two 4-bit pixels

            if (x % 2 == 0) // First pixel in the byte (upper 4 bits)
            {
                data[byteIndex] = (byte)((data[byteIndex] & 0x0F) | (pixelValue << 4));
            }
            else // Second pixel in the byte (lower 4 bits)
            {
                data[byteIndex] = (byte)((data[byteIndex] & 0xF0) | pixelValue);
            }
        }

        // Set an 8-bit pixel
        private static void Set8BitPixel(byte[] data, int x, int y, byte pixelValue, int width, int uncompressedBytesPerRow)
        {
            int rowStart = GetRowStartIndex(y, (int)Math.Ceiling((double)data.Length / uncompressedBytesPerRow), uncompressedBytesPerRow);
            data[rowStart + x] = pixelValue;
        }

        // Set a 24-bit pixel (BGR order)
        private static void Set24BitPixel(byte[] data, int x, int y, byte b, byte g, byte r, int width, int uncompressedBytesPerRow)
        {
            int rowStart = GetRowStartIndex(y, (int)Math.Ceiling((double)data.Length / uncompressedBytesPerRow), uncompressedBytesPerRow);
            int pixelStart = rowStart + (x * 3);
            data[pixelStart] = b;
            data[pixelStart + 1] = g;
            data[pixelStart + 2] = r;
        }

        private static Bitmap Decode_BITMAP_OS2_ArrayPart(byte[] bmpData, byte[] resData)
        {
            try
            {
                if (bmpData.Length < 14 || !(bmpData[0] == 0x42 && bmpData[1] == 0x4D))
                {
                    Console.WriteLine($"[DEBUG] Decode_BITMAP_OS2_V1_Alt: Not a bitmap.");
                    return null;
                }
                int offset = 14;

                uint bcSize = BitConverter.ToUInt32(bmpData, offset + 0);
                ushort width = BitConverter.ToUInt16(bmpData, offset + 4);
                ushort height = BitConverter.ToUInt16(bmpData, offset + 6);
                ushort planes = BitConverter.ToUInt16(bmpData, offset + 8);
                ushort bitCount = BitConverter.ToUInt16(bmpData, offset + 10);

                // Validate planes (always 1 for standard bitmaps)
                if (planes != 1)
                {
                    Console.WriteLine("[DEBUG] Decode_BITMAP_OS2_V1_Alt: Unsupported planes count: " + planes);
                    return null;
                }

                // --- Calculate Palette Information ---
                int numColors = 0;
                if (bitCount <= 8) // Indexed color formats (1, 4, 8 bpp)
                {
                    numColors = 1 << bitCount;
                }
                else if (bitCount == 24) // 24-bit has no palette 
                {
                    numColors = 0;
                }
                else
                {
                    Console.WriteLine($"[DEBUG] Decode_BITMAP_OS2_V1_Alt: Unsupported bit depth: {bitCount}");
                    return null;
                }

                int paletteOffsetFromBCHStart = offset + (int)bcSize;
                int paletteSize = numColors * 3; // OS/2 palettes use 3 bytes (BGR) per entry

                // Check 'data' length for palette. 'data' contains BCH and palette.
                if (numColors > 0 && bmpData.Length < paletteOffsetFromBCHStart + paletteSize)
                {
                    Console.WriteLine("[DEBUG] Decode_BITMAP_OS2_V1_Alt: 'data' (BCH + palette) too short for full palette.");
                    return null;
                }

                Color[] palette = null;
                if (numColors > 0)
                {
                    palette = new Color[numColors];
                    for (int i = 0; i < numColors; i++)
                    {
                        // Read palette entries from the 'data' array
                        byte b = bmpData[paletteOffsetFromBCHStart + i * 3 + 0];
                        byte g = bmpData[paletteOffsetFromBCHStart + i * 3 + 1];
                        byte r = bmpData[paletteOffsetFromBCHStart + i * 3 + 2];
                        palette[i] = Color.FromArgb(r, g, b); // OS/2 is BGR, System.Drawing is RGB
                    }
                }

                // --- Locate Pixel Data in 'resData' ---
                if (resData.Length < 14) // Ensure resData has at least the full BITMAPFILEHEADER
                {
                    Console.WriteLine("[DEBUG] Decode_BITMAP_OS2_V1_Alt: 'resData' too short to read BITMAPFILEHEADER.");
                    return null;
                }
                uint bfOffBits = BitConverter.ToUInt16(bmpData, 10); 

                // Calculate expected size of pixel data
                int bitsPerLine = width * bitCount;
                int stride = ((bitsPerLine + 31) / 32) * 4; // Scanline padded to 4-byte boundary
                int totalPixelBytes = stride * height;

                // Ensure 'resData' is large enough to contain the pixel data
                if (resData.Length < bfOffBits + (long)totalPixelBytes)
                {
                    Console.WriteLine($"[DEBUG] Decode_BITMAP_OS2_V1_Alt: 'resData' too short for pixel data. Expected {bfOffBits + totalPixelBytes}, got {resData.Length}.");
                    return null;
                }

                // Extract the pixel data into a new array from 'resData'.
                byte[] bitmapData = new byte[totalPixelBytes];
                Array.Copy(resData, (int)bfOffBits, bitmapData, 0, totalPixelBytes);

                return GenerateBitmapFromData(bitmapData, null, width, height, bitCount, palette);
            }
            catch (Exception ex)
            {
                // Re-using original debug message structure
                Console.WriteLine("[DEBUG] TryParseOS2V1 failed: " + ex.Message);
                return null;
            }
        }

        private static Bitmap Decode_BITMAP_OS2_V1(byte[] data, byte[] resData)
        {
            try
            {
                if (data.Length < 12)
                    return null;

                ushort bcSize = BitConverter.ToUInt16(data, 10);
                ushort width = BitConverter.ToUInt16(data, 14);
                ushort height = BitConverter.ToUInt16(data, 16);
                ushort planes = BitConverter.ToUInt16(data, 18);
                ushort bitCount = BitConverter.ToUInt16(data, 20);
                int numColors = 1 << bitCount;
                int paletteSize = numColors * 3; // 3 bytes per color (RGB)
                int paletteOffset = bcSize - paletteSize;

                if (planes != 1 || (bitCount != 1 && bitCount != 4 && bitCount != 8))
                {
                    Console.WriteLine("[DEBUG] Unsupported OS/2 v1 planes or BPP: planes={0}, bpp={1}", planes, bitCount);
                    return null;
                }

                if (data.Length < paletteOffset + paletteSize)
                {
                    Console.WriteLine("[DEBUG] Data too short for palette (header size: {0})", bcSize);
                    return null;
                }

                Color[] palette = new Color[numColors];
                for (int i = 0; i < numColors; i++)
                {
                    byte b = data[paletteOffset + i * 3 + 0];
                    byte g = data[paletteOffset + i * 3 + 1];
                    byte r = data[paletteOffset + i * 3 + 2];
                    palette[i] = Color.FromArgb(r, g, b);
                }

                int pixelOffset = paletteOffset + paletteSize;
                int bitsPerLine = width * bitCount;
                int stride = ((bitsPerLine + 31) / 32) * 4; // Scanline padded to 4-byte boundary

                int totalPixelBytes = stride * height;
                if (data.Length < pixelOffset + totalPixelBytes)
                {
                    Console.WriteLine("[DEBUG] Data too short for pixel rows (header size: {0})", bcSize);
                    return null;
                }

                byte[] bitmapData = new byte[totalPixelBytes];
                Array.Copy(data, pixelOffset, bitmapData, 0, totalPixelBytes);

                return GenerateBitmapFromData(bitmapData, null, width, height, bitCount, palette);
            }
            catch (Exception ex)
            {
                Console.WriteLine("[DEBUG] TryParseOS2V1 failed: " + ex.Message);
                return null;
            }
        }

        public static Bitmap GenerateBitmapFromData(byte[] pixelData, byte[] maskData, int width, int height, int bitCount, Color[] palette)
        {
            // Unified function to decode the bitmap given the data.
            // It is made to work with everything, Windows Bitmaps, Icons, Cursors and OS/2 Bitmaps, Icons, Pointers etc.
            int colorStride = ((width * bitCount + 31) / 32) * 4;
            int maskStride = ((width + 31) / 32) * 4;

            if (bitCount == 4 && palette.Length == 0)
            {
                // Palette EGA Windows as fallback with bitCount=4 when is missing 
                palette = new Color[]
                {
                    Color.Black, Color.Blue, Color.Green, Color.Cyan,
                    Color.Red, Color.Magenta, Color.Brown, Color.LightGray,
                    Color.DarkGray, Color.LightBlue, Color.LightGreen, Color.LightCyan,
                    Color.LightCoral, Color.LightPink, Color.Yellow, Color.White
                };
            }

            Bitmap bmp = new Bitmap(width, height, PixelFormat.Format32bppArgb);
            BitmapData bmpData = bmp.LockBits(new Rectangle(0, 0, width, height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);

            bool applyMask = maskData != null && maskData.Length > 0;

            unsafe
            {
                byte* ptr = (byte*)bmpData.Scan0;

                for (int y = 0; y < height; y++)
                {
                    int invY = height - 1 - y;

                    for (int x = 0; x < width; x++)
                    {
                        Color color = Color.Magenta;

                        int pixelOffset = invY * colorStride;
                        if (bitCount == 32)
                        {
                            int off = pixelOffset + x * 4;
                            if (off + 3 < pixelData.Length)
                            {
                                byte b = pixelData[off];
                                byte g = pixelData[off + 1];
                                byte r = pixelData[off + 2];
                                byte a = pixelData[off + 3];
                                color = Color.FromArgb(a, r, g, b);
                            }
                        }
                        else if (bitCount == 24)
                        {
                            int off = pixelOffset + x * 3;
                            if (off + 2 < pixelData.Length)
                            {
                                byte b = pixelData[off];
                                byte g = pixelData[off + 1];
                                byte r = pixelData[off + 2];
                                color = Color.FromArgb(255, r, g, b);
                            }
                        }
                        else if (bitCount == 8 && palette != null)
                        {
                            int off = pixelOffset + x;
                            if (off < pixelData.Length)
                            {
                                int idx = pixelData[off];
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 4 && palette != null)
                        {
                            int off = pixelOffset + (x / 2);
                            if (off < pixelData.Length)
                            {
                                byte val = pixelData[off];
                                int idx = (x % 2 == 0) ? (val >> 4) : (val & 0x0F);
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }
                        else if (bitCount == 1 && palette != null)
                        {
                            int off = pixelOffset + (x / 8);
                            if (off < pixelData.Length)
                            {
                                byte val = pixelData[off];
                                int idx = (val >> (7 - (x % 8))) & 1;
                                if (idx < palette.Length)
                                    color = palette[idx];
                            }
                        }

                        // AND mask (optional)
                        if (applyMask)
                        {
                            int maskByteIndex = invY * maskStride + (x / 8);
                            int maskBit = 7 - (x % 8);
                            if (maskByteIndex < maskData.Length)
                            {
                                bool isTransparent = (maskData[maskByteIndex] & (1 << maskBit)) != 0;
                                if (isTransparent)
                                {
                                    color = Color.FromArgb(0, color.R, color.G, color.B);
                                }
                            }
                        }

                        byte* pixelPtr = ptr + y * bmpData.Stride + x * 4;
                        pixelPtr[0] = color.B;
                        pixelPtr[1] = color.G;
                        pixelPtr[2] = color.R;
                        pixelPtr[3] = color.A;
                    }
                }
            }

            bmp.UnlockBits(bmpData);
            return bmp;
        }

        public static void CopyLarge(byte[] source, long sourceOffset, byte[] destination, int destOffset, long count)
        {
            const int chunkSize = 1024 * 1024; // 1MB chunks

            while (count > 0)
            {
                int thisChunk = (int)Math.Min(count, chunkSize);

                if (sourceOffset > int.MaxValue)
                    throw new OverflowException("Source offset exceeds supported range.");

                Buffer.BlockCopy(source, (int)sourceOffset, destination, destOffset, thisChunk);
                sourceOffset += thisChunk;
                destOffset += thisChunk;
                count -= thisChunk;
            }
        }
    }
}
