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
                    else if(signature == 0x454C || signature == 0x584C)
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
                        treeView1.Nodes.Add(childName);
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
                        }

                        // Add the child to the parent node
                        parentNode.Nodes.Add(childName);
                    }
                }

            }
        }

        byte[] GetData(string headerType, string typeName, string targetResourceName, out string message, out bool found)
        {
            message = "";
            found = false;
            byte[] resData = null;
            if (headerType.StartsWith("PE"))
            {
                resData = PeResources.OpenResourcePE(typeName, targetResourceName, out message, out found);
            }
            else if (headerType.StartsWith("LX"))
            {
                resData = LxResources.OpenResourceLX(typeName, targetResourceName, out message, out found);
            }
            else if (headerType.StartsWith("NE"))
            {
                resData = NeResources.OpenResourceNE(typeName, targetResourceName, out message, out found);
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
                    string val = RT_GROUP_ICON.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                }
                else if (typeName == "RT_GROUP_CURSOR")
                {
                    string val = RT_GROUP_CURSOR.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    string dump = Program.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
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
                    string dump = Program.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                }
                else if (typeName == "RT_STRING")
                {
                    string val = RT_STRING.Get(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    string dump = Program.DumpRaw(resData);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
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
    }
}
