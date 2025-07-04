using System;
using System.Collections;
using System.Collections.Generic;
using System.Data;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;
using System.Xml.Linq;

namespace Peare
{
    public partial class MainForm : Form
    {
        string currentFilePath;
        string currentHeaderType; // "PE" or "NE"
        IntPtr currentModuleHandle; // For PE

        public const int RT_CURSOR = 1;
        public const int RT_BITMAP = 2;
        public const int RT_ICON = 3;
        public const int RT_MENU = 4;
        public const int RT_DIALOG = 5;
        public const int RT_STRING = 6;
        public const int RT_FONTDIR = 7;
        public const int RT_FONT = 8;
        public const int RT_ACCELERATOR = 9;
        public const int RT_RCDATA = 10;
        public const int RT_MESSAGETABLE = 11;
        public const int RT_GROUP_CURSOR = RT_CURSOR + 11;
        public const int RT_GROUP_ICON = RT_ICON + 11;
        public const int RT_VERSION = 16;
        public const int RT_DLGINCLUDE = 17;
        public const int RT_PLUGPLAY = 19;
        public const int RT_VXD = 20;
        public const int RT_ANICURSOR = 21;
        public const int RT_ANIICON = 22;
        public const int RT_HTML = 23;
        public const int RT_MANIFEST = 24;

        Dictionary<int, string> resourceTypes = new Dictionary<int, string>
        {
            { RT_CURSOR, nameof(RT_CURSOR) },
            { RT_BITMAP, nameof(RT_BITMAP) },
            { RT_ICON, nameof(RT_ICON) },
            { RT_MENU, nameof(RT_MENU) },
            { RT_DIALOG, nameof(RT_DIALOG) },
            { RT_STRING, nameof(RT_STRING) },
            { RT_FONTDIR, nameof(RT_FONTDIR) },
            { RT_FONT, nameof(RT_FONT) },
            { RT_ACCELERATOR, nameof(RT_ACCELERATOR) },
            { RT_RCDATA, nameof(RT_RCDATA) },
            { RT_MESSAGETABLE, nameof(RT_MESSAGETABLE) },
            { RT_GROUP_CURSOR, nameof(RT_GROUP_CURSOR) },
            { RT_GROUP_ICON, nameof(RT_GROUP_ICON) },
            { RT_VERSION, nameof(RT_VERSION) },
            { RT_DLGINCLUDE, nameof(RT_DLGINCLUDE) },
            { RT_PLUGPLAY, nameof(RT_PLUGPLAY) },
            { RT_VXD, nameof(RT_VXD) },
            { RT_ANICURSOR, nameof(RT_ANICURSOR) },
            { RT_ANIICON, nameof(RT_ANIICON) },
            { RT_HTML, nameof(RT_HTML) },
            { RT_MANIFEST, nameof(RT_MANIFEST) }
        };

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


        public MainForm()
        {
            InitializeComponent();
        }

