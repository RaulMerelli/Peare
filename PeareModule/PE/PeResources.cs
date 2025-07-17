using System;
using System.Collections.Generic;
using System.Drawing;
using System.Linq;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Text;
using System.Threading.Tasks;

namespace PeareModule
{
    public static class PeResources
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
        private static extern bool EnumResourceNames(IntPtr hModule, IntPtr lpszType, EnumResNameProc lpEnumFunc, IntPtr lParam);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool EnumResourceNames([In()] IntPtr hModule, [In] string lpszType, EnumResNameProc lpEnumFunc, IntPtr lParam);

        private delegate bool EnumResTypeProc(IntPtr hModule, IntPtr lpszType, IntPtr lParam);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool EnumResourceTypes(IntPtr hModule, EnumResTypeProc lpEnumFunc, IntPtr lParam);

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        public static extern IntPtr LoadBitmap(IntPtr hInstance, string lpBitmapName);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr LoadLibraryEx(string dllToLoad, IntPtr hFile, LoadLibraryFlags flags);

        [DllImport("gdi32.dll", CharSet = CharSet.Auto, SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool DeleteObject(IntPtr hObject);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr FindResource(IntPtr hModule, string lpName, string lpType);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr FindResource(IntPtr hModule, string lpName, IntPtr lpType);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr FindResource(IntPtr hModule, IntPtr lpName, string lpType);

        [DllImport("kernel32.dll", SetLastError = true)]
        public static extern IntPtr FindResource(IntPtr hModule, IntPtr lpName, IntPtr lpType);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr LoadResource(IntPtr hModule, IntPtr hResInfo);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr LockResource(IntPtr hResData);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern bool FreeLibrary(IntPtr hModule);

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint SizeofResource(IntPtr hModule, IntPtr hResInfo);

        private delegate bool EnumResNameProc(IntPtr hModule, IntPtr lpszType, IntPtr lpszName, IntPtr lParam);

        // Add this helper function to get error messages
        static string GetErrorMessage(int errorCode)
        {
            var buffer = new StringBuilder(256);
            FormatMessage(
                FormatMessageFlags.FORMAT_MESSAGE_FROM_SYSTEM |
                FormatMessageFlags.FORMAT_MESSAGE_IGNORE_INSERTS,
                IntPtr.Zero,
                errorCode,
                0,
                buffer,
                buffer.Capacity,
                IntPtr.Zero);
            return buffer.ToString().Trim();
        }

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern int FormatMessage(
            FormatMessageFlags dwFlags,
            IntPtr lpSource,
            int dwMessageId,
            int dwLanguageId,
            StringBuilder lpBuffer,
            int nSize,
            IntPtr Arguments);

        public static Dictionary<int, string> PeResourceTypes = new Dictionary<int, string>
        {
            { 0x01, "RT_CURSOR" },
            { 0x02, "RT_BITMAP" },
            { 0x03, "RT_ICON" },
            { 0x04, "RT_MENU" },
            { 0x05, "RT_DIALOG" },
            { 0x06, "RT_STRING" },
            { 0x07, "RT_FONTDIR" },
            { 0x08, "RT_FONT" },
            { 0x09, "RT_ACCELERATOR" },
            { 0x0A, "RT_RCDATA" },
            { 0x0B, "RT_MESSAGETABLE" },
            { 0x0C, "RT_GROUP_CURSOR" },
            { 0x0D, "RT_UNKNOWN(13)" },
            { 0x0E, "RT_GROUP_ICON" },
            { 0x0F, "RT_UNKNOWN(15)" },
            { 0x10, "RT_VERSION" },
            { 0x11, "RT_DLGINCLUDE" },
            { 0x12, "RT_UNKNOWN(18)" },
            { 0x13, "RT_PLUGPLAY" },
            { 0x14, "RT_VXD" },
            { 0x15, "RT_ANICURSOR" },
            { 0x16, "RT_ANIICON" },
            { 0x17, "RT_HTML" },
            { 0x18, "RT_MANIFEST" }
        };

        public static List<string[]> OpenPE(string filePath)
        {
            List<string[]> relations = new List<string[]>();

            IntPtr hModule = LoadLibraryEx(filePath, IntPtr.Zero, LoadLibraryFlags.LOAD_LIBRARY_AS_DATAFILE);

            EnumResourceTypes(hModule, (h, typePtr, lParam) =>
            {
                bool isInt = IsIntResource(typePtr);
                string typeName = isInt
                    ? PeResourceTypes.TryGetValue(typePtr.ToInt32(), out var name) ? name : $"#{typePtr.ToInt32()}"
                    : Marshal.PtrToStringAnsi(typePtr);

                relations.Add(new string[] { "Root", typeName });

                EnumResNameProc nameCallback = (h2, t, namePtr, lparam2) =>
                {
                    string nameStr = IsIntResource(namePtr)
                        ? $"#{namePtr.ToInt32()}"
                        : Marshal.PtrToStringAuto(namePtr);
                    relations.Add(new string[] { typeName, nameStr });
                    return true;
                };

                if (isInt)
                    EnumResourceNames(hModule, typePtr, nameCallback, IntPtr.Zero);
                else
                    EnumResourceNames(hModule, typeName, nameCallback, IntPtr.Zero);

                return true;
            }, IntPtr.Zero);

            return relations;
        }

        public static bool IsIntResource(IntPtr ptr)
        {
            // if the value is <= 0xFFFF it is int, else it points to a string
            return ((ulong)ptr.ToInt64() & 0xFFFF0000) == 0;
        }

        public static byte[] OpenResourcePE(ModuleResources.ModuleProperties properties, string lpType, string lpName, out string message, out bool found)
        {
            found = false;
            message = "";

            IntPtr hModule = LoadLibraryEx(properties.filePath, IntPtr.Zero, LoadLibraryFlags.LOAD_LIBRARY_AS_DATAFILE);
            if (hModule == IntPtr.Zero)
            {
                Console.WriteLine(GetErrorMessage(Marshal.GetLastWin32Error()));
                return new byte[0];
            }

            // Find and load the resource
            IntPtr hResource = IntPtr.Zero;

            var numericType = PeResourceTypes.Where(x => x.Value == lpType).Select(x => (int?)x.Key).FirstOrDefault();
            int numericName = IsDigitsOnly(lpName) ? int.Parse(lpName) : -1;

            // FindResource has 4 different signatures
            if (numericType.HasValue)
            {
                if (numericName == -1)
                {
                    hResource = FindResource(hModule, lpName, new IntPtr(numericType.Value));
                }
                else
                {
                    hResource = FindResource(hModule, new IntPtr(numericName), new IntPtr(numericType.Value));
                }
            }
            else
            {
                if (numericName == -1)
                {
                    hResource = FindResource(hModule, lpName, lpType);
                }
                else
                {
                    hResource = FindResource(hModule, new IntPtr(numericName), lpType);
                }
            }
            if (hResource != IntPtr.Zero) 
                found = true;

            if (!found)
            {
                message = $"Windows PE Resource {lpType} {lpName} not found.";
                return new byte[0];
            }

            IntPtr hResourceData = LoadResource(hModule, hResource);

            // Access the data
            IntPtr pRes = LockResource(hResourceData);
            uint size = SizeofResource(hModule, hResource);
            byte[] bytes = new byte[size];

            message = $"Windows PE Resource {lpType} {lpName} found.\nLength: {bytes.Length} byte.";
            if (pRes != IntPtr.Zero)
            {
                for (int i = 0; i < bytes.Length; i++)
                {
                    bytes[i] = Marshal.ReadByte(pRes, i);
                }
            }
            FreeLibrary(hModule);
            return bytes;
        }

        private static bool IsDigitsOnly(string str)
        {
            foreach (char c in str)
            {
                if (c < '0' || c > '9')
                    return false;
            }

            return true;
        }
    }
}
