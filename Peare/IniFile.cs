using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace Peare
{
    // Thanks Danny Beckett from Stack Overflow for this management class. I expanded it with my needs.
    // https://stackoverflow.com/questions/217902/reading-writing-an-ini-file
    class IniFile   // revision 11
    {
        string Path;
        string EXE = Assembly.GetExecutingAssembly().GetName().Name;

        [DllImport("kernel32", CharSet = CharSet.Unicode)]
        static extern long WritePrivateProfileString(string Section, string Key, string Value, string FilePath);

        [DllImport("kernel32", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string Section, string Key, string Default, StringBuilder RetVal, int Size, string FilePath);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        static extern int GetPrivateProfileString(string lpAppName, string lpKeyName, string lpDefault, [Out] char[] lpReturnedString, int nSize, string lpFileName);

        public IniFile(string IniPath = null)
        {
            Path = new FileInfo(IniPath ?? EXE + ".ini").FullName;
        }

        public string Read(string Key, string Section = null)
        {
            var RetVal = new StringBuilder(255);
            GetPrivateProfileString(Section ?? EXE, Key, "", RetVal, 255, Path);
            return RetVal.ToString();
        }

        public void Write(string Key, string Value, string Section = null)
        {
            WritePrivateProfileString(Section ?? EXE, Key, Value, Path);
        }

        public void WriteIfKeyNotExists(string Key, string Value, string Section = null)
        {
            if (!KeyExists(Key, Section))
            {
                Write(Key, Value, Section);
            }
        }

        public void DeleteKey(string Key, string Section = null)
        {
            Write(Key, null, Section ?? EXE);
        }

        public void DeleteSection(string Section = null)
        {
            Write(null, null, Section ?? EXE);
        }

        public bool KeyExists(string Key, string Section = null)
        {
            return Read(Key, Section).Length > 0;
        }

        public List<string> GetSections()
        {
            const int bufferSize = 2048;
            var buffer = new char[bufferSize];

            int charsRead = GetPrivateProfileString(null, null, null, buffer, bufferSize, Path);

            var result = new string(buffer, 0, charsRead);
            return result.Split('\0').Select(x => x.Trim()).ToList();
        }


    }
}