        private void mnu_Open_Click(object sender, EventArgs e)
        {
            OpenFileDialog ofd = new OpenFileDialog();

            // Separeted extensions list
            string[] peExtArray = new string[] { "exe", "dll", "sys", "ocx", "cpl", "scr", "drv", "ax", "efi", "mui", "tlb", "acm", "spl", "sct", "wll", "xll", "fll", "pyd", "bpl", "msstyles" };
            string[] neExtArray = new string[] { "exe", "dll", "drv", "386", "vxd", "fon" };

            // Build string filter with *.
            string peExt = string.Join(";", peExtArray.Select(ext => "*." + ext));
            string neExt = string.Join(";", neExtArray.Select(ext => "*." + ext));

            // Join and remove duplicates
            var combinedSet = new HashSet<string>(peExtArray.Concat(neExtArray));
            string combinedExt = string.Join(";", combinedSet.Select(ext => "*." + ext));

            ofd.Filter =
                $"PE + NE files ({combinedExt})|{combinedExt}|" +
                $"PE files ({peExt})|{peExt}|" +
                $"NE files ({neExt})|{neExt}|" +
                "All files (*.*)|*.*";

            if (ofd.ShowDialog() == DialogResult.OK)
            {
                if (currentModuleHandle != IntPtr.Zero)
                {
                    FreeLibrary(currentModuleHandle);
                }
                currentFilePath = ofd.FileName;
                currentHeaderType = DetectExecutableHeader(currentFilePath);
                treeView1.Nodes.Clear();

                // Clean previous visualization
                flowLayoutPanel1.Controls.Clear();
                textBox1.Clear();

                if (currentHeaderType == "PE (Portable Executable)")
                {
                    Text = "PE file open: " + currentFilePath;
                    OpenPE(currentFilePath);
                }
                else if (currentHeaderType == "NE (New Executable)")
                {
                    Text = "NE file open: " + currentFilePath;
                    OpenNE(currentFilePath);
                }
                else if (currentHeaderType == "LE (Linear Executable, VxD or DOS Extender)")
                {
                    Text = "LE file open: " + currentFilePath;
                    OpenNE(currentFilePath);
                }
                else
                {
                    MessageBox.Show($"Cannot open file: {currentFilePath}\nHeader found: {currentHeaderType}", "Info file");
                }
            }
        }

        void OpenPE(string filePath)
        {
            currentModuleHandle = LoadLibraryEx(filePath, IntPtr.Zero, LoadLibraryFlags.LOAD_LIBRARY_AS_DATAFILE);

            EnumResourceTypes(currentModuleHandle, (h, typePtr, lParam) =>
            {
                bool isInt = IsIntResource(typePtr);
                string typeName = isInt
                    ? resourceTypes.TryGetValue(typePtr.ToInt32(), out var name) ? name : $"#{typePtr.ToInt32()}"
                    : Marshal.PtrToStringAnsi(typePtr);

                TreeNode typeNode = new TreeNode(typeName);

                EnumResNameProc nameCallback = (h2, t, namePtr, lparam2) =>
                {
                    string nameStr = IsIntResource(namePtr)
                        ? $"#{namePtr.ToInt32()}"
                        : Marshal.PtrToStringAuto(namePtr);
                    typeNode.Nodes.Add(nameStr);
                    return true;
                };

                if (isInt)
                    EnumResourceNames(currentModuleHandle, typePtr, nameCallback, IntPtr.Zero);
                else
                    EnumResourceNames(currentModuleHandle, typeName, nameCallback, IntPtr.Zero);

                treeView1.Nodes.Add(typeNode);
                return true;
            }, IntPtr.Zero);
        }

        bool IsIntResource(IntPtr ptr)
        {
            // if the value is <= 0xFFFF allora è intero, altrimenti è puntatore a stringa
            return ((ulong)ptr.ToInt64() & 0xFFFF0000) == 0;
        }

