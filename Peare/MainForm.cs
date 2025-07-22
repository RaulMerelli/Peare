using Peare.Properties;
using PeareModule;
using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

namespace Peare
{
    public partial class MainForm : Form
    {
        public static string currentFilePath;

        public MainForm()
        {
            InitializeComponent();
        }

        private void Open()
        {
            OpenFileDialog ofd = new OpenFileDialog();
            // This function needs to be updated to include LE and LX

            // Separeted extensions list
            string[] peExtArray = new string[] { "exe", "dll", "sys", "ocx", "cpl", "scr", "drv", "ax", "efi", "mui", "tlb", "acm", "spl", "sct", "wll", "xll", "fll", "pyd", "bpl", "ifs", "msstyles" };
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
                currentFilePath = ofd.FileName;
                List<string[]> relations = ModuleResources.ListTypesAndRes(currentFilePath);
                ModuleResources.ModuleProperties moduleProperties = ModuleResources.GetModuleProperties(currentFilePath);

                // Clean previous visualization
                treeView1.Nodes.Clear();
                flowLayoutPanel1.Controls.Clear();
                lbMessage.Text = "";

                Text = moduleProperties.Description + " file open: " + currentFilePath;
                if (moduleProperties.headerType == ModuleResources.HeaderType.Error)
                {
                    MessageBox.Show($"Cannot open file: {currentFilePath}\nHeader found: {moduleProperties.Description}", "Info file");
                }

                foreach (var relation in relations)
                {
                    string parentName = relation[0];
                    string childName = relation[1];

                    if (parentName == "Root")
                    {
                        // Add directly as a root node
                        TreeNode currentNode = treeView1.Nodes.Add(childName);
                        currentNode.ImageKey = "RT_FOLDER_CLOSE";
                        currentNode.SelectedImageKey = "RT_FOLDER_CLOSE";
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
                            parentNode.ImageKey = "RT_FOLDER_OPEN";
                            parentNode.SelectedImageKey = "RT_FOLDER_OPEN";
                        }

                        // Add the child to the parent node
                        TreeNode currentNode = parentNode.Nodes.Add(childName);
                        string icon = "RT_DEFAULT";
                        if (imageList1.Images.Keys.Contains(parentName))
                        {
                            icon = parentName;
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

            ModuleResources.ModuleProperties moduleProperties = ModuleResources.GetModuleProperties(currentFilePath);
            byte[] resData = ModuleResources.OpenResource(moduleProperties, typeName, targetResourceName, out message, out found);

            if (found)
            {
                if (typeName == "RT_FONTDIR")
                {
                    string val = RT_FONTDIR.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_MENU")
                {
                    ModuleResources.DumpRaw(resData);
                    string val = RT_MENU.Get(resData, moduleProperties);
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
                        string val = ModuleResources.DumpRaw(resData);
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
                        string val = ModuleResources.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                }
                else if (typeName == "RT_GROUP_ICON")
                {
                    List<Bitmap> bitmaps = new List<Bitmap>();
                    string val = RT_GROUP_ICON.Get(resData, moduleProperties, out bitmaps);
                    foreach (Bitmap bmp in bitmaps)
                    {
                        flowLayoutPanel1.Controls.Add(GetPictureBox(bmp));
                    }
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_GROUP_CURSOR")
                {
                    List<Bitmap> bitmaps = new List<Bitmap>();
                    string val = RT_GROUP_CURSOR.Get(resData, moduleProperties, out bitmaps);
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
                else if (typeName == "RT_NAMETABLE")
                {
                    string val = RT_NAMETABLE.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    string dump = ModuleResources.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                }
                else if (typeName == "RT_ACCELERATOR")
                {
                    string val = RT_ACCELERATOR.Get(resData, moduleProperties);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    string dump = ModuleResources.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                }
                else if (typeName == "RT_ACCELTABLE")
                {
                    string val = RT_ACCELTABLE.Get(resData, moduleProperties);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    string dump = ModuleResources.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                }
                else if (typeName == "RT_MESSAGE" || typeName == "RT_MESSAGETABLE")
                {
                    string val = RT_MESSAGE.Get(resData, moduleProperties);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_STRING")
                {
                    string val = RT_STRING.Get(resData, moduleProperties);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else
                {
                    string val = ModuleResources.DumpRaw(resData, true);
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

        public class MenuLoader
        {
            public MenuLoader(string filepath, string resName, string extDefault)
            {
                this.filepath = filepath;
                this.resName = resName;
                this.extDefault = extDefault;
            }

            public string filepath;
            public string resName;
            public string extDefault;
        }

        private void MainForm_Load(object sender, EventArgs e)
        {
            Stopwatch sw = Stopwatch.StartNew();
            string shell32 = @"C:\Windows\System32\shell32.dll";
            string mmcndmgr = @"C:\Windows\System32\mmcndmgr.dll";

            IniFile settings = new IniFile("Settings.ini");

            SetTreeViewStyle();

            void WriteIcoSection(string type, string path, string ordinal, string ext)
            {
                settings.WriteIfKeyNotExists("path", path, type);
                settings.WriteIfKeyNotExists("ordinal", ordinal, type);
                settings.WriteIfKeyNotExists("iconExt", ext, type);
            }

            WriteIcoSection("RT_FOLDER_OPEN", shell32, "4", "folder_open"); 
            WriteIcoSection("RT_FOLDER_CLOSE", shell32, "5", "folder_close");
            WriteIcoSection("RT_DEFAULT", shell32, "1", "default_unknown_icon");
            WriteIcoSection("RT_FONT", shell32, "155", "fon");
            WriteIcoSection("RT_FONTDIR", shell32, "155", "fon");
            WriteIcoSection("RT_VERSION", shell32, "151", "ini");
            WriteIcoSection("RT_MENU", shell32, "151", "ini");
            WriteIcoSection("RT_STRING", shell32, "151", "ini");
            WriteIcoSection("RT_BITMAP", shell32, "16823", "bmp");
            WriteIcoSection("RT_ICON", shell32, "16823", "bmp");
            WriteIcoSection("RT_MANIFEST", mmcndmgr, "1098", "xml");

            List<Bitmap> bitmaps = new List<Bitmap>();
            Bitmap bmp = null;
            Dictionary<string, ModuleResources.ModuleProperties> properties = new Dictionary<string, ModuleResources.ModuleProperties>();
            foreach (string section in settings.GetSections())
            {
                if (!string.IsNullOrEmpty(section))
                {
                    string path = settings.Read("path", section);
                    string ordinal = settings.Read("ordinal", section);
                    string iconExt = settings.Read("iconExt", section);

                    if (!properties.ContainsKey(path))
                    {
                        properties[path] = ModuleResources.GetModuleProperties(path);
                    }
                    RT_GROUP_ICON.Get(ModuleResources.OpenResource(properties[path], "RT_GROUP_ICON", ordinal, out _, out _), properties[path], out bitmaps);
                    if (bitmaps.Count > 0)
                    {
                        imageList1.Images.Add(section, bitmaps.Where(x => x.Width == 16 && x.Height == 16).Last());
                    }
                    else if (iconExt.StartsWith("folder"))
                    {
                        bmp = IconFromExt.GetFolder();
                        imageList1.Images.Add(section, bmp == null ? imageList1.Images[2] : bmp);
                    }
                    else
                    {
                        bmp = IconFromExt.Get(iconExt);
                        imageList1.Images.Add(section, bmp == null ? imageList1.Images[2] : bmp);
                    }
                }
            }
            sw.Stop();
            Console.WriteLine($"Peare started in {sw.ElapsedMilliseconds}ms");
        }
    }
}