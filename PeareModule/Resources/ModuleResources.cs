﻿using System;
using System.Collections.Generic;
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
            ModuleProperties result = new ModuleProperties();
            result.filePath = path;
            result.versionType = VersionType.Unknown;
            result.headerType = HeaderType.Error;
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
                                version = " for unkwown OS";
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

    }
}
