using PeareModule;
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Text;

namespace Peare
{
    internal sealed class ResourceFileFormat
    {
        public ResourceFileFormat(string extension, string description, string mimeType)
        {
            Extension = string.IsNullOrEmpty(extension)
                ? ".bin"
                : (extension.StartsWith(".") ? extension.ToLowerInvariant() : "." + extension.ToLowerInvariant());
            Description = description ?? "Binary data";
            MimeType = mimeType ?? "application/octet-stream";
        }

        public string Extension { get; private set; }
        public string Description { get; private set; }
        public string MimeType { get; private set; }
    }

    internal static class ResourceFormatDetector
    {
        public static ResourceFileFormat Detect(string resourceType, byte[] data)
        {
            string normalizedType = (resourceType ?? string.Empty).Trim().ToUpperInvariant();

            // Group resources use the same first bytes as ICO/CUR files, but their
            // entries contain resource IDs instead of file offsets.
            if (normalizedType == "RT_GROUP_ICON" || normalizedType == "RT_GROUP_CURSOR" || normalizedType == "RT_POINTER")
                return DetectStructuralResourceType(normalizedType);

            ResourceFileFormat signature = DetectSignature(data);
            if (signature != null)
                return signature;

            // Other known RT_* structures keep a descriptive extension when no
            // complete embedded file signature is present.
            ResourceFileFormat structuralType = DetectStructuralResourceType(normalizedType);
            if (structuralType != null)
                return structuralType;

            ResourceFileFormat typed = DetectResourceType(normalizedType);
            if (typed != null)
                return typed;

            if (LooksLikeDib(data, 0))
                return Format(".dib", "Device-independent bitmap", "image/bmp");

            if (LooksLikeReadableText(data))
                return Format(".txt", "Text", "text/plain");

            return Format(".bin", "Unknown binary data", "application/octet-stream");
        }

        private static ResourceFileFormat DetectStructuralResourceType(string type)
        {
            switch (type)
            {
                case "RT_GROUP_ICON":
                    return Format(".grpicon", "Windows group icon resource", "application/octet-stream");
                case "RT_GROUP_CURSOR":
                    return Format(".grpcursor", "Windows group cursor resource", "application/octet-stream");
                case "RT_CURSOR":
                    return Format(".cur", "Windows cursor resource", "image/x-icon");
                case "RT_POINTER":
                    return Format(".ptr", "OS/2 pointer resource", "application/octet-stream");
                case "RT_FONTDIR":
                    return Format(".fontdir", "Font directory resource", "application/octet-stream");
                case "RT_MENU":
                    return Format(".menu", "Menu resource", "application/octet-stream");
                case "RT_DIALOG":
                    return Format(".dlg", "Dialog resource", "application/octet-stream");
                case "RT_STRING":
                    return Format(".str", "String-table resource", "application/octet-stream");
                case "RT_VERSION":
                    return Format(".version", "Version-information resource", "application/octet-stream");
                case "RT_ACCELERATOR":
                case "RT_ACCELTABLE":
                    return Format(".accel", "Accelerator-table resource", "application/octet-stream");
                case "RT_MESSAGE":
                case "RT_MESSAGETABLE":
                    return Format(".msgtable", "Message-table resource", "application/octet-stream");
                case "RT_NAMETABLE":
                    return Format(".nametable", "Name-table resource", "application/octet-stream");
                case "RT_DISPLAYINFO":
                    return Format(".displayinfo", "Display-information resource", "application/octet-stream");
                case "RT_HELPTABLE":
                    return Format(".helptable", "Help-table resource", "application/octet-stream");
                case "RT_HELPSUBTABLE":
                    return Format(".helpsubtable", "Help-subtable resource", "application/octet-stream");
                case "RT_DLGINCLUDE":
                    return Format(".dlginc", "Dialog-include resource", "application/octet-stream");
                case "RT_DLGINIT":
                    return Format(".dlginit", "Dialog-initialization resource", "application/octet-stream");
                case "RT_TOOLBAR":
                    return Format(".toolbar", "Toolbar resource", "application/octet-stream");
            }

            return null;
        }