        void OpenNE(string filePath)
        {
            byte[] fileBytes = File.ReadAllBytes(filePath);
            int neHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
            int resourceTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
            int resourceTablePos = neHeaderOffset + resourceTableOffset;

            ushort alignShift = BitConverter.ToUInt16(fileBytes, resourceTablePos);
            int align = 1 << alignShift;

            int pos = resourceTablePos + 2;

            while (true)
            {
                if (pos + 2 > fileBytes.Length) break;
                ushort typeID = BitConverter.ToUInt16(fileBytes, pos);
                if (typeID == 0) break;

                bool isNamed = (typeID & 0x8000) == 0;
                ushort type = (ushort)(typeID & 0x7FFF);

                string typeName;
                if (isNamed)
                {
                    int nameOffset = resourceTablePos + type;
                    if (nameOffset < fileBytes.Length)
                    {
                        byte nameLen = fileBytes[nameOffset];
                        if (nameOffset + 1 + nameLen <= fileBytes.Length)
                        {
                            typeName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                        }
                        else
                        {
                            typeName = "#[InvalidTypeName]";
                        }
                    }
                    else
                    {
                        typeName = "#[InvalidTypeOffset]";
                    }
                }
                else
                {
                    if (type <= 11 && resourceTypes.ContainsKey(type))
                        typeName = resourceTypes[type];
                    else
                        typeName = $"#{type}";
                }

                if (pos + 8 > fileBytes.Length) break;
                ushort resourceCount = BitConverter.ToUInt16(fileBytes, pos + 2);
                pos += 8;

                List<string> resourceNames = new List<string>();
                for (int i = 0; i < resourceCount; i++)
                {
                    if (pos + 12 > fileBytes.Length) break;

                    ushort idField = BitConverter.ToUInt16(fileBytes, pos + 6);
                    string resourceName;

                    if ((idField & 0x8000) == 0)
                    {
                        int nameOffset = resourceTablePos + idField;
                        if (nameOffset < fileBytes.Length)
                        {
                            byte nameLen = fileBytes[nameOffset];
                            if (nameOffset + 1 + nameLen <= fileBytes.Length)
                            {
                                resourceName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                            }
                            else
                            {
                                resourceName = "#[Invalid]";
                            }
                        }
                        else
                        {
                            resourceName = "#[Invalid]";
                        }
                    }
                    else
                    {
                        resourceName = "#" + (idField & 0x7FFF);
                    }
                    resourceNames.Add(resourceName);
                    pos += 12;
                }

                TreeNode typeNode = new TreeNode(typeName);
                foreach (var name in resourceNames)
                {
                    typeNode.Nodes.Add(name);
                }

                if (typeNode.Nodes.Count > 0)
                {
                    treeView1.Nodes.Add(typeNode);
                }
            }
        }

