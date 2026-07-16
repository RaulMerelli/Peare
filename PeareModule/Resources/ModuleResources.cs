using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;

namespace PeareModule
{
    public static class ModuleResources
    {
        public static T Deserialize<T>(byte[] array) where T : struct
        {
            var size = Marshal.SizeOf(typeof(T));
            var ptr = Marshal.AllocHGlobal(size);
            Marshal.Copy(array, 0, ptr, size);
            var s = (T)Marshal.PtrToStructure(ptr, typeof(T));
            Marshal.FreeHGlobal(ptr);
            return s;
        }

        public static string DumpRaw(byte[] data, bool showAddressAndAscii = true)
        {
            if (data == null || data.Length == 0)
            {
                Console.WriteLine("No data.");
                return "No data.";
            }

            int offset = 0;
            StringBuilder result = new StringBuilder();

            for (int line = 0; line < data.Length; line += 16)
            {
                int lineOffset = offset + line;
                int lineLength = Math.Min(16, data.Length - line);

                StringBuilder hex = new StringBuilder();
                for (int j = 0; j < lineLength; j++)
                {
                    hex.AppendFormat("{0:X2} ", data[lineOffset + j]);
                }

                hex.Append(' ', (16 - lineLength) * 3); // pad hex column

                StringBuilder ascii = new StringBuilder();
                for (int j = 0; j < lineLength; j++)
                {
                    byte b = data[lineOffset + j];
                    ascii.Append(b >= 32 && b <= 126 ? (char)b : '.');
                }
                string lineStr = "";
                if (showAddressAndAscii)
                {
                    lineStr = $"{lineOffset:X04}: {hex}| {ascii}";
                }
                else
                {
                    lineStr = $"{hex}";
                }
                Console.WriteLine(lineStr);
                result.AppendLine(lineStr);
            }

            Console.WriteLine();
            result.AppendLine();

            return result.ToString();
        }

        public static dynamic RawDetect(byte[] resData, ModuleProperties properties)
        {
            if (resData == null || resData.Length == 0)
                return null;

            // Fonts have a sufficiently distinctive header and must be tried before
            // generic binary/image detection.
            if (IsOS2Fnt(resData))
            {
                try
                {
                    return OS2_RT_FONT.Decode(resData);
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Header matched an OS/2 font, but decoding failed: " + ex.Message);
                }
            }

            if (IsWindowsFnt(resData))
            {
                try
                {
                    // The header identifies a Windows FNT even when it is stored in
                    // a module targeting another platform. Force the Windows parser.
                    ModuleProperties fontProperties = new ModuleProperties
                    {
                        Description = properties == null ? null : properties.Description,
                        filePath = properties == null ? null : properties.filePath,
                        headerType = HeaderType.PE,
                        versionType = VersionType.Windows
                    };
                    return RT_FONT.Decode(resData, fontProperties);
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Header matched a Windows font, but decoding failed: " + ex.Message);
                }
            }

            // Windows Media Player stores its internal skins as binary WSZ trees.
            // Decode these before generic image/text detection.
            string wszText;
            if (WszDecoder.TryDecode(resData, out wszText))
                return wszText;

            // Complete image files (PNG, JPEG, GIF, BMP, TIFF, ICO/CUR, WMF/EMF)
            // can be decoded directly by GDI+ without knowing the resource type name.
            Bitmap standardImage = TryDecodeStandardImage(resData);
            if (standardImage != null)
                return standardImage;

            // A cursor resource is a DIB preceded by hotspot X/Y values.
            if (IsDibHeader(resData, 4))
            {
                try
                {
                    Img cursor = RT_CURSOR.Get(resData);
                    if (cursor != null && cursor.Bitmap != null)
                        return cursor;
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Header matched a cursor, but decoding failed: " + ex.Message);
                }
            }

            // RT_BITMAP already handles Windows DIBs, OS/2 bitmap arrays and
            // OS/2 IC/CI/CP/PT pointer/icon formats.
            if (IsDibHeader(resData, 0) || HasBitmapResourceSignature(resData))
            {
                try
                {
                    List<Img> images = RT_BITMAP.Get(resData);
                    if (images != null && images.Count > 0)
                        return images;
                }
                catch (Exception ex)
                {
                    Console.WriteLine("Header matched an image resource, but decoding failed: " + ex.Message);
                }
            }

            // Text remains a string, so the UI can use its existing text viewer.
            string text = TryDecodeText(resData);
            if (text != null)
                return text;

            return null;
        }

