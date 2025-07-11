using System;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace Peare
{
    public static class Program
    {
        public static string currentFilePath;
        public static string currentHeaderType;
        public static bool isOS2;
        public static IntPtr currentModuleHandle; // For PE

        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }

        public static T Deserialize<T>(byte[] array) where T : struct
        {
            var size = Marshal.SizeOf(typeof(T));
            var ptr = Marshal.AllocHGlobal(size);
            Marshal.Copy(array, 0, ptr, size);
            var s = (T)Marshal.PtrToStructure(ptr, typeof(T));
            Marshal.FreeHGlobal(ptr);
            return s;
        }

        public static string DumpRaw(byte[] data)
        {
            if (data == null || data.Length < 8)
            {
                Console.WriteLine("Data too short.");
                return "Data too short.";
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

                hex.Append(' ', (16 - lineLength) * 3);

                StringBuilder ascii = new StringBuilder();
                for (int j = 0; j < lineLength; j++)
                {
                    byte b = data[lineOffset + j];
                    ascii.Append(b >= 32 && b <= 126 ? (char)b : '.');
                }

                string lineStr = $"{lineOffset:X04}: {hex}| {ascii}";
                Console.WriteLine(lineStr);
                result.AppendLine(lineStr);
            }

            Console.WriteLine();
            result.AppendLine();

            return result.ToString();
        }

    }
}