        private static ResourceFileFormat DetectResourceType(string type)
        {
            switch (type)
            {
                case "RT_BITMAP":
                    return Format(".dib", "Device-independent bitmap resource", "image/bmp");
                case "RT_ICON":
                    return Format(".dib", "Icon image resource", "image/bmp");
                case "RT_FONT":
                    return Format(".fnt", "Bitmap font resource", "application/x-font");
                case "RT_HTML":
                    return Format(".html", "HTML document", "text/html");
                case "RT_MANIFEST":
                    return Format(".xml", "Application manifest", "application/xml");
                case "RT_RCDATA":
                    return Format(".rcdata", "Raw application resource", "application/octet-stream");
                case "RT_ANIICON":
                case "RT_ANICURSOR":
                    return Format(".ani", "Animated icon or cursor", "application/x-navi-animation");
                case "RT_PLUGPLAY":
                    return Format(".pnp", "Plug and Play resource", "application/octet-stream");
                case "RT_VXD":
                    return Format(".vxd", "Virtual device driver resource", "application/octet-stream");
            }

            return null;
        }

        private static ResourceFileFormat DetectSignature(byte[] data)
        {
            if (data == null || data.Length == 0)
                return null;

            if (StartsWith(data, 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A))
                return Format(".png", "PNG image", "image/png");
            if (data.Length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
                return Format(".jpg", "JPEG image", "image/jpeg");
            if (AsciiEquals(data, 0, "GIF87a") || AsciiEquals(data, 0, "GIF89a"))
                return Format(".gif", "GIF image", "image/gif");
            if (AsciiEquals(data, 0, "BM"))
                return Format(".bmp", "Bitmap image", "image/bmp");
            if (AsciiEquals(data, 0, "BA"))
                return Format(".bmp", "OS/2 bitmap array", "image/bmp");
            if (AsciiEquals(data, 0, "IC") || AsciiEquals(data, 0, "CI") ||
                AsciiEquals(data, 0, "CP") || AsciiEquals(data, 0, "PT"))
                return Format(".ptr", "OS/2 icon or pointer bitmap", "application/octet-stream");
            if (StartsWith(data, 0x49, 0x49, 0x2A, 0x00) || StartsWith(data, 0x4D, 0x4D, 0x00, 0x2A))
                return Format(".tif", "TIFF image", "image/tiff");
            if (StartsWith(data, 0x00, 0x00, 0x01, 0x00))
                return Format(".ico", "Windows icon", "image/x-icon");
            if (StartsWith(data, 0x00, 0x00, 0x02, 0x00))
                return Format(".cur", "Windows cursor", "image/x-icon");
            if (AsciiEquals(data, 0, "DDS "))
                return Format(".dds", "DirectDraw surface", "image/vnd-ms.dds");
            if (AsciiEquals(data, 0, "8BPS"))
                return Format(".psd", "Adobe Photoshop image", "image/vnd.adobe.photoshop");
            if (AsciiEquals(data, 0, "qoif"))
                return Format(".qoi", "Quite OK Image", "image/qoi");
            if (StartsWith(data, 0x76, 0x2F, 0x31, 0x01))
                return Format(".exr", "OpenEXR image", "image/x-exr");
            if (StartsWith(data, 0x00, 0x00, 0x00, 0x0C, 0x6A, 0x50, 0x20, 0x20, 0x0D, 0x0A, 0x87, 0x0A))
                return Format(".jp2", "JPEG 2000 image", "image/jp2");
            if (StartsWith(data, 0xD7, 0xCD, 0xC6, 0x9A))
                return Format(".wmf", "Windows metafile", "image/wmf");
            if (data.Length >= 44 && BitConverter.ToUInt32(data, 0) == 1 && AsciiEquals(data, 40, " EMF"))
                return Format(".emf", "Enhanced metafile", "image/emf");
            if (LooksLikePcx(data))
                return Format(".pcx", "PCX image", "image/x-pcx");
            if (LooksLikePortableAnymap(data))
                return Format(".pnm", "Portable anymap image", "image/x-portable-anymap");

            ResourceFileFormat riff = DetectRiff(data);
            if (riff != null)
                return riff;

            ResourceFileFormat iff = DetectIff(data);
            if (iff != null)
                return iff;

            ResourceFileFormat isoBaseMedia = DetectIsoBaseMedia(data);
            if (isoBaseMedia != null)
                return isoBaseMedia;

            if (StartsWith(data, 0x1A, 0x45, 0xDF, 0xA3))
            {
                string prefix = GetAsciiWindow(data, 0, Math.Min(data.Length, 256)).ToLowerInvariant();
                return prefix.Contains("webm")
                    ? Format(".webm", "WebM video", "video/webm")
                    : Format(".mkv", "Matroska container", "video/x-matroska");
            }
            if (StartsWith(data, 0x00, 0x00, 0x01, 0xBA))
                return Format(".mpg", "MPEG program stream", "video/mpeg");
            if (LooksLikeMpegTransportStream(data))
                return Format(".ts", "MPEG transport stream", "video/mp2t");
            if (AsciiEquals(data, 0, "FLV"))
                return Format(".flv", "Flash video", "video/x-flv");
            if (AsciiEquals(data, 0, "FWS") || AsciiEquals(data, 0, "CWS") || AsciiEquals(data, 0, "ZWS"))
                return Format(".swf", "Shockwave Flash", "application/x-shockwave-flash");
            if (StartsWith(data, 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C))
                return Format(".asf", "Advanced Systems Format", "video/x-ms-asf");

            if (AsciiEquals(data, 0, "OggS"))
                return Format(".ogg", "Ogg container", "application/ogg");
            if (AsciiEquals(data, 0, "fLaC"))
                return Format(".flac", "FLAC audio", "audio/flac");
            if (AsciiEquals(data, 0, "MThd"))
                return Format(".mid", "MIDI sequence", "audio/midi");
            if (AsciiEquals(data, 0, ".snd"))
                return Format(".au", "Sun/NeXT audio", "audio/basic");
            if (LooksLikeMp3(data))
                return Format(".mp3", "MPEG audio", "audio/mpeg");

            if (StartsWith(data, 0x50, 0x4B, 0x03, 0x04) || StartsWith(data, 0x50, 0x4B, 0x05, 0x06) || StartsWith(data, 0x50, 0x4B, 0x07, 0x08))
                return Format(".zip", "ZIP archive", "application/zip");
            if (StartsWith(data, 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07))
                return Format(".rar", "RAR archive", "application/vnd.rar");
            if (StartsWith(data, 0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C))
                return Format(".7z", "7-Zip archive", "application/x-7z-compressed");
            if (StartsWith(data, 0x1F, 0x8B))
                return Format(".gz", "GZip archive", "application/gzip");
            if (AsciiEquals(data, 0, "BZh"))
                return Format(".bz2", "BZip2 archive", "application/x-bzip2");
            if (StartsWith(data, 0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00))
                return Format(".xz", "XZ archive", "application/x-xz");
            if (AsciiEquals(data, 0, "MSCF"))
                return Format(".cab", "Microsoft Cabinet archive", "application/vnd.ms-cab-compressed");
            if (data.Length > 262 && AsciiEquals(data, 257, "ustar"))
                return Format(".tar", "TAR archive", "application/x-tar");

            if (AsciiEquals(data, 0, "%PDF-"))
                return Format(".pdf", "PDF document", "application/pdf");
            if (StartsWith(data, 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1))
                return Format(".ole", "OLE compound document", "application/x-ole-storage");
            if (AsciiEquals(data, 0, "ITSF"))
                return Format(".chm", "Compiled HTML Help", "application/vnd.ms-htmlhelp");
            if (AsciiEquals(data, 0, "SQLite format 3\0"))
                return Format(".sqlite", "SQLite database", "application/vnd.sqlite3");
            if (AsciiEquals(data, 0, "regf"))
                return Format(".hiv", "Windows registry hive", "application/octet-stream");
            if (StartsWith(data, 0x4C, 0x00, 0x00, 0x00, 0x01, 0x14, 0x02, 0x00))
                return Format(".lnk", "Windows shortcut", "application/x-ms-shortcut");
            if (AsciiEquals(data, 0, "Microsoft C/C++ MSF 7.00"))
                return Format(".pdb", "Program database", "application/octet-stream");

            if (AsciiEquals(data, 0, "OTTO"))
                return Format(".otf", "OpenType font", "font/otf");
            if (StartsWith(data, 0x00, 0x01, 0x00, 0x00) || AsciiEquals(data, 0, "true"))
                return Format(".ttf", "TrueType font", "font/ttf");
            if (AsciiEquals(data, 0, "ttcf"))
                return Format(".ttc", "TrueType collection", "font/collection");
            if (AsciiEquals(data, 0, "wOFF"))
                return Format(".woff", "Web Open Font Format", "font/woff");
            if (AsciiEquals(data, 0, "wOF2"))
                return Format(".woff2", "Web Open Font Format 2", "font/woff2");

            if (StartsWith(data, 0x4D, 0x5A))
                return Format(".exe", "DOS/Windows executable", "application/vnd.microsoft.portable-executable");
            if (StartsWith(data, 0x7F, 0x45, 0x4C, 0x46))
                return Format(".elf", "ELF executable", "application/x-elf");
            if (StartsWith(data, 0xCA, 0xFE, 0xBA, 0xBE))
                return Format(".class", "Java class", "application/java-vm");
            if (AsciiEquals(data, 0, "dex\n"))
                return Format(".dex", "Android Dalvik executable", "application/vnd.android.dex");
            if (StartsWith(data, 0x00, 0x61, 0x73, 0x6D))
                return Format(".wasm", "WebAssembly module", "application/wasm");
            if (StartsWith(data, 0x1B, 0x4C, 0x75, 0x61))
                return Format(".luac", "Lua bytecode", "application/octet-stream");

            string text = GetTextPrefix(data, 2048);
            if (text != null)
            {
                string trimmed = text.TrimStart('\uFEFF', ' ', '\t', '\r', '\n');
                if (trimmed.StartsWith("<!DOCTYPE html", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.StartsWith("<html", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.StartsWith("<head", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.StartsWith("<body", StringComparison.OrdinalIgnoreCase))
                    return Format(".html", "HTML document", "text/html");
                if (trimmed.StartsWith("<svg", StringComparison.OrdinalIgnoreCase))
                    return Format(".svg", "SVG image", "image/svg+xml");
                if (trimmed.StartsWith("<?xml", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.StartsWith("<manifest", StringComparison.OrdinalIgnoreCase) ||
                    trimmed.StartsWith("<assembly", StringComparison.OrdinalIgnoreCase))
                    return Format(".xml", "XML document", "application/xml");
                if (trimmed.StartsWith("{\\rtf", StringComparison.OrdinalIgnoreCase))
                    return Format(".rtf", "Rich Text Format document", "application/rtf");
                if (LooksLikeJson(trimmed))
                    return Format(".json", "JSON document", "application/json");
            }

            if (LooksLikeDib(data, 0))
                return Format(".dib", "Device-independent bitmap", "image/bmp");

            return null;
        }

        private static ResourceFileFormat DetectRiff(byte[] data)
        {
            if (data.Length < 12 || (!AsciiEquals(data, 0, "RIFF") && !AsciiEquals(data, 0, "RIFX")))
                return null;

            string form = GetAsciiWindow(data, 8, 4);
            switch (form)
            {
                case "AVI ":
                    return Format(".avi", "AVI video", "video/x-msvideo");
                case "WAVE":
                    return Format(".wav", "WAVE audio", "audio/wav");
                case "WEBP":
                    return Format(".webp", "WebP image", "image/webp");
                case "ACON":
                    return Format(".ani", "Animated cursor", "application/x-navi-animation");
                case "RMID":
                    return Format(".rmi", "RIFF MIDI", "audio/midi");
                default:
                    return Format(".riff", "RIFF container", "application/riff");
            }
        }

        private static ResourceFileFormat DetectIff(byte[] data)
        {
            if (data.Length < 12 || !AsciiEquals(data, 0, "FORM"))
                return null;

            string form = GetAsciiWindow(data, 8, 4);
            switch (form)
            {
                case "AIFF":
                case "AIFC":
                    return Format(".aiff", "AIFF audio", "audio/aiff");
                case "ILBM":
                case "PBM ":
                case "ACBM":
                    return Format(".iff", "IFF bitmap image", "image/x-iff");
                case "8SVX":
                    return Format(".8svx", "8SVX audio", "audio/x-8svx");
                case "ANIM":
                    return Format(".anim", "IFF animation", "video/x-anim");
                default:
                    return Format(".iff", "IFF container", "application/x-iff");
            }
        }

        private static ResourceFileFormat DetectIsoBaseMedia(byte[] data)
        {
            if (data.Length < 12 || !AsciiEquals(data, 4, "ftyp"))
                return null;

            string brand = GetAsciiWindow(data, 8, 4);
            if (brand == "qt  ")
                return Format(".mov", "QuickTime movie", "video/quicktime");
            if (brand == "M4A " || brand == "M4B ")
                return Format(brand == "M4A " ? ".m4a" : ".m4b", "MPEG-4 audio", "audio/mp4");
            if (brand == "avif" || brand == "avis")
                return Format(".avif", "AVIF image", "image/avif");
            if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "hevx" || brand == "mif1" || brand == "msf1")
                return Format(".heic", "HEIF image", "image/heic");

            return Format(".mp4", "MPEG-4 container", "video/mp4");
        }

        private static bool LooksLikeDib(byte[] data, int offset)
        {
            if (data == null || offset < 0 || data.Length < offset + 12)
                return false;

            uint size = BitConverter.ToUInt32(data, offset);
            if (size == 12)
            {
                ushort width = BitConverter.ToUInt16(data, offset + 4);
                ushort height = BitConverter.ToUInt16(data, offset + 6);
                ushort planes = BitConverter.ToUInt16(data, offset + 8);
                ushort bits = BitConverter.ToUInt16(data, offset + 10);
                return width > 0 && height > 0 && planes == 1 && IsBitmapBitCount(bits);
            }

            if ((size == 16 || size == 40 || size == 52 || size == 56 || size == 64 || size == 108 || size == 124) && data.Length >= offset + 16)
            {
                int width = BitConverter.ToInt32(data, offset + 4);
                int height = BitConverter.ToInt32(data, offset + 8);
                ushort planes = BitConverter.ToUInt16(data, offset + 12);
                ushort bits = BitConverter.ToUInt16(data, offset + 14);
                return width > 0 && height != 0 && planes == 1 && IsBitmapBitCount(bits);
            }

            return false;
        }

        private static bool IsBitmapBitCount(ushort bits)
        {
            return bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 16 || bits == 24 || bits == 32;
        }

        private static bool LooksLikePcx(byte[] data)
        {
            if (data.Length < 128 || data[0] != 0x0A)
                return false;
            byte encoding = data[2];
            byte bitsPerPixel = data[3];
            return encoding == 1 && (bitsPerPixel == 1 || bitsPerPixel == 2 || bitsPerPixel == 4 || bitsPerPixel == 8);
        }

        private static bool LooksLikePortableAnymap(byte[] data)
        {
            return data.Length >= 3 && data[0] == (byte)'P' && data[1] >= (byte)'1' && data[1] <= (byte)'7' &&
                   (data[2] == (byte)' ' || data[2] == (byte)'\t' || data[2] == (byte)'\r' || data[2] == (byte)'\n');
        }

        private static bool LooksLikeMp3(byte[] data)
        {
            if (AsciiEquals(data, 0, "ID3"))
                return true;
            return data.Length >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0 && (data[1] & 0x18) != 0x08;
        }

        private static bool LooksLikeMpegTransportStream(byte[] data)
        {
            if (data.Length < 376 || data[0] != 0x47)
                return false;
            return data[188] == 0x47 || (data.Length > 376 && data[376] == 0x47);
        }

        private static bool LooksLikeJson(string text)
        {
            if (string.IsNullOrEmpty(text))
                return false;
            char first = text[0];
            if (first != '{' && first != '[')
                return false;
            int closing = first == '{' ? text.LastIndexOf('}') : text.LastIndexOf(']');
            return closing > 0;
        }

        private static bool LooksLikeReadableText(byte[] data)
        {
            string text = GetTextPrefix(data, 4096);
            if (string.IsNullOrEmpty(text))
                return false;

            int sampleLength = Math.Min(text.Length, 2048);
            int readable = 0;
            for (int i = 0; i < sampleLength; i++)
            {
                char c = text[i];
                if (c == '\r' || c == '\n' || c == '\t' || !char.IsControl(c))
                    readable++;
            }
            return sampleLength > 0 && readable >= sampleLength * 9 / 10;
        }

        private static string GetTextPrefix(byte[] data, int maximumBytes)
        {
            if (data == null || data.Length == 0)
                return null;

            int length = Math.Min(data.Length, maximumBytes);
            try
            {
                if (length >= 3 && StartsWith(data, 0xEF, 0xBB, 0xBF))
                    return Encoding.UTF8.GetString(data, 3, length - 3);
                if (length >= 2 && data[0] == 0xFF && data[1] == 0xFE)
                    return Encoding.Unicode.GetString(data, 2, length - 2);
                if (length >= 2 && data[0] == 0xFE && data[1] == 0xFF)
                    return Encoding.BigEndianUnicode.GetString(data, 2, length - 2);

                int oddZeroes = 0;
                int evenZeroes = 0;
                int pairs = Math.Min(length / 2, 128);
                for (int i = 0; i < pairs; i++)
                {
                    if (data[i * 2] == 0) evenZeroes++;
                    if (data[i * 2 + 1] == 0) oddZeroes++;
                }
                if (pairs >= 4 && oddZeroes > pairs / 2 && evenZeroes < pairs / 5)
                    return Encoding.Unicode.GetString(data, 0, length - (length % 2));
                if (pairs >= 4 && evenZeroes > pairs / 2 && oddZeroes < pairs / 5)
                    return Encoding.BigEndianUnicode.GetString(data, 0, length - (length % 2));

                return new UTF8Encoding(false, true).GetString(data, 0, length);
            }
            catch
            {
                try
                {
                    return Encoding.Default.GetString(data, 0, length);
                }
                catch
                {
                    return null;
                }
            }
        }

        private static string GetAsciiWindow(byte[] data, int offset, int count)
        {
            if (data == null || offset < 0 || count < 0 || offset + count > data.Length)
                return string.Empty;
            return Encoding.ASCII.GetString(data, offset, count);
        }

        private static bool AsciiEquals(byte[] data, int offset, string value)
        {
            if (data == null || value == null || offset < 0 || offset + value.Length > data.Length)
                return false;
            for (int i = 0; i < value.Length; i++)
            {
                if (data[offset + i] != (byte)value[i])
                    return false;
            }
            return true;
        }

        private static bool StartsWith(byte[] data, params byte[] signature)
        {
            if (data == null || signature == null || data.Length < signature.Length)
                return false;
            for (int i = 0; i < signature.Length; i++)
            {
                if (data[i] != signature[i])
                    return false;
            }
            return true;
        }

        private static ResourceFileFormat Format(string extension, string description, string mimeType)
        {
            return new ResourceFileFormat(extension, description, mimeType);
        }
    }

    internal sealed class ConvertedResourceFile
    {
        public string FileName { get; set; }
        public byte[] Data { get; set; }
    }

    internal static class ResourceConversion
    {
        public static List<string> GetAvailableExtensions(object decodedResource)
        {
            List<string> result = new List<string>();
            if (GetBitmaps(decodedResource).Count > 0)
            {
                result.Add(".png");
                result.Add(".bmp");
            }
            else if (decodedResource is string)
            {
                result.Add(".txt");
            }
            return result;
        }

        public static List<ConvertedResourceFile> Convert(object decodedResource, string extension, string baseName)
        {
            List<ConvertedResourceFile> result = new List<ConvertedResourceFile>();
            extension = NormalizeExtension(extension);
            baseName = SanitizeFileName(baseName);

            DecodedFont decodedFont = decodedResource as DecodedFont;
            if (decodedFont != null && (extension == ".png" || extension == ".bmp"))
            {
                ImageFormat glyphFormat = extension == ".png" ? ImageFormat.Png : ImageFormat.Bmp;
                for (int i = 0; i < decodedFont.Glyphs.Count; i++)
                {
                    FontGlyph glyph = decodedFont.Glyphs[i];
                    if (glyph == null || glyph.Bitmap == null)
                        continue;

                    using (MemoryStream stream = new MemoryStream())
                    {
                        glyph.Bitmap.Save(stream, glyphFormat);
                        result.Add(new ConvertedResourceFile
                        {
                            FileName = baseName + "_char_" + glyph.CharacterCode.ToString("X4") + extension,
                            Data = stream.ToArray()
                        });
                    }
                }
                return result;
            }

            List<Bitmap> bitmaps = GetBitmaps(decodedResource);
            if (bitmaps.Count > 0 && (extension == ".png" || extension == ".bmp"))
            {
                ImageFormat format = extension == ".png" ? ImageFormat.Png : ImageFormat.Bmp;
                for (int i = 0; i < bitmaps.Count; i++)
                {
                    Bitmap bitmap = bitmaps[i];
                    if (bitmap == null)
                        continue;

                    using (MemoryStream stream = new MemoryStream())
                    {
                        bitmap.Save(stream, format);
                        string suffix = bitmaps.Count > 1 ? "_" + (i + 1).ToString("D3") : string.Empty;
                        result.Add(new ConvertedResourceFile
                        {
                            FileName = baseName + suffix + extension,
                            Data = stream.ToArray()
                        });
                    }
                }
                return result;
            }

            string text = decodedResource as string;
            if (text != null && extension == ".txt")
            {
                result.Add(new ConvertedResourceFile
                {
                    FileName = baseName + extension,
                    Data = new UTF8Encoding(true).GetBytes(text)
                });
            }

            return result;
        }

        private static List<Bitmap> GetBitmaps(object decodedResource)
        {
            List<Bitmap> result = new List<Bitmap>();

            DecodedFont decodedFont = decodedResource as DecodedFont;
            if (decodedFont != null)
            {
                for (int i = 0; i < decodedFont.Glyphs.Count; i++)
                {
                    FontGlyph glyph = decodedFont.Glyphs[i];
                    if (glyph != null && glyph.Bitmap != null)
                        result.Add(glyph.Bitmap);
                }
                return result;
            }

            Bitmap bitmap = decodedResource as Bitmap;
            if (bitmap != null)
            {
                result.Add(bitmap);
                return result;
            }

            Img image = decodedResource as Img;
            if (image != null && image.Bitmap != null)
            {
                result.Add(image.Bitmap);
                return result;
            }

            IEnumerable<Img> images = decodedResource as IEnumerable<Img>;
            if (images != null)
            {
                foreach (Img item in images)
                {
                    if (item != null && item.Bitmap != null)
                        result.Add(item.Bitmap);
                }
            }

            return result;
        }

        public static string NormalizeExtension(string extension)
        {
            if (string.IsNullOrEmpty(extension))
                return ".bin";
            return extension.StartsWith(".") ? extension.ToLowerInvariant() : "." + extension.ToLowerInvariant();
        }

        public static string SanitizeFileName(string value)
        {
            string result = string.IsNullOrWhiteSpace(value) ? "resource" : value.Trim();
            foreach (char invalid in Path.GetInvalidFileNameChars())
                result = result.Replace(invalid, '_');
            result = result.Trim(' ', '.');
            return string.IsNullOrEmpty(result) ? "resource" : result;
        }

        public static string GetUniquePath(string path)
        {
            if (!File.Exists(path))
                return path;

            string directory = Path.GetDirectoryName(path);
            string name = Path.GetFileNameWithoutExtension(path);
            string extension = Path.GetExtension(path);
            int index = 2;
            string candidate;
            do
            {
                candidate = Path.Combine(directory, name + "_" + index.ToString() + extension);
                index++;
            }
            while (File.Exists(candidate));
            return candidate;
        }
    }
}