        private static bool IsOS2Fnt(byte[] data)
        {
            // The signature is nine characters. Depending on the producer it is
            // followed by NUL, a space or additional header data. Requiring the
            // tenth byte to be a space caused valid standalone OS/2 fonts to be
            // missed by RawDetect.
            return data.Length >= 17 && Encoding.ASCII.GetString(data, 8, 9) == "OS/2 FONT";
        }

        private static bool IsWindowsFnt(byte[] data)
        {
            if (data.Length < 149)
                return false;

            ushort version = BitConverter.ToUInt16(data, 0);
            if (version != 0x0100 && version != 0x0200 && version != 0x0300)
                return false;

            uint declaredSize = BitConverter.ToUInt32(data, 2);
            if (declaredSize < 117 || declaredSize > data.Length)
                return false;

            ushort pixelHeight = BitConverter.ToUInt16(data, 88);
            byte firstChar = data[95];
            byte lastChar = data[96];
            uint bitsOffset = BitConverter.ToUInt32(data, 113);

            return pixelHeight > 0 &&
                   lastChar >= firstChar &&
                   bitsOffset > 0 &&
                   bitsOffset < data.Length;
        }

        private static Bitmap TryDecodeStandardImage(byte[] data)
        {
            if (!HasStandardImageSignature(data))
                return null;

            try
            {
                // ICO files are more reliably decoded through Icon than Image.FromStream.
                if (data.Length >= 4 &&
                    data[0] == 0x00 && data[1] == 0x00 &&
                    (data[2] == 0x01 || data[2] == 0x02) && data[3] == 0x00)
                {
                    using (MemoryStream stream = new MemoryStream(data, false))
                    using (Icon icon = new Icon(stream))
                    {
                        return icon.ToBitmap();
                    }
                }

                using (MemoryStream stream = new MemoryStream(data, false))
                using (Image image = Image.FromStream(stream, true, true))
                {
                    // Clone the image because GDI+ otherwise keeps a dependency on the stream.
                    return new Bitmap(image);
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine("Known image header found, but GDI+ decoding failed: " + ex.Message);
                return null;
            }
        }

        private static bool HasStandardImageSignature(byte[] data)
        {
            if (data.Length >= 8 &&
                data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47 &&
                data[4] == 0x0D && data[5] == 0x0A && data[6] == 0x1A && data[7] == 0x0A)
                return true; // PNG

            if (data.Length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
                return true; // JPEG

            if (data.Length >= 6 &&
                ((Encoding.ASCII.GetString(data, 0, 6) == "GIF87a") ||
                 (Encoding.ASCII.GetString(data, 0, 6) == "GIF89a")))
                return true;

            if (data.Length >= 4 &&
                ((data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00) ||
                 (data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A)))
                return true; // TIFF

            if (data.Length >= 2 && data[0] == 0x42 && data[1] == 0x4D)
                return true; // BMP file

            if (data.Length >= 4 && data[0] == 0x00 && data[1] == 0x00 &&
                ((data[2] == 0x01 && data[3] == 0x00) ||
                 (data[2] == 0x02 && data[3] == 0x00)))
                return true; // ICO or CUR file

            if (data.Length >= 4 &&
                data[0] == 0xD7 && data[1] == 0xCD && data[2] == 0xC6 && data[3] == 0x9A)
                return true; // Placeable WMF

            if (data.Length >= 44 &&
                BitConverter.ToUInt32(data, 0) == 1 &&
                data[40] == 0x20 && data[41] == 0x45 && data[42] == 0x4D && data[43] == 0x46)
                return true; // EMF

            return false;
        }

        private static bool HasBitmapResourceSignature(byte[] data)
        {
            if (data.Length < 2)
                return false;

            ushort signature = BitConverter.ToUInt16(data, 0);
            return signature == 0x4142 || // BA - OS/2 bitmap array
                   signature == 0x4D42 || // BM - bitmap file
                   signature == 0x4943 || // CI
                   signature == 0x4349 || // IC
                   signature == 0x5043 || // CP
                   signature == 0x5450;   // PT
        }

        private static bool IsDibHeader(byte[] data, int offset)
        {
            if (data == null || offset < 0 || data.Length < offset + 12)
                return false;

            uint headerSize = BitConverter.ToUInt32(data, offset);
            if (headerSize == 12)
            {
                ushort width = BitConverter.ToUInt16(data, offset + 4);
                ushort height = BitConverter.ToUInt16(data, offset + 6);
                ushort planes = BitConverter.ToUInt16(data, offset + 8);
                ushort bitCount = BitConverter.ToUInt16(data, offset + 10);
                return width > 0 && height > 0 && planes == 1 && IsKnownBitCount(bitCount);
            }

            if (headerSize == 16 || headerSize == 40 || headerSize == 52 ||
                headerSize == 56 || headerSize == 64 || headerSize == 108 ||
                headerSize == 124)
            {
                if (data.Length < offset + 16)
                    return false;

                int width = BitConverter.ToInt32(data, offset + 4);
                int height = BitConverter.ToInt32(data, offset + 8);
                ushort planes = BitConverter.ToUInt16(data, offset + 12);
                ushort bitCount = BitConverter.ToUInt16(data, offset + 14);

                return width > 0 && height != 0 && planes == 1 && IsKnownBitCount(bitCount);
            }

            return false;
        }

        private static bool IsKnownBitCount(ushort bitCount)
        {
            return bitCount == 1 || bitCount == 2 || bitCount == 4 ||
                   bitCount == 8 || bitCount == 16 || bitCount == 24 ||
                   bitCount == 32;
        }

        private static string TryDecodeText(byte[] data)
        {
            if (data == null || data.Length == 0)
                return null;

            Encoding encoding = null;
            int offset = 0;
            bool hasBom = true;
            bool inferredUnicode = false;

            if (data.Length >= 4 && data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFE && data[3] == 0xFF)
            {
                encoding = new UTF32Encoding(true, true, true);
                offset = 4;
            }
            else if (data.Length >= 4 && data[0] == 0xFF && data[1] == 0xFE && data[2] == 0x00 && data[3] == 0x00)
            {
                encoding = new UTF32Encoding(false, true, true);
                offset = 4;
            }
            else if (data.Length >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
            {
                encoding = new UTF8Encoding(false, true);
                offset = 3;
            }
            else if (data.Length >= 2 && data[0] == 0xFE && data[1] == 0xFF)
            {
                encoding = new UnicodeEncoding(true, true, true);
                offset = 2;
            }
            else if (data.Length >= 2 && data[0] == 0xFF && data[1] == 0xFE)
            {
                encoding = new UnicodeEncoding(false, true, true);
                offset = 2;
            }
            else
            {
                hasBom = false;
                if (LooksLikeUtf16(data, true))
                {
                    encoding = new UnicodeEncoding(false, false, true);
                    inferredUnicode = true;
                }
                else if (LooksLikeUtf16(data, false))
                {
                    encoding = new UnicodeEncoding(true, false, true);
                    inferredUnicode = true;
                }
                else
                    encoding = new UTF8Encoding(false, true);
            }

            string text;
            try
            {
                text = encoding.GetString(data, offset, data.Length - offset);
            }
            catch (DecoderFallbackException)
            {
                if (hasBom || !LooksLikeSingleByteText(data))
                    return null;

                text = Encoding.Default.GetString(data);
            }

            text = text.TrimEnd('\0');
            if (!LooksLikeReadableText(text))
                return null;

            string trimmed = text.TrimStart('\uFEFF', ' ', '\t', '\r', '\n');
            if (trimmed.Length == 0)
                return text;

            if (hasBom || inferredUnicode || HasKnownTextHeader(trimmed) || LooksLikeSingleByteText(data))
                return text;

            return null;
        }

        private static bool HasKnownTextHeader(string text)
        {
            return text.StartsWith("<!DOCTYPE html", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<html", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<head", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<body", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<?xml", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<svg", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<manifest", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("<assembly", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("{\\rtf", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("@charset", StringComparison.OrdinalIgnoreCase) ||
                   text.StartsWith("{", StringComparison.Ordinal) ||
                   text.StartsWith("[", StringComparison.Ordinal);
        }

        private static bool LooksLikeUtf16(byte[] data, bool littleEndian)
        {
            int pairs = Math.Min(data.Length / 2, 128);
            if (pairs < 4)
                return false;

            int expectedZeroes = 0;
            int unexpectedZeroes = 0;
            for (int i = 0; i < pairs; i++)
            {
                byte first = data[i * 2];
                byte second = data[i * 2 + 1];
                byte expected = littleEndian ? second : first;
                byte unexpected = littleEndian ? first : second;
                if (expected == 0) expectedZeroes++;
                if (unexpected == 0) unexpectedZeroes++;
            }

            return expectedZeroes >= pairs * 3 / 5 && unexpectedZeroes <= pairs / 5;
        }

        private static bool LooksLikeSingleByteText(byte[] data)
        {
            int sampleLength = Math.Min(data.Length, 4096);
            while (sampleLength > 0 && data[sampleLength - 1] == 0)
                sampleLength--;
            if (sampleLength == 0)
                return false;

            int readable = 0;
            int zeroes = 0;
            for (int i = 0; i < sampleLength; i++)
            {
                byte value = data[i];
                if (value == 0)
                    zeroes++;
                if (value == 9 || value == 10 || value == 13 || value >= 32)
                    readable++;
            }

            return zeroes == 0 && readable >= sampleLength * 9 / 10;
        }

        private static bool LooksLikeReadableText(string text)
        {
            if (String.IsNullOrEmpty(text))
                return false;

            int sampleLength = Math.Min(text.Length, 4096);
            int readable = 0;
            int zeroes = 0;
            for (int i = 0; i < sampleLength; i++)
            {
                char value = text[i];
                if (value == '\0')
                    zeroes++;
                if (value == '\t' || value == '\r' || value == '\n' || !Char.IsControl(value))
                    readable++;
            }

            return zeroes <= Math.Max(1, sampleLength / 100) &&
                   readable >= sampleLength * 9 / 10;
        }

        public static List<string[]> ListTypesAndRes(string currentFilePath)
        {
            switch (GetModuleProperties(currentFilePath).headerType)
            {
                case HeaderType.PE:
                    return PeResources.OpenPE(currentFilePath);
                case HeaderType.NE:
                    return NeResources.OpenNE(currentFilePath);
                case HeaderType.LE:
                    return LeResources.OpenLE(currentFilePath);
                case HeaderType.LX:
                    return LxResources.OpenLX(currentFilePath);
            }
            return new List<string[]>();
        }

        public static byte[] OpenResource(string currentFilePath, string typeName, string targetResourceName, out string message, out bool found)
        {
            message = "";
            found = false;
            ModuleProperties properties = GetModuleProperties(currentFilePath);
            switch (properties.headerType)
            {
                case HeaderType.PE:
                    return PeResources.OpenResourcePE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.NE:
                    return NeResources.OpenResourceNE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.LE:
                    return LeResources.OpenResourceLE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.LX:
                    return LxResources.OpenResourceLX(properties, typeName, targetResourceName, out message, out found);
            }
            return null;
        }

        public static byte[] OpenResource(ModuleProperties properties, string typeName, string targetResourceName, out string message, out bool found)
        {
            message = "";
            found = false;
            switch (properties.headerType)
            {
                case HeaderType.PE:
                    return PeResources.OpenResourcePE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.NE:
                    return NeResources.OpenResourceNE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.LE:
                    return LeResources.OpenResourceLE(properties, typeName, targetResourceName, out message, out found);
                case HeaderType.LX:
                    return LxResources.OpenResourceLX(properties, typeName, targetResourceName, out message, out found);
            }
            return null;
        }

        public enum HeaderType
        {
            Error,
            MZonly,
            PE,
            NE,
            LE,
            LX
        }

        public enum VersionType
        {
            Unknown,
            OS2,
            Windows,
            MSDOS4,
            Win386,
            IBMMPN
        }

        public class ModuleProperties
        {
            public string Description;
            public HeaderType headerType;
            public VersionType versionType;
            public string filePath;
        }

        // Read the header and check file type
        public static ModuleProperties GetModuleProperties(string path)
        {
            ModuleProperties result = new ModuleProperties
            {
                filePath = path,
                versionType = VersionType.Unknown,
                headerType = HeaderType.Error
            };
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    // 1. Verify if is MZ
                    ushort mzSignature = br.ReadUInt16();

                    // 2. Go to offset 0x3C in order to find the extended header offset
                    fs.Seek(0x3C, SeekOrigin.Begin);
                    int headerOffset = br.ReadInt32();

                    if (headerOffset + 2 > fs.Length)
                    {
                        if (mzSignature == 0x5A4D)
                        {
                            result.headerType = HeaderType.MZonly;
                            result.Description = "MZ with invalid secondary header";
                            return result;
                        }
                        else
                        {
                            result.Description = "Not an executable (no MZ header)";
                            return result;
                        }
                    }

                    // 3. Go to extended header and read the signature
                    fs.Seek(headerOffset, SeekOrigin.Begin);
                    ushort signature = br.ReadUInt16();

                    string version = "";

                    if (signature == 0x454E)
                    {
                        // targetOS NE
                        fs.Seek(headerOffset + 0x36, SeekOrigin.Begin);
                    }
                    else if (signature == 0x454C || signature == 0x584C)
                    {
                        // targetOS LE/LX
                        fs.Seek(headerOffset + 0x0A, SeekOrigin.Begin);
                    }

                    // NE/LE/LX
                    if (new int[] { 0x454E, 0x454C, 0x584C }.Contains(signature))
                    {
                        byte targetOS = br.ReadByte();

                        switch (targetOS)
                        {
                            case 0x00:
                                if (signature == 0x454E)
                                {
                                    result.versionType =
                                        DetectUnknownNeTarget(br, fs, headerOffset);

                                    switch (result.versionType)
                                    {
                                        case VersionType.OS2:
                                            version = " for OS/2";
                                            break;

                                        case VersionType.MSDOS4:
                                            version = " for MS-DOS 4.x";
                                            break;

                                        default:
                                            version = " for unknown OS";
                                            break;
                                    }
                                }
                                else
                                {
                                    version = " for unknown OS";
                                }
                                break;
                            case 0x01:
                                result.versionType = VersionType.OS2;
                                version = " for OS/2";
                                break;
                            case 0x02:
                                result.versionType = VersionType.Windows;
                                version = " for Windows";
                                break;
                            case 0x03:
                                result.versionType = VersionType.MSDOS4;
                                version = " for MS-DOS 4.x";
                                break;
                            case 0x04:
                                result.versionType = VersionType.Win386;
                                version = " for Windows 386";
                                break;
                            case 0x05:
                                result.versionType = VersionType.IBMMPN;
                                version = " for IBM Microkernel Personality Neutral";
                                break;
                        }
                    }

                    // NE/LE/LX/PE
                    if (new int[] { 0x454E, 0x454C, 0x584C, 0x4550 }.Contains(signature))
                    {
                        switch (signature)
                        {
                            case 0x4550:
                                result.headerType = HeaderType.PE;
                                result.versionType = VersionType.Windows;
                                result.Description = $"PE (Portable Executable{version})";
                                break;
                            case 0x454E:
                                result.headerType = HeaderType.NE;
                                result.Description = $"NE (New Executable{version})";
                                break;
                            case 0x584C:
                                result.headerType = HeaderType.LX;
                                result.Description = $"LX (Linear Executable Extended{version})";
                                break;
                            case 0x454C:
                                result.headerType = HeaderType.LE;
                                result.Description = $"LE (Linear Executable{version})";
                                break;
                        }
                        return result;
                    }

                    if (mzSignature == 0x5A4D)
                    {
                        result.headerType = HeaderType.MZonly;

                        // 4. Search for typical packer signatures
                        fs.Seek(0, SeekOrigin.Begin);
                        byte[] fullData = br.ReadBytes((int)Math.Min(fs.Length, 4096)); // max 4 KB 

                        string fullText = System.Text.Encoding.ASCII.GetString(fullData);

                        if (fullText.Contains("UPX!"))
                        {
                            result.Description = "MZ (possibly packed with UPX)";
                        }
                        else if (fullText.Contains("PKLITE"))
                        {
                            result.Description = "MZ (possibly packed with PKLITE)";
                        }
                        else if (fullText.Contains("LZ91") || fullText.Contains("LZEXE"))
                        {
                            result.Description = "MZ (possibly packed with LZEXE)";
                        }
                        else if (fullText.Contains("EXEPACK"))
                        {
                            result.Description = "MZ (possibly packed with EXEPACK)";
                        }
                        else
                        {
                            result.Description = "MZ without known secondary header (maybe plain DOS MZ or unknown packer)";
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                result.Description = "An error happened analyzing the file";
            }
            return result;
        }

        private static VersionType DetectUnknownNeTarget(BinaryReader br, Stream fs, int neHeaderOffset)
        {
            // Byte 2 header NE: linker major version
            fs.Seek(neHeaderOffset + 0x02, SeekOrigin.Begin);
            byte linkerMajor = br.ReadByte();

            // MT-DOS 4.x uses linker NE 4.x.
            if (linkerMajor == 4)
                return VersionType.MSDOS4;

            // Number of refs to imported modules: ne_cmod
            fs.Seek(neHeaderOffset + 0x1E, SeekOrigin.Begin);
            ushort moduleCount = br.ReadUInt16();

            // Module Reference Table: ne_modtab
            fs.Seek(neHeaderOffset + 0x28, SeekOrigin.Begin);
            ushort moduleTableOffset = br.ReadUInt16();

            // Imported Names Table: ne_imptab
            ushort importedNamesOffset = br.ReadUInt16();

            long moduleTablePosition = neHeaderOffset + moduleTableOffset;

            long importedNamesPosition =  neHeaderOffset + importedNamesOffset;

            string[] os2Modules = { "DOSCALLS", "KBDCALLS", "VIOCALLS", "MOUCALLS", "NLS", "QUECALLS" };

            for (int i = 0; i < moduleCount; i++)
            {
                long referencePosition = moduleTablePosition + i * 2L;

                if (referencePosition + 2 > fs.Length)
                    break;

                fs.Seek(referencePosition, SeekOrigin.Begin);
                ushort nameOffset = br.ReadUInt16();

                long namePosition = importedNamesPosition + nameOffset;

                if (namePosition >= fs.Length)
                    continue;

                fs.Seek(namePosition, SeekOrigin.Begin);
                byte nameLength = br.ReadByte();

                if (nameLength == 0 || namePosition + 1 + nameLength > fs.Length)
                    continue;

                string moduleName = Encoding.ASCII.GetString(br.ReadBytes(nameLength)).ToUpperInvariant();

                if (os2Modules.Contains(moduleName))
                    return VersionType.OS2;
            }

            return VersionType.Unknown;
        }
    }
}
