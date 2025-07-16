using System;
using System.Collections;
using System.Collections.Generic;
using System.Data;
using System.Diagnostics;
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
        public MainForm()
        {
            InitializeComponent();
        }

        // Read the header and check file type
        private string DetectExecutableHeader(string path)
        {
            try
            {
                Program.isUnicode = false;
                Program.isOS2 = false;
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
                                Program.isOS2 = true;
                                version = " for OS/2";
                                break;
                            case 0x02:
                                version = " for Windows";
                                break;
                            case 0x03:
                                version = " for MS-DOS 4.x";
                                break;
                            case 0x04:
                                version = " for Windows 386";
                                break;
                            case 0x05:
                                version = " for IBM Microkernel Personality Neutral";
                                break;
                        }
                    }

                    if (signature == 0x4550)
                    {
                        Program.isUnicode = true;
                    }

                    switch (signature)
                    {
                        case 0x4550: return $"PE (Portable Executable{version})";
                        case 0x454E: return $"NE (New Executable{version})";
                        case 0x584C: return $"LX (Linear Executable Extended{version})";
                        case 0x454C: return $"LE (Linear Executable{version})";
                    }

                    // 4. Search for typical packer signatures
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


        private void Open()
        {
            OpenFileDialog ofd = new OpenFileDialog();
            // This function needs to be updated to include LE and LX

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
                List<string[]> relations = new List<string[]>();
                Program.currentFilePath = ofd.FileName;
                Program.currentHeaderType = DetectExecutableHeader(Program.currentFilePath);
                treeView1.Nodes.Clear();

                // Clean previous visualization
                flowLayoutPanel1.Controls.Clear();
                lbMessage.Text = "";

                Text = Program.currentHeaderType + " file open: " + Program.currentFilePath;
                if (Program.currentHeaderType.StartsWith("PE"))
                {
                    relations = PeResources.OpenPE(Program.currentFilePath);
                }
                else if (Program.currentHeaderType.StartsWith("NE"))
                {
                    relations = NeResources.OpenNE(Program.currentFilePath);
                }
                else if (Program.currentHeaderType.StartsWith("LE"))
                {
                    relations = LeResources.OpenLE(Program.currentFilePath);
                }
                else if (Program.currentHeaderType.StartsWith("LX"))
                {
                    relations = LxResources.OpenLX(Program.currentFilePath);
                }
                else
                {
                    MessageBox.Show($"Cannot open file: {Program.currentFilePath}\nHeader found: {Program.currentHeaderType}", "Info file");
                }
                foreach (var relation in relations)
                {
                    string parentName = relation[0];
                    string childName = relation[1];

                    if (parentName == "Root")
                    {
                        // Add directly as a root node
                        TreeNode currentNode = treeView1.Nodes.Add(childName);
                        currentNode.ImageKey = "FolderClose";
                        currentNode.SelectedImageKey = "FolderClose";
                    }
                    else
                    {
                        // Search for the parent node among first-level nodes
                        TreeNode parentNode = null;

                        foreach (TreeNode node in treeView1.Nodes)
                        {
                            if (node.Text == parentName)
                            {
                                parentNode = node;
                                break;
                            }
                        }

                        // If not found, create it and add it to the root
                        if (parentNode == null)
                        {
                            parentNode = new TreeNode(parentName);
                            treeView1.Nodes.Add(parentNode);
                            parentNode.ImageKey = "Folder";
                            parentNode.SelectedImageKey = "Folder";
                        }

                        // Add the child to the parent node
                        TreeNode currentNode = parentNode.Nodes.Add(childName);
                        string icon = "File";
                        switch (parentName)
                        {
                            case "RT_FONT":
                            case "RT_FONTDIR":
                                icon = "FontFile";
                                break;
                            case "RT_BITMAP":
                            case "RT_ICON":
                                icon = "BitmapFile";
                                break;
                            case "RT_VERSION":
                            case "RT_MENU":
                            case "RT_STRING":
                                icon = "ConfigFile";
                                break;
                            case "RT_MANIFEST":
                                icon = "XmlFile";
                                break;
                        }
                        currentNode.ImageKey = icon;
                        currentNode.SelectedImageKey = icon;
                    }
                }

            }
        }

        private void treeView1_BeforeExpandCollapse(object sender, TreeViewCancelEventArgs e)
        {
            if (!e.Node.IsExpanded && e.Node.Nodes.Count > 0)
            {
                e.Node.ImageKey = "FolderOpen";
                e.Node.SelectedImageKey = "FolderOpen";
            }
            else if (e.Node.IsExpanded && e.Node.Nodes.Count > 0)
            {
                e.Node.ImageKey = "FolderClose";
                e.Node.SelectedImageKey = "FolderClose";
            }
        }

        byte[] GetData(string headerType, string typeName, string targetResourceName, out string message, out bool found)
        {
            message = "";
            found = false;
            byte[] resData = null;
            if (headerType.StartsWith("PE"))
            {
                resData = PeResources.OpenResourcePE(Program.currentFilePath, typeName, targetResourceName, out message, out found);
            }
            else if (headerType.StartsWith("LX"))
            {
                resData = LxResources.OpenResourceLX(Program.currentFilePath, typeName, targetResourceName, out message, out found);
            }
            else if (headerType.StartsWith("NE"))
            {
                resData = NeResources.OpenResourceNE(Program.currentFilePath, typeName, targetResourceName, out message, out found);
            }
            return resData;
        }

        private void treeView1_AfterSelect(object sender, TreeViewEventArgs e)
        {
            // Clean previous visualization
            flowLayoutPanel1.Controls.Clear();
            lbMessage.Text = "";

            if (e.Node.Parent == null)
            {
                // Resource type node, do not do anything
                return;
            }

            string typeName = e.Node.Parent.Text;
            string resourceLabel = e.Node.Text;

            bool isNumericResource = resourceLabel.StartsWith("#");
            string targetResourceName = isNumericResource ? resourceLabel.Substring(1) : resourceLabel;
            string message = "";
            bool found = false;

            byte[] resData = GetData(Program.currentHeaderType.Substring(0, 2), typeName, targetResourceName, out message, out found);

            if (found)
            {
                if (typeName == "RT_FONTDIR")
                {
                    string val = RT_FONTDIR.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_MENU")
                {
                    Program.DumpRaw(resData);
                    string val = RT_MENU.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_FONT")
                {
                    flowLayoutPanel1.Controls.Add(GetPictureBox(RT_FONT.Get(resData)));
                }
                else if (typeName == "RT_ICON")
                {
                    flowLayoutPanel1.Controls.Add(GetPictureBox(RT_ICON.Get(resData)));
                }
                else if (typeName == "RT_CURSOR")
                {
                    flowLayoutPanel1.Controls.Add(GetPictureBox(RT_CURSOR.Get(resData)));
                }
                else if (typeName == "RT_DISPLAYINFO")
                {
                    string val = RT_DISPLAYINFO.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_POINTER")
                {
                    bool result = false;
                    foreach (Bitmap bmp in RT_POINTER.Get(resData))
                    {
                        result = true;
                        flowLayoutPanel1.Controls.Add(GetPictureBox(bmp));
                    }
                    if (!result)
                    {
                        string val = Program.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                }
                else if (typeName == "RT_BITMAP")
                {
                    bool result = false;
                    foreach (Bitmap bmp in RT_BITMAP.Get(resData))
                    {
                        result = true;
                        flowLayoutPanel1.Controls.Add(GetPictureBox(bmp));
                    }
                    if (!result)
                    {
                        string val = Program.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                }
                else if (typeName == "RT_GROUP_ICON")
                {
                    List<Bitmap> bitmaps = new List<Bitmap>();
                    string val = RT_GROUP_ICON.Get(resData, Program.currentFilePath, out bitmaps);
                    foreach (Bitmap bmp in bitmaps)
                    {
                        flowLayoutPanel1.Controls.Add(GetPictureBox(bmp));
                    }
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_GROUP_CURSOR")
                {
                    List<Bitmap> bitmaps = new List<Bitmap>();
                    string val = RT_GROUP_CURSOR.Get(resData, Program.currentFilePath, out bitmaps);
                    foreach (Bitmap bmp in bitmaps)
                    {
                        flowLayoutPanel1.Controls.Add(GetPictureBox(bmp));
                    }
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_VERSION")
                {
                    string val = RT_VERSION.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_MESSAGE" || typeName == "RT_MESSAGETABLE")
                {
                    string val = RT_MESSAGE.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_STRING")
                {
                    string val = RT_STRING.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else
                {
                    string val = Program.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
            }

            lbMessage.Text = message.Replace("\n", " ");
        }

        PictureBox GetPictureBox(Bitmap bitmap)
        {
            return new PictureBox
            {
                Image = bitmap,
                SizeMode = PictureBoxSizeMode.AutoSize
            };
        }

        TextBox GetTextbox(string val)
        {
            return new TextBox
            {
                AcceptsReturn = true,
                AcceptsTab = true,
                Visible = true,
                Enabled = true,
                Width = flowLayoutPanel1.ClientSize.Width - flowLayoutPanel1.Padding.Horizontal - 50,
                Height = flowLayoutPanel1.ClientSize.Height - flowLayoutPanel1.Padding.Vertical - 50,
                ScrollBars = ScrollBars.Both,
                Font = new Font(new FontFamily("Consolas"), 10, FontStyle.Regular, GraphicsUnit.Point),
                Multiline = true,
                Text = val,
            };
        }

        void OpenIssue()
        {
            Process.Start("https://github.com/RaulMerelli/Peare/issues");
        }

        void OpenAbout()
        {
            new About().ShowDialog();
        }

        private void menuClick(object sender, EventArgs e)
        {
            switch (sender)
            {
                case var _ when sender == mnu_ExpandTreeview:
                    treeView1.ExpandAll();
                    break;

                case var _ when sender == mnu_CollapseTreeview:
                    treeView1.CollapseAll();
                    break;

                case var _ when sender == mnu_Open:
                    Open();
                    break;

                case var _ when sender == mnu_Exit:
                    Close();
                    break;

                case var _ when sender == mnu_OpenIssue:
                    OpenIssue();
                    break;

                case var _ when sender == mnu_About:
                    OpenAbout();
                    break;
            }
        }

        [DllImport("user32.dll", CharSet = CharSet.Auto)]
        private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

        [DllImport("uxtheme.dll", CharSet = CharSet.Unicode)]
        public static extern int SetWindowTheme(IntPtr hWnd, string pszSubAppName, string pszSubIdList);

        const int TVM_SETEXTENDEDSTYLE = 0x1100 + 44;
        const int TVS_EX_DOUBLEBUFFER = 0x0004;
        const int TVS_EX_FADEINOUTEXPANDOS = 0x0040;

        private void SetTreeViewStyle()
        {
            SendMessage(treeView1.Handle, TVM_SETEXTENDEDSTYLE, (IntPtr)(TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS), (IntPtr)(TVS_EX_DOUBLEBUFFER | TVS_EX_FADEINOUTEXPANDOS));
            SetWindowTheme(treeView1.Handle, "Explorer", null);
        }

        private void MainForm_Load(object sender, EventArgs e)
        {
            SetTreeViewStyle();

            // Add icons to imagelist using our method or from the associated extension icon as fallback
            string shell32 = @"C:\Windows\System32\shell32.dll";
            string mmcndmgr = @"C:\Windows\System32\mmcndmgr.dll";
            List<Bitmap> bitmaps = new List<Bitmap>();
            Bitmap bmp = null;
            // Folder Close
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "4", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("FolderClose", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            // Folder Open
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "5", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("FolderOpen", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            // File
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "1", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("File", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            // Font File
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "155", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("FontFile", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            // Config File
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "151", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("ConfigFile", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            // Bitmap File
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(shell32, "RT_GROUP_ICON", "16823", out _, out _), shell32, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("BitmapFile", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            else
            {
                bmp = IconFromExt.Get(".bmp");
                imageList1.Images.Add("BitmapFile", bmp == null ? imageList1.Images[2] : bmp);
            }
            // Xml File
            RT_GROUP_ICON.Get(PeResources.OpenResourcePE(mmcndmgr, "RT_GROUP_ICON", "1098", out _, out _), mmcndmgr, out bitmaps);
            if (bitmaps.Count > 0)
            {
                imageList1.Images.Add("XmlFile", bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
            }
            else
            {
                bmp = IconFromExt.Get(".xml");
                imageList1.Images.Add("XmlFile", bmp == null ? imageList1.Images[2] : bmp);
            }
        }
    }
}