        // Read the header and check file type
        private string DetectExecutableHeader(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read))
                using (var br = new BinaryReader(fs))
                {
                    // 1. Verify MZ
                    ushort mzSignature = br.ReadUInt16();
                    if (mzSignature != 0x5A4D) // "MZ"
                        return "Not an executable (no MZ header)";

                    // 2. Go to offset 0x3C in order to find the extended header offset
                    fs.Seek(0x3C, SeekOrigin.Begin);
                    int headerOffset = br.ReadInt32();

                    // 3. Go to extended header and read the signature
                    fs.Seek(headerOffset, SeekOrigin.Begin);
                    ushort signature = br.ReadUInt16();

                    switch (signature)
                    {
                        case 0x4550: return "PE (Portable Executable)";
                        case 0x454E: return "NE (New Executable)";
                        case 0x584C: return "LX (Linear Executable)";
                        case 0x454C: return "LE (Linear Executable, VxD or DOS Extender)";
                    }

                    // 4. Cerca firme tipiche di packer
                    fs.Seek(0, SeekOrigin.Begin);
                    byte[] fullData = br.ReadBytes((int)Math.Min(fs.Length, 4096)); // max 4 KB 

                    string fullText = System.Text.Encoding.ASCII.GetString(fullData);

                    if (fullText.Contains("UPX!"))
                        return "MZ (possibly packed with UPX)";
                    if (fullText.Contains("PKLITE"))
                        return "MZ (possibly packed with PKLITE)";
                    if (fullText.Contains("LZ91") || fullText.Contains("LZEXE"))
                        return "MZ (possibly packed with LZEXE)";
                    if (fullText.Contains("EXEPACK"))
                        return "MZ (possibly packed with EXEPACK)";

                    // 5. Generic case
                    return "MZ without known secondary header (maybe plain DOS MZ or unknown packer)";
                }
            }
            catch (Exception ex)
            {
                return "An error happened analyzing the file";
            }
        }

        private void treeView1_AfterSelect(object sender, TreeViewEventArgs e)
        {
            // Clean previous visualization
            flowLayoutPanel1.Controls.Clear();
            textBox1.Clear();

            if (e.Node.Parent == null)
            {
                // Resource type node, do not do anything
                return;
            }

            string typeName = e.Node.Parent.Text;
            string resourceLabel = e.Node.Text;

            bool isNumericResource = resourceLabel.StartsWith("#");
            string targetResourceName = isNumericResource ? resourceLabel.Substring(1) : resourceLabel;
            if (currentHeaderType == "PE (Portable Executable)")
            {
                if (currentModuleHandle == IntPtr.Zero) return;

                if (typeName == nameof(RT_STRING))
                {
                    // Wrong
                    var strings = LoadStrings(currentModuleHandle, targetResourceName, typeName);
                    textBox1.Lines = strings;
                }
                else if (typeName == nameof(RT_BITMAP))
                {
                    AddBitmapToFlow(currentModuleHandle, targetResourceName);
                }
                else
                {
                    textBox1.Text = $"Resource {typeName} {targetResourceName} selected.";
                    var strings = LoadStrings(currentModuleHandle, targetResourceName, typeName);
                    textBox1.Lines = strings;
                }
            }
            else if (currentHeaderType == "NE (New Executable)")
            {
                byte[] fileBytes = File.ReadAllBytes(currentFilePath);
                int neHeaderOffset = BitConverter.ToInt32(fileBytes, 0x3C);
                int resourceTableOffset = BitConverter.ToUInt16(fileBytes, neHeaderOffset + 0x24);
                int resourceTablePos = neHeaderOffset + resourceTableOffset;
                ushort alignShift = BitConverter.ToUInt16(fileBytes, resourceTablePos);
                int align = 1 << alignShift;

                int pos = resourceTablePos + 2;
                bool found = false;

                while (true)
                {
                    if (pos + 2 > fileBytes.Length) break;

                    ushort typeID = BitConverter.ToUInt16(fileBytes, pos);
                    if (typeID == 0) break;

                    bool isTypeNamed = (typeID & 0x8000) == 0;
                    ushort typeVal = (ushort)(typeID & 0x7FFF);

                    string currentTypeName;
                    if (isTypeNamed)
                    {
                        int nameOffset = resourceTablePos + typeVal;
                        if (nameOffset < fileBytes.Length)
                        {
                            byte len = fileBytes[nameOffset];
                            currentTypeName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, len);
                        }
                        else currentTypeName = "#[InvalidType]";
                    }
                    else
                    {
                        if (typeVal <= 11)
                        {
                            currentTypeName = resourceTypes.ContainsKey(typeVal) ? resourceTypes[typeVal] : $"#{typeVal}";
                        }
                        else
                        {
                            currentTypeName = $"#{typeVal}";
                        }
                    }

                    ushort resourceCount = BitConverter.ToUInt16(fileBytes, pos + 2);
                    pos += 8;

                    if (currentTypeName == typeName)
                    {
                        for (int i = 0; i < resourceCount; i++)
                        {
                            if (pos + 12 > fileBytes.Length) break;

                            ushort offsetUnits = BitConverter.ToUInt16(fileBytes, pos);
                            ushort lengthUnits = BitConverter.ToUInt16(fileBytes, pos + 2);
                            ushort idField = BitConverter.ToUInt16(fileBytes, pos + 6);

                            bool isName = (idField & 0x8000) == 0;

                            bool match = false;

                            if (isNumericResource && !isName)
                            {
                                int idVal = idField & 0x7FFF;
                                if (idVal.ToString() == targetResourceName)
                                    match = true;
                            }
                            else if (!isNumericResource && isName)
                            {
                                int nameOffset = resourceTablePos + idField;
                                if (nameOffset < fileBytes.Length)
                                {
                                    byte nameLen = fileBytes[nameOffset];
                                    if (nameOffset + 1 + nameLen <= fileBytes.Length)
                                    {
                                        string resName = Encoding.ASCII.GetString(fileBytes, nameOffset + 1, nameLen);
                                        if (resName == targetResourceName)
                                            match = true;
                                    }
                                }
                            }

                            if (match)
                            {
                                int dataOffset = offsetUnits * align;
                                int dataLength = lengthUnits * align;

                                byte[] resData = new byte[dataLength];

                                Array.Copy(fileBytes, dataOffset, resData, 0, dataLength);

                                textBox1.Text = $"Resource NE {typeName} {resourceLabel} selected.\nLength: {dataLength} byte.\r\n";

                                if (typeName == nameof(RT_BITMAP))
                                {
                                    flowLayoutPanel1.Controls.Add(new PictureBox
                                    {
                                        Image = BitmapNE.Get(resData),
                                        SizeMode = PictureBoxSizeMode.AutoSize
                                    });
                                }
                                else if (typeName == nameof(RT_ICON))
                                {
                                    flowLayoutPanel1.Controls.Add(new PictureBox
                                    {
                                        Image = IconNE.Get(resData),
                                        SizeMode = PictureBoxSizeMode.AutoSize
                                    });
                                }
                                else if (typeName == nameof(RT_FONT))
                                {
                                    flowLayoutPanel1.Controls.Add(new PictureBox
                                    {
                                        Image = Fnt.Get(resData),
                                        SizeMode = PictureBoxSizeMode.AutoSize
                                    });
                                }
                                else
                                {
                                    string val = System.Text.Encoding.ASCII.GetString(resData, 0, resData.Length).Replace('\0', ' ') + "\r\n" + BitConverter.ToString(resData);
                                    flowLayoutPanel1.Controls.Add(new TextBox
                                    {
                                        AcceptsReturn = true,
                                        AcceptsTab = true,
                                        Visible = true,
                                        Enabled = true,
                                        Width = flowLayoutPanel1.ClientSize.Width - flowLayoutPanel1.Padding.Horizontal,
                                        Height = flowLayoutPanel1.ClientSize.Height - flowLayoutPanel1.Padding.Vertical,
                                        Multiline = true,
                                        Text = val,
                                    });
                                }

                                found = true;
                                break;
                            }

                            pos += 12;
                        }

                        if (found) break;
                    }
                    else
                    {
                        pos += resourceCount * 12;
                    }
                }

                if (!found)
                {
                    textBox1.Text = $"Resource {typeName} {resourceLabel} not found.";
                }
            }
        }

        string[] LoadStrings(IntPtr hDll, string lpName, string lpType)
        {
            List<string> retList = new List<string>();
            // Find and load the resource
            IntPtr hResource = FindResource(hDll, lpName, lpType);
            IntPtr hResourceData = LoadResource(hDll, hResource);

            // Access the text
            IntPtr pText = LockResource(hResourceData);
            uint size = SizeofResource(hDll, hResource);
            byte[] bytes = new byte[size];

            string text = "";

            if (pText != IntPtr.Zero)
            {
                for (int i = 0; i < bytes.Length; i++)
                {
                    bytes[i] = Marshal.ReadByte(pText, i);
                }
                foreach (char c in Encoding.Unicode.GetChars(bytes))
                {
                    if (c == '\0') //NUL
                    {
                        retList.Add(text);
                        text = "";
                    }
                    else
                    {
                        text += c;
                    }
                }
            }
            retList.Add(text);

            return retList.ToArray();
        }

        void AddBitmapToFlow(IntPtr hDll, string lpBitmapName)
        {
            IntPtr myimage = LoadBitmap(hDll, lpBitmapName);
            Bitmap bitmap = Bitmap.FromHbitmap(myimage);
            var pbox = new PictureBox();
            pbox.SizeMode = PictureBoxSizeMode.AutoSize;
            pbox.Image = bitmap;
            flowLayoutPanel1.Controls.Add(pbox);
            // Remember to free the HBITMAP when you're done with it
            DeleteObject(myimage);
        }

    }
}
