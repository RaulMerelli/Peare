using System;
using System.Linq;
using System.Text;

namespace PeareModule
{
    public static class OS2_RT_FONTDIR
    {
        public static string Get(byte[] resData)
        {
            StringBuilder rcString = new StringBuilder();

            int resType = BitConverter.ToInt16(resData, 0);
            int numEntries = BitConverter.ToInt16(resData, 2);
            int blockSize = BitConverter.ToInt16(resData, 4); // Always 182

            rcString.AppendLine("RT_FONTDIR");
            rcString.AppendLine("{");
            for (int i = 0; i < numEntries; i++)
            {
                int offset = 6 + (i * blockSize);
                byte[] blockData = resData.Skip(offset).Take(blockSize).ToArray();
                int localOffset = 0;
                Console.WriteLine("block " + i);
                ModuleResources.DumpRaw(blockData); // this is printing the dump output
                // First two bytes are the resource id
                int resourceId = ReadInt16(blockData, ref localOffset);                         // 00->02
                // From offset 2 the structure is FOCAMETRICS
                int ulIdentity = ReadInt32(blockData, ref localOffset);                         // 02->06
                int ulSize = ReadInt32(blockData, ref localOffset);                             // 06->10
                string szFamilyname = RT_STRING.ReadLenString(blockData, ref localOffset, 850, 32).Trim(); // 10->42
                string szFacename = RT_STRING.ReadLenString(blockData, ref localOffset, 850, 32).Trim();   // 42->74
                short usRegistryId = ReadInt16(blockData, ref localOffset);                     // 74->76
                ushort usCodePage = (ushort)ReadInt16(blockData, ref localOffset);              // 76->78 (USHORT is 2 bytes, cast from short)
                short yEmHeight = ReadInt16(blockData, ref localOffset);                        // 78->80
                short yXHeight = ReadInt16(blockData, ref localOffset);                         // 80->82
                short yMaxAscender = ReadInt16(blockData, ref localOffset);                     // 82->84
                short yMaxDescender = ReadInt16(blockData, ref localOffset);                    // 84->86
                short yLowerCaseAscent = ReadInt16(blockData, ref localOffset);                 // 86->88
                short yLowerCaseDescent = ReadInt16(blockData, ref localOffset);                // 88->90
                short yInternalLeading = ReadInt16(blockData, ref localOffset);                 // 90->92
                short yExternalLeading = ReadInt16(blockData, ref localOffset);                 // 92->94
                short xAveCharWidth = ReadInt16(blockData, ref localOffset);                    // 94->96
                short xMaxCharInc = ReadInt16(blockData, ref localOffset);                      // 96->98
                short xEmInc = ReadInt16(blockData, ref localOffset);                           // 98->100
                short yMaxBaselineExt = ReadInt16(blockData, ref localOffset);                  // 100->102
                short sCharSlope = ReadInt16(blockData, ref localOffset);                       // 102->104
                short sInlineDir = ReadInt16(blockData, ref localOffset);                       // 104->106
                short sCharRot = ReadInt16(blockData, ref localOffset);                         // 106->108
                ushort usWeightClass = (ushort)ReadInt16(blockData, ref localOffset);           // 108->110
                ushort usWidthClass = (ushort)ReadInt16(blockData, ref localOffset);            // 110->112
                short xDeviceRes = ReadInt16(blockData, ref localOffset);                       // 112->114
                short yDeviceRes = ReadInt16(blockData, ref localOffset);                       // 114->116
                short usFirstChar = ReadInt16(blockData, ref localOffset);                      // 116->118
                short usLastChar = ReadInt16(blockData, ref localOffset);                       // 118->120
                short usDefaultChar = ReadInt16(blockData, ref localOffset);                    // 120->122
                short usBreakChar = ReadInt16(blockData, ref localOffset);                      // 122->124
                short usNominalPointSize = ReadInt16(blockData, ref localOffset);               // 124->126
                short usMinimumPointSize = ReadInt16(blockData, ref localOffset);               // 126->128
                short usMaximumPointSize = ReadInt16(blockData, ref localOffset);               // 128->130
                short usTypeFlags = ReadInt16(blockData, ref localOffset);                      // 130->132
                short fsDefn = ReadInt16(blockData, ref localOffset);                           // 132->134
                short fsSelectionFlags = ReadInt16(blockData, ref localOffset);                 // 134->136
                short fsCapabilities = ReadInt16(blockData, ref localOffset);                   // 136->138
                short ySubscriptXSize = ReadInt16(blockData, ref localOffset);                  // 138->140
                short ySubscriptYSize = ReadInt16(blockData, ref localOffset);                  // 140->142
                short ySubscriptXOffset = ReadInt16(blockData, ref localOffset);                // 142->144
                short ySubscriptYOffset = ReadInt16(blockData, ref localOffset);                // 144->146
                short ySuperscriptXSize = ReadInt16(blockData, ref localOffset);                // 146->148
                short ySuperscriptYSize = ReadInt16(blockData, ref localOffset);                // 148->150
                short ySuperscriptXOffset = ReadInt16(blockData, ref localOffset);              // 150->152
                short ySuperscriptYOffset = ReadInt16(blockData, ref localOffset);              // 152->154
                short yUnderscoreSize = ReadInt16(blockData, ref localOffset);                  // 154->156
                short yUnderscorePosition = ReadInt16(blockData, ref localOffset);              // 156->158
                short yStrikeoutSize = ReadInt16(blockData, ref localOffset);                   // 158->160
                short yStrikeoutPosition = ReadInt16(blockData, ref localOffset);               // 160->162
                short usKerningPairs = ReadInt16(blockData, ref localOffset);                   // 162->164
                short sFamilyClass = ReadInt16(blockData, ref localOffset);                     // 164->166

                rcString.AppendLine("    RT FONT #" + resourceId);
                rcString.AppendLine("    {");
                rcString.AppendLine($"        ulIdentity = {ulIdentity}");
                rcString.AppendLine($"        ulSize = {ulSize}");
                rcString.AppendLine($"        szFamilyname = \"{szFamilyname}\"");
                rcString.AppendLine($"        szFacename = \"{szFacename}\"");
                rcString.AppendLine($"        usRegistryId = {usRegistryId}");
                rcString.AppendLine($"        usCodePage = {usCodePage}");
                rcString.AppendLine($"        yEmHeight = {yEmHeight}");
                rcString.AppendLine($"        yXHeight = {yXHeight}");
                rcString.AppendLine($"        yMaxAscender = {yMaxAscender}");
                rcString.AppendLine($"        yMaxDescender = {yMaxDescender}");
                rcString.AppendLine($"        yLowerCaseAscent = {yLowerCaseAscent}");
                rcString.AppendLine($"        yLowerCaseDescent = {yLowerCaseDescent}");
                rcString.AppendLine($"        yInternalLeading = {yInternalLeading}");
                rcString.AppendLine($"        yExternalLeading = {yExternalLeading}");
                rcString.AppendLine($"        xAveCharWidth = {xAveCharWidth}");
                rcString.AppendLine($"        xMaxCharInc = {xMaxCharInc}");
                rcString.AppendLine($"        xEmInc = {xEmInc}");
                rcString.AppendLine($"        yMaxBaselineExt = {yMaxBaselineExt}");
                rcString.AppendLine($"        sCharSlope = {sCharSlope}");
                rcString.AppendLine($"        sInlineDir = {sInlineDir}");
                rcString.AppendLine($"        sCharRot = {sCharRot}");
                rcString.AppendLine($"        usWeightClass = {usWeightClass}");
                rcString.AppendLine($"        usWidthClass = {usWidthClass}");
                rcString.AppendLine($"        xDeviceRes = {xDeviceRes}");
                rcString.AppendLine($"        yDeviceRes = {yDeviceRes}");
                rcString.AppendLine($"        usFirstChar = {usFirstChar}");
                rcString.AppendLine($"        usLastChar = {usLastChar}");
                rcString.AppendLine($"        usDefaultChar = {usDefaultChar}");
                rcString.AppendLine($"        usBreakChar = {usBreakChar}");
                rcString.AppendLine($"        usNominalPointSize = {usNominalPointSize}");
                rcString.AppendLine($"        usMinimumPointSize = {usMinimumPointSize}");
                rcString.AppendLine($"        usMaximumPointSize = {usMaximumPointSize}");
                rcString.AppendLine($"        usTypeFlags = {usTypeFlags}");
                rcString.AppendLine($"        fsDefn = {fsDefn}");
                rcString.AppendLine($"        fsSelectionFlags = {fsSelectionFlags}");
                rcString.AppendLine($"        fsCapabilities = {fsCapabilities}");
                rcString.AppendLine($"        ySubscriptXSize = {ySubscriptXSize}");
                rcString.AppendLine($"        ySubscriptYSize = {ySubscriptYSize}");
                rcString.AppendLine($"        ySubscriptXOffset = {ySubscriptXOffset}");
                rcString.AppendLine($"        ySubscriptYOffset = {ySubscriptYOffset}");
                rcString.AppendLine($"        ySuperscriptXSize = {ySuperscriptXSize}");
                rcString.AppendLine($"        ySuperscriptYSize = {ySuperscriptYSize}");
                rcString.AppendLine($"        ySuperscriptXOffset = {ySuperscriptXOffset}");
                rcString.AppendLine($"        ySuperscriptYOffset = {ySuperscriptYOffset}");
                rcString.AppendLine($"        yUnderscoreSize = {yUnderscoreSize}");
                rcString.AppendLine($"        yUnderscorePosition = {yUnderscorePosition}");
                rcString.AppendLine($"        yStrikeoutSize = {yStrikeoutSize}");
                rcString.AppendLine($"        yStrikeoutPosition = {yStrikeoutPosition}");
                rcString.AppendLine($"        usKerningPairs = {usKerningPairs}");
                rcString.AppendLine($"        sFamilyClass = {sFamilyClass}");
                rcString.AppendLine("    }");
            }
            rcString.AppendLine("}");
            return rcString.ToString();
        }

        public static short ReadInt16(byte[] data, ref int offset)
        {
            // Little-endian: Least significant byte first
            short value = (short)(data[offset] | (data[offset + 1] << 8));
            offset += 2;
            return value;
        }

        public static int ReadInt32(byte[] data, ref int offset)
        {
            // Little-endian: Least significant byte first
            int value = (data[offset] |
                         (data[offset + 1] << 8) |
                         (data[offset + 2] << 16) |
                         (data[offset + 3] << 24));
            offset += 4;
            return value;
        }
    }
}