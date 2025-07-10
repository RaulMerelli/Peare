using System;
using System.Text;
using System.Windows.Forms;

namespace Peare
{
    public static class Program
    {
        public static string currentFilePath;
        public static string currentHeaderType; // "PE" or "NE"
        public static IntPtr currentModuleHandle; // For PE

        [STAThread]
        static void Main()
        {
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainForm());
        }

        public static void DumpRaw(byte[] data)
        {
            if (data == null || data.Length < 8)
            {
                Console.WriteLine("Data too short.");
                return;
            }

            int offset = 0;

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

                Console.WriteLine($"{lineOffset:X04}: {hex} | {ascii}");
            }

            Console.WriteLine();
        }
    }
}
