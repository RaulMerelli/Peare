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

        private ModuleResources.ModuleProperties currentModuleProperties;
        private byte[] selectedResourceData;
        private string selectedResourceType;
        private string selectedResourceLabel;
        private object selectedDecodedResource;
        private ResourceFileFormat selectedOriginalFormat;
        private List<string> selectedConversionExtensions = new List<string>();

        private sealed class PreparedFontGlyph : IDisposable
        {
            public Bitmap Bitmap;
            public int CharacterCode;
            public int DrawX;
            public int DrawY;

            public void Dispose()
            {
                if (Bitmap != null)
                {
                    Bitmap.Dispose();
                    Bitmap = null;
                }
            }
        }

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
                currentModuleProperties = ModuleResources.GetModuleProperties(currentFilePath);
                ModuleResources.ModuleProperties moduleProperties = currentModuleProperties;

                ClearSelectedResource();

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

                UpdateResourceMenu();
            }
        }

        private void treeView1_BeforeExpandCollapse(object sender, TreeViewCancelEventArgs e)
        {
            if (!e.Node.IsExpanded && e.Node.Nodes.Count > 0)
            {
                e.Node.ImageKey = "RT_FOLDEROPEN";
                e.Node.SelectedImageKey = "RT_FOLDEROPEN";
            }
            else if (e.Node.IsExpanded && e.Node.Nodes.Count > 0)
            {
                e.Node.ImageKey = "RT_FOLDER_CLOSE";
                e.Node.SelectedImageKey = "RT_FOLDER_CLOSE";
            }
        }

        private void treeView1_AfterSelect(object sender, TreeViewEventArgs e)
        {
            flowLayoutPanel1.Controls.Clear();
            lbMessage.Text = "";
            ClearSelectedResource();

            if (e.Node.Parent == null)
            {
                UpdateResourceMenu();
                return;
            }

            string typeName = e.Node.Parent.Text;
            string resourceLabel = e.Node.Text;
            bool isNumericResource = resourceLabel.StartsWith("#");
            string targetResourceName = isNumericResource ? resourceLabel.Substring(1) : resourceLabel;
            string message = "";
            bool found = false;

            ModuleResources.ModuleProperties moduleProperties = currentModuleProperties ?? ModuleResources.GetModuleProperties(currentFilePath);
            byte[] resData = ModuleResources.OpenResource(moduleProperties, typeName, targetResourceName, out message, out found);

            if (found)
            {
                selectedResourceData = resData;
                selectedResourceType = typeName;
                selectedResourceLabel = resourceLabel;

                try
                {
                    if (typeName == "RT_FONTDIR")
                    {
                        string val = RT_FONTDIR.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_MENU")
                    {
                        ModuleResources.DumpRaw(resData);
                        string val = RT_MENU.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_FONT")
                    {
                        DecodedFont font = RT_FONT.Decode(resData, moduleProperties);
                        selectedDecodedResource = font;
                        ShowFontPreview(font);
                    }
                    else if (typeName == "RT_ICON")
                    {
                        Img image = RT_ICON.Get(resData);
                        selectedDecodedResource = image;
                        flowLayoutPanel1.Controls.Add(GetPictureBox(image.Bitmap));
                    }
                    else if (typeName == "RT_CURSOR")
                    {
                        Img image = RT_CURSOR.Get(resData);
                        selectedDecodedResource = image;
                        flowLayoutPanel1.Controls.Add(GetPictureBox(image.Bitmap));
                    }
                    else if (typeName == "RT_DISPLAYINFO")
                    {
                        string val = RT_DISPLAYINFO.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_HELPTABLE")
                    {
                        string val = RT_HELPTABLE.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_HELPSUBTABLE")
                    {
                        string val = RT_HELPSUBTABLE.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_DLGINCLUDE")
                    {
                        string val = RT_DLGINCLUDE.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_POINTER")
                    {
                        List<Img> images = RT_POINTER.Get(resData);
                        selectedDecodedResource = images;
                        bool result = false;
                        foreach (Img img in images)
                        {
                            result = true;
                            flowLayoutPanel1.Controls.Add(GetPictureBox(img.Bitmap));
                        }
                        if (!result)
                        {
                            string val = ModuleResources.DumpRaw(resData);
                            flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        }
                    }
                    else if (typeName == "RT_BITMAP")
                    {
                        List<Img> images = RT_BITMAP.Get(resData);
                        selectedDecodedResource = images;
                        bool result = false;
                        foreach (Img img in images)
                        {
                            result = true;
                            flowLayoutPanel1.Controls.Add(GetPictureBox(img.Bitmap));
                        }
                        if (!result)
                        {
                            string val = ModuleResources.DumpRaw(resData);
                            flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        }
                    }
                    else if (typeName == "RT_GROUP_ICON")
                    {
                        List<Img> imgs = new List<Img>();
                        string val = RT_GROUP_ICON.Get(resData, moduleProperties, out imgs);
                        selectedDecodedResource = imgs.Count > 0 ? (object)imgs : val;
                        foreach (Img img in imgs)
                            flowLayoutPanel1.Controls.Add(GetPictureBox(img.Bitmap));
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_GROUP_CURSOR")
                    {
                        List<Img> imgs = new List<Img>();
                        string val = RT_GROUP_CURSOR.Get(resData, moduleProperties, out imgs);
                        selectedDecodedResource = imgs.Count > 0 ? (object)imgs : val;
                        foreach (Img img in imgs)
                            flowLayoutPanel1.Controls.Add(GetPictureBox(img.Bitmap));
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_VERSION")
                    {
                        string val = RT_VERSION.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_NAMETABLE")
                    {
                        string val = RT_NAMETABLE.Get(resData);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        string dump = ModuleResources.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                    }
                    else if (typeName == "RT_ACCELERATOR")
                    {
                        string val = RT_ACCELERATOR.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        string dump = ModuleResources.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                    }
                    else if (typeName == "RT_ACCELTABLE")
                    {
                        string val = RT_ACCELTABLE.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        string dump = ModuleResources.DumpRaw(resData);
                        flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                    }
                    else if (typeName == "RT_MESSAGE" || typeName == "RT_MESSAGETABLE")
                    {
                        string val = RT_MESSAGE.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_STRING")
                    {
                        string val = RT_STRING.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else if (typeName == "RT_DIALOG")
                    {
                        string val = RT_DIALOG.Get(resData, moduleProperties);
                        selectedDecodedResource = val;
                        flowLayoutPanel1.Controls.Add(GetTextbox(val));
                    }
                    else
                    {
                        object rawResult = ModuleResources.RawDetect(resData, moduleProperties);
                        selectedDecodedResource = rawResult;
                        if (!ShowRawResult(rawResult))
                        {
                            string val = ModuleResources.DumpRaw(resData, true);
                            flowLayoutPanel1.Controls.Add(GetTextbox(val));
                        }
                    }
                }
                catch (Exception ex)
                {
                    selectedDecodedResource = null;
                    flowLayoutPanel1.Controls.Clear();
                    string dump = ModuleResources.DumpRaw(resData, true);
                    flowLayoutPanel1.Controls.Add(GetTextbox(dump));
                    string decodeMessage = "Resource loaded, but preview failed: " + ex.Message;
                    message = string.IsNullOrEmpty(message) ? decodeMessage : message + " " + decodeMessage;
                }
            }

            lbMessage.Text = message.Replace("\n", " ");
            UpdateResourceMenu();
        }

        private bool ShowRawResult(object rawResult)
        {
            if (rawResult == null)
                return false;

            List<Img> images = rawResult as List<Img>;
            if (images != null)
            {
                bool shown = false;
                foreach (Img image in images)
                {
                    if (image != null && image.Bitmap != null)
                    {
                        flowLayoutPanel1.Controls.Add(GetPictureBox(image.Bitmap));
                        shown = true;
                    }
                }
                return shown;
            }

            Img singleImage = rawResult as Img;
            if (singleImage != null && singleImage.Bitmap != null)
            {
                flowLayoutPanel1.Controls.Add(GetPictureBox(singleImage.Bitmap));
                return true;
            }

            DecodedFont decodedFont = rawResult as DecodedFont;
            if (decodedFont != null)
            {
                ShowFontPreview(decodedFont);
                return decodedFont.Glyphs.Count > 0;
            }

            Bitmap bitmap = rawResult as Bitmap;
            if (bitmap != null)
            {
                flowLayoutPanel1.Controls.Add(GetPictureBox(bitmap));
                return true;
            }

            string text = rawResult as string;
            if (text != null)
            {
                flowLayoutPanel1.Controls.Add(GetTextbox(text));
                return true;
            }

            return false;
        }

        private void ClearSelectedResource()
        {
            IDisposable disposable = selectedDecodedResource as IDisposable;
            if (disposable != null)
                disposable.Dispose();

            selectedResourceData = null;
            selectedResourceType = null;
            selectedResourceLabel = null;
            selectedDecodedResource = null;
            selectedOriginalFormat = null;
            selectedConversionExtensions.Clear();
        }

        private void UpdateResourceMenu()
        {
            bool hasContainer = !string.IsNullOrEmpty(currentFilePath) && currentModuleProperties != null;
            mnu_Resource.Enabled = hasContainer;
            mnu_ExportAllResources.Enabled = hasContainer && treeView1.Nodes.Count > 0;

            mnu_ExportConverted.MenuItems.Clear();
            selectedConversionExtensions.Clear();

            if (selectedResourceData == null)
            {
                mnu_ExportOriginal.Enabled = false;
                mnu_ExportOriginal.Text = "Export original";
                mnu_ExportConverted.Enabled = false;
                mnu_ExportConverted.Text = "Export converted";
                return;
            }

            selectedOriginalFormat = ResourceFormatDetector.Detect(selectedResourceType, selectedResourceData);
            mnu_ExportOriginal.Enabled = true;
            mnu_ExportOriginal.Text = "Export original (" + selectedOriginalFormat.Extension + ")";

            selectedConversionExtensions = ResourceConversion.GetAvailableExtensions(selectedDecodedResource);
            mnu_ExportConverted.Enabled = selectedConversionExtensions.Count > 0;

            if (selectedConversionExtensions.Count == 0)
            {
                mnu_ExportConverted.Text = "Export converted";
            }
            else if (selectedConversionExtensions.Count == 1)
            {
                mnu_ExportConverted.Text = "Export converted (" + selectedConversionExtensions[0] + ")";
            }
            else
            {
                mnu_ExportConverted.Text = "Export converted";
                foreach (string extension in selectedConversionExtensions)
                {
                    string capturedExtension = extension;
                    MenuItem item = new MenuItem(extension.TrimStart('.').ToUpperInvariant() + " (" + extension + ")");
                    item.Click += delegate { ExportSelectedConverted(capturedExtension); };
                    mnu_ExportConverted.MenuItems.Add(item);
                }
            }
        }

        private string GetSelectedResourceBaseName()
        {
            string moduleName = Path.GetFileNameWithoutExtension(currentFilePath);
            return ResourceConversion.SanitizeFileName(moduleName + "_" + selectedResourceType + "_" + selectedResourceLabel);
        }

        private void ExportSelectedOriginal()
        {
            if (selectedResourceData == null)
                return;

            ResourceFileFormat format = selectedOriginalFormat ?? ResourceFormatDetector.Detect(selectedResourceType, selectedResourceData);
            string baseName = GetSelectedResourceBaseName();

            using (SaveFileDialog dialog = new SaveFileDialog())
            {
                dialog.Title = "Export original resource";
                dialog.FileName = baseName + format.Extension;
                dialog.Filter = format.Description + " (*" + format.Extension + ")|*" + format.Extension + "|All files (*.*)|*.*";
                dialog.DefaultExt = format.Extension.TrimStart('.');
                dialog.AddExtension = true;

                if (dialog.ShowDialog() == DialogResult.OK)
                {
                    File.WriteAllBytes(dialog.FileName, selectedResourceData);
                    lbMessage.Text = "Exported original resource: " + dialog.FileName;
                }
            }
        }

        private void ExportSelectedConverted(string extension)
        {
            if (selectedDecodedResource == null)
                return;

            List<ConvertedResourceFile> files = ResourceConversion.Convert(selectedDecodedResource, extension, GetSelectedResourceBaseName());
            if (files.Count == 0)
            {
                MessageBox.Show("The selected resource could not be converted.", "Export resource", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            if (files.Count == 1)
            {
                using (SaveFileDialog dialog = new SaveFileDialog())
                {
                    dialog.Title = "Export converted resource";
                    dialog.FileName = files[0].FileName;
                    dialog.Filter = extension.TrimStart('.').ToUpperInvariant() + " file (*" + extension + ")|*" + extension + "|All files (*.*)|*.*";
                    dialog.DefaultExt = extension.TrimStart('.');
                    dialog.AddExtension = true;

                    if (dialog.ShowDialog() == DialogResult.OK)
                    {
                        File.WriteAllBytes(dialog.FileName, files[0].Data);
                        lbMessage.Text = "Exported converted resource: " + dialog.FileName;
                    }
                }
                return;
            }

            using (FolderBrowserDialog dialog = new FolderBrowserDialog())
            {
                dialog.Description = "Select the folder for the converted resource files";
                if (dialog.ShowDialog() != DialogResult.OK)
                    return;

                int exported = 0;
                foreach (ConvertedResourceFile file in files)
                {
                    string path = ResourceConversion.GetUniquePath(Path.Combine(dialog.SelectedPath, file.FileName));
                    File.WriteAllBytes(path, file.Data);
                    exported++;
                }
                lbMessage.Text = "Exported " + exported.ToString() + " converted resource files to " + dialog.SelectedPath;
            }
        }

        private void ExportAllResources()
        {
            if (string.IsNullOrEmpty(currentFilePath) || currentModuleProperties == null)
                return;

            using (FolderBrowserDialog dialog = new FolderBrowserDialog())
            {
                dialog.Description = "Select the destination folder for all original resources";
                if (dialog.ShowDialog() != DialogResult.OK)
                    return;

                string moduleName = ResourceConversion.SanitizeFileName(Path.GetFileNameWithoutExtension(currentFilePath));
                string exportRoot = Path.Combine(dialog.SelectedPath, moduleName + "_resources");
                Directory.CreateDirectory(exportRoot);

                int exported = 0;
                int failed = 0;

                foreach (TreeNode typeNode in treeView1.Nodes)
                {
                    string typeName = typeNode.Text;
                    string typeFolder = Path.Combine(exportRoot, ResourceConversion.SanitizeFileName(typeName));
                    Directory.CreateDirectory(typeFolder);

                    foreach (TreeNode resourceNode in typeNode.Nodes)
                    {
                        string resourceLabel = resourceNode.Text;
                        string targetName = resourceLabel.StartsWith("#") ? resourceLabel.Substring(1) : resourceLabel;
                        string message;
                        bool found;

                        try
                        {
                            byte[] data = ModuleResources.OpenResource(currentModuleProperties, typeName, targetName, out message, out found);
                            if (!found || data == null)
                            {
                                failed++;
                                continue;
                            }

                            ResourceFileFormat format = ResourceFormatDetector.Detect(typeName, data);
                            string fileName = ResourceConversion.SanitizeFileName(resourceLabel) + format.Extension;
                            string path = ResourceConversion.GetUniquePath(Path.Combine(typeFolder, fileName));
                            File.WriteAllBytes(path, data);
                            exported++;
                        }
                        catch (Exception ex)
                        {
                            Console.WriteLine("Failed to export " + typeName + " " + resourceLabel + ": " + ex.Message);
                            failed++;
                        }
                    }
                }

                string summary = "Exported " + exported.ToString() + " original resources to " + exportRoot;
                if (failed > 0)
                    summary += ". Failed: " + failed.ToString();
                lbMessage.Text = summary;
                MessageBox.Show(summary, "Export all resources", MessageBoxButtons.OK,
                    failed == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
            }
        }

        private void ShowFontPreview(DecodedFont font)
        {
            if (font == null)
                return;

            FlowLayoutPanel fontPanel = new FlowLayoutPanel
            {
                AutoSize = true,
                AutoSizeMode = AutoSizeMode.GrowAndShrink,
                FlowDirection = FlowDirection.TopDown,
                WrapContents = false,
                Margin = new Padding(0),
                Padding = new Padding(8)
            };

            int displayedGlyphCount = font.Glyphs.Count > 0
                ? font.Glyphs.Count
                : font.DeclaredGlyphCount;
            string details = (string.IsNullOrEmpty(font.FaceName) ? "Unnamed font" : font.FaceName) +
                " — " + (string.IsNullOrEmpty(font.FormatName) ? "FNT" : font.FormatName) +
                " — " + (font.IsVector ? "vector" : "raster") +
                " — " + displayedGlyphCount.ToString() + " glyphs" +
                " — codes " + font.FirstCharacter.ToString() + "–" + font.LastCharacter.ToString() +
                " — native height " + font.PixelHeight.ToString() + " px";
            if (font.CodePage > 0)
                details += " — CP" + font.CodePage.ToString();
            else if (font.CharacterSet > 0)
                details += " — charset " + font.CharacterSet.ToString();

            fontPanel.Controls.Add(new Label
            {
                AutoSize = true,
                Font = new Font(Font, FontStyle.Bold),
                Text = details,
                Margin = new Padding(3, 3, 3, 10)
            });

            if (!string.IsNullOrEmpty(font.PreviewMessage))
            {
                fontPanel.Controls.Add(new Label
                {
                    AutoSize = true,
                    MaximumSize = new Size(760, 0),
                    Text = font.PreviewMessage,
                    Margin = new Padding(3, 0, 3, 10)
                });
            }

            const string sampleText = "The quick brown fox jumps over the lazy dog";
            int[] scales = new int[] { 1, 2, 3, 4 };
            for (int i = 0; i < scales.Length; i++)
            {
                int scale = scales[i];
                Bitmap sample = RenderFontText(font, sampleText, scale);
                if (sample == null)
                    continue;

                fontPanel.Controls.Add(new Label
                {
                    AutoSize = true,
                    Text = "Sample — " + (font.PixelHeight * scale).ToString() + " px (" + scale.ToString() + "×)",
                    Margin = new Padding(3, 8, 3, 2)
                });
                fontPanel.Controls.Add(GetPictureBox(sample));
            }

            Bitmap glyphMap = RenderGlyphMap(font);
            if (glyphMap != null)
            {
                fontPanel.Controls.Add(new Label
                {
                    AutoSize = true,
                    Text = "Glyph map",
                    Margin = new Padding(3, 12, 3, 2)
                });
                fontPanel.Controls.Add(GetPictureBox(glyphMap));
            }

            flowLayoutPanel1.Controls.Add(fontPanel);
        }

        private Bitmap RenderFontText(DecodedFont font, string text, int scale)
        {
            if (font == null || font.Glyphs == null || font.Glyphs.Count == 0 || string.IsNullOrEmpty(text))
                return null;
            if (scale < 1)
                scale = 1;

            List<PreparedFontGlyph> prepared = new List<PreparedFontGlyph>();
            int penX = 0;
            int minimumX = 0;
            int minimumY = 0;
            int maximumX = 0;
            int maximumY = Math.Max(1, font.LineHeight * scale);

            try
            {
                for (int i = 0; i < text.Length; i++)
                {
                    FontGlyph glyph = ResolveFontGlyph(font, text[i]);
                    if (glyph == null)
                    {
                        penX += Math.Max(1, font.PixelHeight / 2) * scale;
                        maximumX = Math.Max(maximumX, penX);
                        continue;
                    }

                    int offsetX;
                    int offsetY;
                    Bitmap bitmap = RT_FONT.RenderGlyph(glyph, scale, out offsetX, out offsetY);
                    PreparedFontGlyph item = new PreparedFontGlyph
                    {
                        Bitmap = bitmap,
                        CharacterCode = glyph.CharacterCode,
                        DrawX = penX + offsetX,
                        DrawY = offsetY
                    };
                    prepared.Add(item);

                    minimumX = Math.Min(minimumX, item.DrawX);
                    minimumY = Math.Min(minimumY, item.DrawY);
                    maximumX = Math.Max(maximumX, item.DrawX + bitmap.Width);
                    maximumY = Math.Max(maximumY, item.DrawY + bitmap.Height);

                    penX += Math.Max(1, glyph.AdvanceX) * scale;
                    maximumX = Math.Max(maximumX, penX);
                }

                const int margin = 8;
                int width = Math.Max(1, maximumX - minimumX + margin * 2);
                int height = Math.Max(1, maximumY - minimumY + margin * 2);
                Bitmap result = new Bitmap(width, height, PixelFormat.Format32bppArgb);
                using (Graphics graphics = Graphics.FromImage(result))
                {
                    graphics.Clear(Color.White);
                    graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceOver;
                    for (int i = 0; i < prepared.Count; i++)
                    {
                        PreparedFontGlyph item = prepared[i];
                        graphics.DrawImageUnscaled(
                            item.Bitmap,
                            margin + item.DrawX - minimumX,
                            margin + item.DrawY - minimumY);
                    }
                }

                return result;
            }
            finally
            {
                for (int i = 0; i < prepared.Count; i++)
                    prepared[i].Dispose();
            }
        }

        private FontGlyph ResolveFontGlyph(DecodedFont font, char value)
        {
            FontGlyph glyph = font.FindGlyph((int)value);
            if (glyph != null)
                return glyph;

            glyph = font.FindGlyph(font.DefaultCharacter);
            if (glyph != null)
                return glyph;

            glyph = font.FindGlyph((int)'?');
            if (glyph != null)
                return glyph;

            return font.FindGlyph((int)' ');
        }

        private Bitmap RenderGlyphMap(DecodedFont font)
        {
            if (font == null || font.Glyphs == null || font.Glyphs.Count == 0)
                return null;

            int scale = font.IsVector ? 1 : (font.PixelHeight <= 16 ? 2 : 1);
            int columns = 16;
            List<PreparedFontGlyph> prepared = new List<PreparedFontGlyph>();
            List<int> widths = new List<int>();
            List<int> heights = new List<int>();

            try
            {
                for (int i = 0; i < font.Glyphs.Count; i++)
                {
                    FontGlyph glyph = font.Glyphs[i];
                    if (glyph == null)
                    {
                        prepared.Add(null);
                        continue;
                    }

                    int offsetX;
                    int offsetY;
                    Bitmap bitmap = RT_FONT.RenderGlyph(glyph, scale, out offsetX, out offsetY);
                    prepared.Add(new PreparedFontGlyph
                    {
                        Bitmap = bitmap,
                        CharacterCode = glyph.CharacterCode
                    });

                    if (bitmap.Width > 0)
                        widths.Add(bitmap.Width);
                    if (bitmap.Height > 0)
                        heights.Add(bitmap.Height);
                }

                int typicalWidth = GetRobustGlyphDimension(widths, Math.Max(8, font.PixelHeight * scale));
                int typicalHeight = GetRobustGlyphDimension(heights, Math.Max(8, font.LineHeight * scale));

                int imageAreaWidth = Math.Max(
                    typicalWidth,
                    Math.Max(8, Math.Min(font.PixelHeight * 2 * scale, 96)));
                int imageAreaHeight = Math.Max(
                    typicalHeight,
                    Math.Max(8, Math.Min(font.LineHeight * scale, 96)));
                imageAreaWidth = Math.Min(imageAreaWidth, 96);
                imageAreaHeight = Math.Min(imageAreaHeight, 96);

                int cellWidth = imageAreaWidth + 12;
                int cellHeight = imageAreaHeight + 18;
                int rows = (font.Glyphs.Count + columns - 1) / columns;
                Bitmap result = new Bitmap(
                    Math.Max(1, columns * cellWidth + 1),
                    Math.Max(1, rows * cellHeight + 1),
                    PixelFormat.Format32bppArgb);

                using (Graphics graphics = Graphics.FromImage(result))
                using (Pen gridPen = new Pen(Color.LightGray))
                using (Font codeFont = new Font(FontFamily.GenericMonospace, 7, FontStyle.Regular, GraphicsUnit.Point))
                {
                    graphics.Clear(Color.White);
                    graphics.InterpolationMode = font.IsVector
                        ? System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic
                        : System.Drawing.Drawing2D.InterpolationMode.NearestNeighbor;
                    graphics.PixelOffsetMode = font.IsVector
                        ? System.Drawing.Drawing2D.PixelOffsetMode.HighQuality
                        : System.Drawing.Drawing2D.PixelOffsetMode.Half;

                    for (int i = 0; i < font.Glyphs.Count; i++)
                    {
                        int column = i % columns;
                        int row = i / columns;
                        int cellX = column * cellWidth;
                        int cellY = row * cellHeight;
                        graphics.DrawRectangle(gridPen, cellX, cellY, cellWidth, cellHeight);

                        PreparedFontGlyph item = prepared[i];
                        FontGlyph glyph = font.Glyphs[i];
                        if (item != null && item.Bitmap != null)
                        {
                            int targetWidth = item.Bitmap.Width;
                            int targetHeight = item.Bitmap.Height;
                            if (targetWidth > imageAreaWidth || targetHeight > imageAreaHeight)
                            {
                                double fit = Math.Min(
                                    (double)imageAreaWidth / Math.Max(1, targetWidth),
                                    (double)imageAreaHeight / Math.Max(1, targetHeight));
                                targetWidth = Math.Max(1, (int)Math.Floor(targetWidth * fit));
                                targetHeight = Math.Max(1, (int)Math.Floor(targetHeight * fit));
                            }

                            int imageX = cellX + 6 + Math.Max(0, (imageAreaWidth - targetWidth) / 2);
                            int imageY = cellY + 2 + Math.Max(0, (imageAreaHeight - targetHeight) / 2);
                            graphics.DrawImage(item.Bitmap,
                                new Rectangle(imageX, imageY, targetWidth, targetHeight),
                                0, 0, item.Bitmap.Width, item.Bitmap.Height,
                                GraphicsUnit.Pixel);
                        }

                        if (glyph != null)
                        {
                            string code = glyph.CharacterCode <= 0xFF
                                ? glyph.CharacterCode.ToString("X2")
                                : glyph.CharacterCode.ToString("X4");
                            graphics.DrawString(code, codeFont, Brushes.DimGray,
                                cellX + 2, cellY + imageAreaHeight + 3);
                        }
                    }
                }

                return result;
            }
            finally
            {
                for (int i = 0; i < prepared.Count; i++)
                {
                    if (prepared[i] != null)
                        prepared[i].Dispose();
                }
            }
        }

        private static int GetRobustGlyphDimension(List<int> values, int fallback)
        {
            if (values == null || values.Count == 0)
                return Math.Max(1, fallback);

            values.Sort();
            int percentileIndex = (int)Math.Floor((values.Count - 1) * 0.90);
            if (percentileIndex < 0)
                percentileIndex = 0;
            if (percentileIndex >= values.Count)
                percentileIndex = values.Count - 1;
            return Math.Max(1, values[percentileIndex]);
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

                case var _ when sender == mnu_ExportOriginal:
                    ExportSelectedOriginal();
                    break;

                case var _ when sender == mnu_ExportConverted:
                    if (selectedConversionExtensions.Count == 1)
                        ExportSelectedConverted(selectedConversionExtensions[0]);
                    break;

                case var _ when sender == mnu_ExportAllResources:
                    ExportAllResources();
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

            // Default icons. Can be changed in the settings.ini file.
            // In a future update also in a settings section of the GUI.
            WriteIcoSection("RT_FOLDER_OPEN", shell32, "4", "folder_open"); 
            WriteIcoSection("RT_FOLDER_CLOSE", shell32, "5", "folder_close");
            WriteIcoSection("RT_DEFAULT", shell32, "1", "default_unknown_icon");
            WriteIcoSection("RT_FONT", shell32, "155", "fon");
            WriteIcoSection("RT_FONTDIR", shell32, "155", "fon");
            WriteIcoSection("RT_VERSION", shell32, "151", "ini");
            WriteIcoSection("RT_MENU", shell32, "151", "ini");
            WriteIcoSection("RT_STRING", shell32, "151", "ini");
            WriteIcoSection("RT_BITMAP", shell32, "16823", "bmp");
            WriteIcoSection("RT_POINTER", shell32, "16823", "bmp");
            WriteIcoSection("RT_ICON", shell32, "16823", "bmp");
            WriteIcoSection("RT_GROUP_ICON", shell32, "16823", "bmp");
            WriteIcoSection("RT_CURSOR", shell32, "16823", "bmp");
            WriteIcoSection("RT_GROUP_CURSOR", shell32, "16823", "bmp");
            WriteIcoSection("RT_MANIFEST", mmcndmgr, "1098", "xml");

            List<Img> imgs = new List<Img>();
            Bitmap bmp = null;
            Dictionary<string, ModuleResources.ModuleProperties> propertiesArray = new Dictionary<string, ModuleResources.ModuleProperties>();
            foreach (string section in settings.GetSections())
            {
                if (!string.IsNullOrEmpty(section))
                {
                    string path = settings.Read("path", section);
                    string ordinal = settings.Read("ordinal", section);
                    string iconExt = settings.Read("iconExt", section);

                    if (!propertiesArray.ContainsKey(path))
                    {
                        propertiesArray[path] = ModuleResources.GetModuleProperties(path);
                    }
                    var properties = propertiesArray[path];
                    if ((properties.headerType == ModuleResources.HeaderType.LE && properties.versionType == ModuleResources.VersionType.OS2) ||
                        (properties.headerType == ModuleResources.HeaderType.NE && properties.versionType == ModuleResources.VersionType.OS2) ||
                        properties.headerType == ModuleResources.HeaderType.LX)
                    {
                        imgs = RT_POINTER.Get(ModuleResources.OpenResource(properties, "RT_POINTER", ordinal, out _, out _));
                    }
                    else
                    {
                        RT_GROUP_ICON.Get(ModuleResources.OpenResource(properties, "RT_GROUP_ICON", ordinal, out _, out _), properties, out imgs);
                    }
                    if (imgs.Count > 0)
                    {
                        List<Img> imgsFiltered = imgs.Where(img => img.Size.Width == 16 && img.Size.Height == 16).ToList();
                        int maxBitCount = imgsFiltered.Max(img => img.BitCount);
                        imageList1.Images.Add(section, imgsFiltered.Where(img => img.BitCount >= maxBitCount).Last().Bitmap);
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
