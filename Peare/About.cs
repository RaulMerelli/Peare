using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Diagnostics;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace Peare
{
    public partial class About : Form
    {
        public About()
        {
            InitializeComponent();
        }

        private void linkLabel1_LinkClicked(object sender, LinkLabelLinkClickedEventArgs e)
        {
            Process.Start("https://github.com/RaulMerelli/Peare");
        }

        private void About_Load(object sender, EventArgs e)
        {
            IntPtr backupPointer = Program.currentModuleHandle;
            try
            {
                // handle to current exe
                Program.currentModuleHandle = PeResources.LoadLibraryEx(System.Reflection.Assembly.GetEntryAssembly().Location, IntPtr.Zero, LoadLibraryFlags.LOAD_LIBRARY_AS_DATAFILE);
                // load the image from the our executable using our functions!
                pictureBox1.Image = RT_ICON.Get(PeResources.OpenResourcePE("RT_ICON", "2", out _, out _));
            }
            catch 
            {

            }

            Program.currentModuleHandle = backupPointer;
        }
    }
}
